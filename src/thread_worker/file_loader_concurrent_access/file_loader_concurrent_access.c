/**
 * @file file_loader_concurrent_access.c
 * @brief FileLoaderService — lock-free concurrent file registry and loader.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/file_loader_service_concurrent/file_loader_concurrent_service.h>

#include <uflib/standard_c_includes.h>
#include <uflib/standard_defs.h>
#include <uflib/config_defs_uflib.h>
#include <uflib/cdt/hashmap/cdt_fixed_width_hashmap.h>
#include <uflib/cdt/cdt_mpsc_queue.h>

#include "file_loader_service_type_priv.h"
#include "file_loader_service_defs_priv.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* ── Cast helper ──────────────────────────────────────────────────────── */

#define SELF(svc)  ((FileLoaderConcurrentAccess *)(svc))

/* ── Forward declarations ─────────────────────────────────────────────── */

static FileLoaderConcurrentAccessNode *sNodeAt(FileLoaderConcurrentAccess *, uint32_t);
static size_t  sNodeSize(uint32_t);
static void    sResolveConfig(FileLoaderConcurrentAccess *, const FileLoaderServiceConcurrent *);
static uint32_t sNextPow2(uint32_t);
static uint32_t sNodeAllocate(FileLoaderConcurrentAccess *);
static void    sNodeFree(FileLoaderConcurrentAccess *, uint32_t);
__attribute__((unused)) static uint32_t sFreeCount(FileLoaderConcurrentAccess *);
static uint32_t sFindVictim(FileLoaderConcurrentAccess *);
static void    sRemoveFileImpl(FileLoaderConcurrentAccess *, const char *);
static FileLoaderServiceHandle sMakeHandle(uint32_t, uint64_t);
static uint32_t sHandleToIdx(FileLoaderServiceHandle);
static uint64_t sHandleToGen(FileLoaderServiceHandle);
static bool    sLoadFile(FileLoaderConcurrentAccess *, uint32_t);
static void    sUnloadBuffer(FileLoaderConcurrentAccess *, uint32_t);
static void    sClockEvictBuffer(FileLoaderConcurrentAccess *);
static int     sHazptrAcquire(FileLoaderConcurrentAccess *);
static void    sHazptrRelease(FileLoaderConcurrentAccess *, int);
static bool    sIsHazardous(FileLoaderConcurrentAccess *, uintptr_t);
static void    sLimboRetire(FileLoaderConcurrentAccess *, void *, size_t);
static void    sLimboDrain(FileLoaderConcurrentAccess *);
static void   *sMonitorThread(void *);

/* ── Handle encoding ──────────────────────────────────────────────────── */

static inline FileLoaderServiceHandle
sMakeHandle(uint32_t poolIdx, uint64_t gen)
{
    return ((uint64_t)poolIdx << FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT)
           | (gen & FILELOADERCONCURRENTACCESS_HANDLE_GEN_MASK);
}
static inline uint32_t sHandleToIdx(FileLoaderServiceHandle h) {
    return (uint32_t)(h >> FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT);
}
static inline uint64_t sHandleToGen(FileLoaderServiceHandle h) {
    return h & FILELOADERCONCURRENTACCESS_HANDLE_GEN_MASK;
}

/* ── Node access/sizing ───────────────────────────────────────────────── */

static inline FileLoaderConcurrentAccessNode *
sNodeAt(FileLoaderConcurrentAccess *self, uint32_t poolIdx) {
    return (FileLoaderConcurrentAccessNode *)(self->node_pool
           + (size_t)poolIdx * self->node_stride);
}
static size_t sNodeSize(uint32_t kw) {
    return ((sizeof(FileLoaderConcurrentAccessNode) + (size_t)kw) + 63U) & ~((size_t)63U);
}
static uint32_t sNextPow2(uint32_t v) {
    if (v == 0) return 1; v--; v |= v>>1; v |= v>>2; v |= v>>4; v |= v>>8; v |= v>>16; return v+1;
}

/* ── Config resolution ────────────────────────────────────────────────── */

static void
sResolveConfig(FileLoaderConcurrentAccess *self, const FileLoaderServiceConcurrent *config)
{
    if (!config) {
        self->pool_capacity = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY;
        self->buffer_cache_capacity = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ;
        self->key_width = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH;
        self->max_file_size_bytes = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES;
        self->hazptr_slot_count = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS;
        self->limbo_capacity = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ;
        self->default_buffer_timeout_ms = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS;
        self->on_evicted = NULL; self->on_evicted_ctx = NULL;
        return;
    }
    self->pool_capacity = config->service_config_params.registry_size
        ? config->service_config_params.registry_size : CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY;
    self->buffer_cache_capacity = config->service_config_params.loaded_size
        ? config->service_config_params.loaded_size : CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ;
    self->key_width = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH;
    self->max_file_size_bytes = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES;
    self->hazptr_slot_count = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS;
    self->limbo_capacity = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ;
    self->default_buffer_timeout_ms = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS;
    self->on_evicted = NULL; self->on_evicted_ctx = NULL;
}

/* ── Node pool ────────────────────────────────────────────────────────── */

static inline uintptr_t sPackFreeNode(uint32_t idx, uint32_t pop) { return ((uintptr_t)pop<<32)|(uintptr_t)idx; }
static inline uint32_t sUnpackFreeIdx(uintptr_t p) { return (uint32_t)(p&0xFFFFFFFFU); }
static inline uint32_t sUnpackPopCount(uintptr_t p) { return (uint32_t)(p>>32); }

static uint32_t
sNodeAllocate(FileLoaderConcurrentAccess *self)
{
    uintptr_t head = atomic_load_explicit(&self->node_free_stack_head, memory_order_acquire);
    while (head != 0) {
        uint32_t idx = sUnpackFreeIdx(head), pop = sUnpackPopCount(head);
        uint32_t next = sNodeAt(self, idx)->next_free;
        uintptr_t newHead = sPackFreeNode(next, pop + 1);
        if (atomic_compare_exchange_strong_explicit(&self->node_free_stack_head, &head, newHead,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&sNodeAt(self, idx)->is_active, true, memory_order_release);
            return idx;
        }
    }
    return 0;
}

static void
sNodeFree(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
    if (poolIdx == 0) return;
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
    atomic_store_explicit(&node->is_active, false, memory_order_release);
    uintptr_t oldHead, newHead;
    do {
        oldHead = atomic_load_explicit(&self->node_free_stack_head, memory_order_acquire);
        node->next_free = sUnpackFreeIdx(oldHead);
        newHead = sPackFreeNode(poolIdx, sUnpackPopCount(oldHead) + 1);
    } while (!atomic_compare_exchange_strong_explicit(&self->node_free_stack_head, &oldHead,
               newHead, memory_order_acq_rel, memory_order_relaxed));
}

__attribute__((unused)) static uint32_t
sFreeCount(FileLoaderConcurrentAccess *self) {
    uint32_t c=0; uintptr_t h=atomic_load_explicit(&self->node_free_stack_head, memory_order_relaxed);
    while(h){c++;h=(uintptr_t)sNodeAt(self,sUnpackFreeIdx(h))->next_free;} return c;
}

/* ── File loading ─────────────────────────────────────────────────────── */

static bool
sLoadFile(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
    struct stat st;
    if (stat(node->path, &st) != 0) goto error;
    if ((size_t)st.st_size > self->max_file_size_bytes) {
        atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_TOO_LARGE, memory_order_release);
        return false;
    }
    if (!(st.st_mode & S_IRUSR)) goto error;
    if (st.st_size == 0) {
        atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE, memory_order_release);
        return true;
    }
    int fd = open(node->path, O_RDONLY);
    if (fd < 0) goto error;
    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) goto error;
    madvise(data, (size_t)st.st_size, MADV_SEQUENTIAL);
    mprotect(data, (size_t)st.st_size, PROT_READ);

    atomic_thread_fence(memory_order_release);
    uintptr_t oldBuf = atomic_exchange_explicit(&node->buffer, (uintptr_t)data, memory_order_acq_rel);
    if (oldBuf) {
        sLimboRetire(self, (void *)oldBuf, 0);
        atomic_fetch_add_explicit(&node->generation, 1, memory_order_release);
    }
    atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE, memory_order_release);
    atomic_fetch_and_explicit(&node->flags, ~(FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD|FILELOADERCONCURRENTACCESS_FLAG_LOADING), memory_order_release);
    return true;
error:
    atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_LOAD_ERROR, memory_order_release);
    return false;
}

/* ── Eviction ─────────────────────────────────────────────────────────── */

static uint32_t
sFindVictim(FileLoaderConcurrentAccess *self)
{
    if (!self->on_evicted) return 0;
    uint32_t start = atomic_fetch_add_explicit(&self->registry_clock_hand, 1, memory_order_relaxed);
    for (uint32_t i=0; i<self->pool_capacity*2; i++) {
        uint32_t idx = (start+i)%self->pool_capacity;
        if (idx==0) continue;
        FileLoaderConcurrentAccessNode *node = sNodeAt(self, idx);
        if (!atomic_load_explicit(&node->is_active, memory_order_acquire)) continue;
        if (atomic_load_explicit(&node->reg_ref_bit, memory_order_relaxed)==1) {
            atomic_store_explicit(&node->reg_ref_bit,0,memory_order_relaxed); continue;
        }
        atomic_store_explicit(&self->registry_clock_hand,(idx+1)%self->pool_capacity,memory_order_relaxed);
        return idx;
    }
    return 0;
}

/* ── Buffer cache ─────────────────────────────────────────────────────── */

static void
sUnloadBuffer(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
    uintptr_t oldBuf = atomic_exchange_explicit(&node->buffer, 0, memory_order_acq_rel);
    if (oldBuf) sLimboRetire(self, (void *)oldBuf, 0);
    atomic_fetch_and_explicit(&node->flags, ~FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE, memory_order_release);
    atomic_fetch_sub_explicit(&self->cache_loaded_count, 1, memory_order_relaxed);
}

static void
sClockEvictBuffer(FileLoaderConcurrentAccess *self)
{
    uint32_t hand = atomic_load_explicit(&self->cache_clock_hand, memory_order_relaxed);
    for (uint32_t i=0; i<self->buffer_cache_capacity*2; i++) {
        uintptr_t packed = atomic_load_explicit(&self->buffer_cache[hand].slot, memory_order_acquire);
        uint32_t poolIdx = (uint32_t)(packed>>32); uint64_t gen = packed&0xFFFFFFFFU;
        if (poolIdx!=0) {
            FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
            if (atomic_load_explicit(&node->generation,memory_order_acquire)!=gen)
                atomic_store_explicit(&self->buffer_cache[hand].slot,0,memory_order_release);
            else if (atomic_load_explicit(&node->cache_ref_bit,memory_order_relaxed)==1)
                atomic_store_explicit(&node->cache_ref_bit,0,memory_order_relaxed);
            else {
                uintptr_t expected=packed;
                if(atomic_compare_exchange_strong_explicit(&self->buffer_cache[hand].slot,&expected,0,memory_order_acq_rel,memory_order_relaxed))
                    { sUnloadBuffer(self,poolIdx); return; }
            }
        }
        hand=(hand+1)%self->buffer_cache_capacity;
        atomic_store_explicit(&self->cache_clock_hand,hand,memory_order_relaxed);
    }
}

/* ── Hazard pointers ──────────────────────────────────────────────────── */

static pthread_key_t sHazptrKey; static pthread_once_t sHazptrKeyOnce=PTHREAD_ONCE_INIT;
static void sMakeHazptrKey(void){pthread_key_create(&sHazptrKey,NULL);}
struct sHazptrThreadState { FileLoaderConcurrentAccess *owner; int slot; };

static int
sHazptrAcquire(FileLoaderConcurrentAccess *self)
{
    pthread_once(&sHazptrKeyOnce,sMakeHazptrKey);
    struct sHazptrThreadState *st = (struct sHazptrThreadState *)pthread_getspecific(sHazptrKey);
    if (st && st->owner==self) return st->slot;
    for (uint32_t i=0; i<self->hazptr_slot_count; i++) {
        uintptr_t expected=0;
        if (atomic_compare_exchange_strong(&self->hazptrs[i],&expected,(uintptr_t)1)) {
            atomic_store_explicit(&self->hazptrs[i],0,memory_order_relaxed);
            if(!st){st=(struct sHazptrThreadState *)calloc(1,sizeof(*st));pthread_setspecific(sHazptrKey,st);}
            st->owner=self; st->slot=(int)i; return (int)i;
        }
    }
    return -1;
}
static void sHazptrRelease(FileLoaderConcurrentAccess *self,int slot) {
    if(slot<0||(uint32_t)slot>=self->hazptr_slot_count)return;
    atomic_store_explicit(&self->hazptrs[slot],0,memory_order_release);
}
static bool sIsHazardous(FileLoaderConcurrentAccess *self,uintptr_t bufPtr) {
    for(uint32_t i=0;i<self->hazptr_slot_count;i++)
        if(atomic_load_explicit(&self->hazptrs[i],memory_order_acquire)==bufPtr) return true;
    return false;
}

/* ── Limbo ring ───────────────────────────────────────────────────────── */

static void
sLimboRetire(FileLoaderConcurrentAccess *self, void *buf, size_t size)
{
    uint32_t wIdx=atomic_load_explicit(&self->limbo_write_idx,memory_order_relaxed);
    uint32_t rIdx=atomic_load_explicit(&self->limbo_read_idx,memory_order_acquire);
    uint32_t nextW=(wIdx+1)%self->limbo_capacity;
    if(nextW==rIdx){munmap(buf,size);return;}
    self->limbo_ring[wIdx].ptr=(uintptr_t)buf; self->limbo_ring[wIdx].size=size;
    atomic_store_explicit(&self->limbo_write_idx,nextW,memory_order_release);
}

static void
sLimboDrain(FileLoaderConcurrentAccess *self)
{
    uint32_t rIdx=atomic_load_explicit(&self->limbo_read_idx,memory_order_relaxed);
    uint32_t wIdx=atomic_load_explicit(&self->limbo_write_idx,memory_order_acquire);
    while(rIdx!=wIdx){
        uintptr_t bufPtr=self->limbo_ring[rIdx].ptr; size_t size=self->limbo_ring[rIdx].size;
        if(bufPtr&&sIsHazardous(self,bufPtr))break;
        if(bufPtr)munmap((void*)bufPtr,size);
        rIdx=(rIdx+1)%self->limbo_capacity;
        atomic_store_explicit(&self->limbo_read_idx,rIdx,memory_order_release);
    }
}

/* ── Monitor thread ───────────────────────────────────────────────────── */

static void *
sMonitorThread(void *arg)
{
    FileLoaderConcurrentAccess *self = (FileLoaderConcurrentAccess *)arg;
    struct epoll_event events[32];
    while (atomic_load_explicit(&self->running, memory_order_acquire)) {
        int nfds=epoll_wait(self->epoll_fd,events,32,(int)CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MONITOR_TIMEOUT_MS);
        (void)nfds;
        for (uint32_t i=1; i<self->pool_capacity; i++) {
            FileLoaderConcurrentAccessNode *node=sNodeAt(self,i);
            if(!atomic_load_explicit(&node->is_active,memory_order_acquire))continue;
            uint32_t flags=atomic_load_explicit(&node->flags,memory_order_acquire);
            if(!(flags&FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD))continue;
            if(flags&FILELOADERCONCURRENTACCESS_FLAG_LOADING)continue;
            uint32_t expected=FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD;
            if(!atomic_compare_exchange_strong_explicit(&node->flags,&expected,FILELOADERCONCURRENTACCESS_FLAG_LOADING,memory_order_acq_rel,memory_order_relaxed))continue;
            if(atomic_load_explicit(&self->cache_loaded_count,memory_order_relaxed)>=self->buffer_cache_capacity)
                sClockEvictBuffer(self);
            if(!(expected&FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE)){
                for(uint32_t c=0;c<self->buffer_cache_capacity;c++){uintptr_t empty=0;
                    if(atomic_compare_exchange_strong_explicit(&self->buffer_cache[c].slot,&empty,(uintptr_t)i,memory_order_acq_rel,memory_order_relaxed)){
                        atomic_fetch_add_explicit(&self->cache_loaded_count,1,memory_order_relaxed);
                        atomic_fetch_or_explicit(&node->flags,FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE,memory_order_release);break;}}
            }
            sLoadFile(self,i);
        }
        sLimboDrain(self);
        atomic_fetch_add_explicit(&self->monitor_heartbeat,1,memory_order_relaxed);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

PUBLIC_API bool
FileLoaderServiceInit(FileLoaderServiceConcurrent *config)
{
    if (!config) return false;
    FileLoaderConcurrentAccess *self = (FileLoaderConcurrentAccess *)calloc(1, sizeof(*self));
    if (!self) return false;
    sResolveConfig(self, config);

    uint32_t fileMapSlots = sNextPow2((uint32_t)((uint64_t)self->pool_capacity*100U
        /CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR));
    if (!LocklessFixedWidthHashMap_Init(&self->file_map, fileMapSlots,
            CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR, self->key_width)) goto fail;
    if (!LocklessFixedWidthHashMap_Init(&self->blacklist_map,
            sNextPow2(CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BLACKLIST_CAPACITY),
            CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR, self->key_width)) goto fail;

    self->node_stride = sNodeSize(self->key_width);
    self->node_pool = (uint8_t *)calloc(self->pool_capacity, self->node_stride);
    if (!self->node_pool) goto fail;
    atomic_init(&self->node_free_stack_head, 0);
    for (uint32_t i = self->pool_capacity-1; i>0; i--) {
        FileLoaderConcurrentAccessNode *node = sNodeAt(self, i);
        node->next_free = sUnpackFreeIdx(atomic_load_explicit(&self->node_free_stack_head, memory_order_relaxed));
        atomic_init(&node->generation, 1); atomic_init(&node->is_active, false);
        atomic_store_explicit(&self->node_free_stack_head,
            sPackFreeNode(i, sUnpackPopCount(atomic_load_explicit(&self->node_free_stack_head,memory_order_relaxed))+1),
            memory_order_release);
    }
    self->buffer_cache = (FileLoaderConcurrentAccessCacheSlot *)calloc(self->buffer_cache_capacity, sizeof(FileLoaderConcurrentAccessCacheSlot));
    if (!self->buffer_cache) goto fail;
    self->hazptrs = (_Atomic uintptr_t *)calloc(self->hazptr_slot_count, sizeof(_Atomic uintptr_t));
    if (!self->hazptrs) goto fail;
    self->limbo_ring = (FileLoaderConcurrentAccessLimboEntry *)calloc(self->limbo_capacity, sizeof(FileLoaderConcurrentAccessLimboEntry));
    if (!self->limbo_ring) goto fail;
    mpsc_queue_init(&self->work_queue);

    /* VTable retained for internal use (not exposed to consumers) */
    memset(&self->vtable, 0, sizeof(self->vtable));

    self->inotify_fd = inotify_init1(IN_NONBLOCK);
    if (self->inotify_fd < 0) goto fail;
    self->epoll_fd = epoll_create1(0);
    if (self->epoll_fd < 0) goto fail;
    { struct epoll_event ev; ev.events=EPOLLIN; ev.data.fd=self->inotify_fd;
      if (epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, self->inotify_fd, &ev)<0) goto fail; }

    if (config->service_config_params.blacklist_entries && config->service_config_params.blacklist_entry_count>0) {
        for (uint32_t i=0; i<config->service_config_params.blacklist_entry_count; i++) {
            const char *blPath = config->service_config_params.blacklist_entries[i];
            if (!blPath) continue; bool isNewBl=false;
            LocklessFixedWidthHashMap_Insert(&self->blacklist_map, blPath, 1, &isNewBl);
        }
    }
    atomic_store_explicit(&self->running, true, memory_order_relaxed);
    atomic_store_explicit(&self->monitor_heartbeat, 0, memory_order_relaxed);
    { int rc=pthread_create(&self->monitor_thread,NULL,sMonitorThread,self);
      if(rc!=0){syslog(LOG_ERR,"%s: pthread_create failed: %s",__func__,strerror(rc));goto fail;} }
    config->service_handle = (FileLoaderService *)self;
    return true;
fail:
    FileLoaderServiceDestroy((FileLoaderService *)self);
    return false;
}

PUBLIC_API void
FileLoaderServiceDestroy(FileLoaderService *service_handle)
{
    if (!service_handle) return;
    FileLoaderConcurrentAccess *self = SELF(service_handle);
    if (self->monitor_thread) { atomic_store_explicit(&self->running,false,memory_order_release);
        pthread_join(self->monitor_thread,NULL); self->monitor_thread=0; }
    if (self->limbo_ring) { for(uint32_t i=0;i<self->limbo_capacity;i++)
        {if(self->limbo_ring[i].ptr)munmap((void*)self->limbo_ring[i].ptr,self->limbo_ring[i].size);}
        free(self->limbo_ring); }
    if (self->node_pool) { for(uint32_t i=1;i<self->pool_capacity;i++) {
        uintptr_t buf=atomic_load_explicit(&sNodeAt(self,i)->buffer,memory_order_relaxed);
        if(buf)munmap((void*)buf,0); } free(self->node_pool); }
    free((void*)self->hazptrs); free(self->buffer_cache);
    if(self->epoll_fd>=0)close(self->epoll_fd); if(self->inotify_fd>=0)close(self->inotify_fd);
    LocklessFixedWidthHashMap_Destroy(&self->blacklist_map);
    LocklessFixedWidthHashMap_Destroy(&self->file_map);
    free(self);
}

PUBLIC_API FileLoaderServiceHandle
FileLoaderServiceAddFile(FileLoaderService *service_handle, const char *path)
{
    FileLoaderConcurrentAccess *self = SELF(service_handle);
    if (!self || !path || !*path) return 0;
    { uint32_t d=0; if(LocklessFixedWidthHashMap_Get(&self->blacklist_map,path,&d)) return 0; }
    bool isNew=false; uint32_t allocIdx=sNodeAllocate(self);
    while(allocIdx==0 && self->on_evicted!=NULL) { uint32_t v=sFindVictim(self);
        if(v==0)break; sRemoveFileImpl(self,sNodeAt(self,v)->path); allocIdx=sNodeAllocate(self); }
    if(allocIdx==0)return 0;
    FileLoaderConcurrentAccessNode *node=sNodeAt(self,allocIdx);
    uint64_t oldGen=atomic_load_explicit(&node->generation,memory_order_relaxed);
    uint32_t savedNext=node->next_free; memset(node,0,self->node_stride);
    atomic_store_explicit(&node->generation,oldGen,memory_order_relaxed);
    node->next_free=savedNext;
    strncpy(node->path,path,self->key_width-1); node->path[self->key_width-1]='\0';
    atomic_store_explicit(&node->state,FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE,memory_order_release);
    atomic_store_explicit(&node->flags,FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD,memory_order_release);
    atomic_store_explicit(&node->is_active,true,memory_order_release);
    uint64_t gen=atomic_fetch_add_explicit(&node->generation,1,memory_order_acq_rel)+1;
    if(!LocklessFixedWidthHashMap_Insert(&self->file_map,path,allocIdx,&isNew)){
        sNodeFree(self,allocIdx);
        /* Key already exists — fetch existing mapping and return fresh handle */
        uint32_t existingIdx=0;
        if(LocklessFixedWidthHashMap_Get(&self->file_map,path,&existingIdx) && existingIdx>0)
            return sMakeHandle(existingIdx,
                atomic_load_explicit(&sNodeAt(self,existingIdx)->generation,memory_order_acquire));
        return 0;
    }
    if(!isNew){sNodeFree(self,allocIdx);
        /* isNew=false path: Insert succeeded but key existed.  Fetch real index. */
        uint32_t existingIdx=0;
        if(LocklessFixedWidthHashMap_Get(&self->file_map,path,&existingIdx) && existingIdx>0){
            FileLoaderConcurrentAccessNode *ex=sNodeAt(self,existingIdx);
            atomic_store_explicit(&ex->reg_ref_bit,1,memory_order_relaxed);
            gen=atomic_load_explicit(&ex->generation,memory_order_acquire);
            return sMakeHandle(existingIdx,gen);
        }
        return 0;
    }
    node->inotify_wd=inotify_add_watch(self->inotify_fd,path,IN_MODIFY|IN_DELETE_SELF|IN_MOVE_SELF|IN_ATTRIB);
    return sMakeHandle(allocIdx,gen);
}

static void
sRemoveFileImpl(FileLoaderConcurrentAccess *self, const char *path)
{
    uint32_t poolIdx=0;
    if(!LocklessFixedWidthHashMap_Remove(&self->file_map,path,&poolIdx))return;
    FileLoaderConcurrentAccessNode *node=sNodeAt(self,poolIdx);
    atomic_store_explicit(&node->state,FILELOADERCONCURRENTACCESS_FILE_STATE_DELETED,memory_order_release);
    if(node->inotify_wd>=0){inotify_rm_watch(self->inotify_fd,node->inotify_wd);node->inotify_wd=-1;}
    uintptr_t oldBuf=atomic_exchange_explicit(&node->buffer,0,memory_order_acq_rel);
    if(oldBuf)sLimboRetire(self,(void*)oldBuf,0);
    atomic_fetch_add_explicit(&node->generation,1,memory_order_release);
    sNodeFree(self,poolIdx);
}

PUBLIC_API void
FileLoaderServiceRemoveFile(FileLoaderService *service_handle, const char *path)
{ FileLoaderConcurrentAccess *self=SELF(service_handle); if(self&&path)sRemoveFileImpl(self,path); }

PUBLIC_API bool
FileLoaderServiceGetFileInfo(FileLoaderService *service_handle, const char *path,
                              FileLoaderServiceFileInfo *out)
{
    FileLoaderConcurrentAccess *self=SELF(service_handle);
    if(!self||!out)return false; memset(out,0,sizeof(*out)); if(!path)return false;
    strncpy(out->path,path,sizeof(out->path)-1);
    {uint32_t d=0;if(LocklessFixedWidthHashMap_Get(&self->blacklist_map,path,&d))
        {out->state=FILE_LOADER_SERVICE_FILE_STATE_BLACKLISTED;return true;}}
    uint32_t poolIdx=0;
    if(!LocklessFixedWidthHashMap_Get(&self->file_map,path,&poolIdx))return true;
    FileLoaderConcurrentAccessNode *node=sNodeAt(self,poolIdx);
    if(!atomic_load_explicit(&node->is_active,memory_order_acquire))return true;
    out->in_registry=true;
    out->state=(FileLoaderServiceFileState)atomic_load_explicit(&node->state,memory_order_acquire);
    out->recently_accessed=atomic_load_explicit(&node->reg_ref_bit,memory_order_relaxed)==1;
    out->is_loaded=atomic_load_explicit(&node->buffer,memory_order_acquire)!=0;
    return true;
}

PUBLIC_API void *
FileLoaderServiceGetFileContent(FileLoaderService *service_handle,
                                 FileLoaderServiceHandle h, size_t *outSize)
{
    FileLoaderConcurrentAccess *self=SELF(service_handle);
    if(!self){errno=EINVAL;return NULL;}
    uint32_t poolIdx=sHandleToIdx(h); uint64_t gen=sHandleToGen(h);
    if(poolIdx==0||poolIdx>=self->pool_capacity){errno=EINVAL;return NULL;}
    int hazSlot=sHazptrAcquire(self); if(hazSlot<0){errno=ENOMEM;return NULL;}
    struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
    int timeoutMs=(int)self->default_buffer_timeout_ms;
    const int maxRecovery=1; int recoveryCount=0;
    while(true){
        FileLoaderConcurrentAccessNode *node=sNodeAt(self,poolIdx);
        if(atomic_load_explicit(&node->generation,memory_order_acquire)!=gen){
            /* ── Handle is stale — attempt implicit recovery ── */
            if(recoveryCount>=maxRecovery){sHazptrRelease(self,hazSlot);errno=ESTALE;return NULL;}
            recoveryCount++;
            /* Copy path from node.  Pool is pre-allocated (static lifetime) so
             * node memory is always valid even if recycled — path[] contains
             * whatever file currently occupies this slot.                    */
            char pathCopy[PATH_MAX];
            strncpy(pathCopy,node->path,sizeof(pathCopy)-1);
            pathCopy[sizeof(pathCopy)-1]='\0';
            /* Re-lookup in hashmap — is this file still registered? */
            uint32_t freshIdx=0;
            if(!LocklessFixedWidthHashMap_Get(&self->file_map,pathCopy,&freshIdx)||freshIdx==0){
                sHazptrRelease(self,hazSlot);errno=ESTALE;return NULL;}
            /* Recover: use fresh handle from current mapping */
            FileLoaderConcurrentAccessNode *fresh=sNodeAt(self,freshIdx);
            poolIdx=freshIdx;
            gen=atomic_load_explicit(&fresh->generation,memory_order_acquire);
            continue;
        }
        uintptr_t bufPtr=atomic_load_explicit(&node->buffer,memory_order_acquire);
        if(bufPtr){atomic_store_explicit(&self->hazptrs[hazSlot],bufPtr,memory_order_release);
            atomic_thread_fence(memory_order_acquire);
            if(atomic_load_explicit(&node->generation,memory_order_acquire)==gen&&atomic_load_explicit(&node->buffer,memory_order_acquire)==bufPtr){
                atomic_store_explicit(&node->cache_ref_bit,1,memory_order_relaxed);
                atomic_store_explicit(&node->reg_ref_bit,1,memory_order_relaxed);
                if(outSize)*outSize=0; return (void*)bufPtr;}
            sHazptrRelease(self,hazSlot); continue;}
        uint32_t st=atomic_load_explicit(&node->state,memory_order_acquire);
        if(st!=FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE){sHazptrRelease(self,hazSlot);errno=ENOENT;return NULL;}
        uint32_t flags=atomic_load_explicit(&node->flags,memory_order_relaxed);
        if(!(flags&(FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD|FILELOADERCONCURRENTACCESS_FLAG_LOADING)))
            atomic_fetch_or_explicit(&node->flags,FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD,memory_order_acq_rel);
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(((now.tv_sec-start.tv_sec)*1000L+(now.tv_nsec-start.tv_nsec)/1000000L)>(long)timeoutMs)
            {sHazptrRelease(self,hazSlot);errno=ETIMEDOUT;return NULL;}
        usleep(10000);
    }
}

PUBLIC_API void
FileLoaderServiceReturnFile(FileLoaderService *service_handle, void *buf)
{
    FileLoaderConcurrentAccess *self=SELF(service_handle); if(!self||!buf)return;
    for(uint32_t i=0;i<self->hazptr_slot_count;i++)
        if(atomic_load_explicit(&self->hazptrs[i],memory_order_relaxed)==(uintptr_t)buf)
            {atomic_store_explicit(&self->hazptrs[i],0,memory_order_release);return;}
}

PUBLIC_API void     FileLoaderServiceStop(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);if(f)atomic_store_explicit(&f->running,false,memory_order_release);}
PUBLIC_API uint32_t FileLoaderServiceSize(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);return f?LocklessFixedWidthHashMap_Size(&f->file_map):0;}
PUBLIC_API uint32_t FileLoaderServiceCapacity(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);return f?f->pool_capacity:0;}
PUBLIC_API uint64_t FileLoaderServiceHeartbeat(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);return f?atomic_load_explicit(&f->monitor_heartbeat,memory_order_relaxed):0;}
PUBLIC_API pthread_t FileLoaderServiceGetThread(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);return f?f->monitor_thread:(pthread_t)0;}
PUBLIC_API bool FileLoaderServiceRegisterThread(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);return f&&sHazptrAcquire(f)>=0;}
PUBLIC_API void FileLoaderServiceUnregisterThread(FileLoaderService *s){FileLoaderConcurrentAccess *f=SELF(s);if(!f)return;pthread_once(&sHazptrKeyOnce,sMakeHazptrKey);struct sHazptrThreadState *st=(struct sHazptrThreadState*)pthread_getspecific(sHazptrKey);if(st&&st->owner==f){sHazptrRelease(f,st->slot);st->owner=NULL;st->slot=-1;}}
