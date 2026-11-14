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
#include <uflib/logger/logger.h>
#include <uflib/utils_threads.h>
#include <uflib/cdt/hashmap/cdt_fixed_width_hashmap.h>
#include <uflib/cdt/cdt_mpsc_queue.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>

#include "file_loader_service_type_priv.h"
#include "file_loader_service_defs_priv.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>

#define SELF(svc)  ((FileLoaderConcurrentAccess *)(svc))

static char sEmptyFileBuffer;

/* ── Forward declarations ─────────────────────────────────────────────── */

static FileLoaderConcurrentAccessNode *sNodeAt(FileLoaderConcurrentAccess *, uint32_t);
static void sRetireMapping(FileLoaderConcurrentAccess *, FileLoaderConcurrentAccessNode *);
static uintptr_t sPublishMapping(FileLoaderConcurrentAccessNode *, uintptr_t, size_t, size_t *);
static void sReleaseCacheSlots(FileLoaderConcurrentAccess *, uint32_t);
static size_t sNodeSize(uint32_t);
static void sResolveConfig(FileLoaderConcurrentAccess *, const FileLoaderServiceConcurrent *);
static uint32_t sNextPow2(uint32_t);
static uint32_t sNodeAllocate(FileLoaderConcurrentAccess *);
static void sNodeFree(FileLoaderConcurrentAccess *, uint32_t);
__attribute__((unused)) static uint32_t sFreeCount(FileLoaderConcurrentAccess *);
static uint32_t sFindVictim(FileLoaderConcurrentAccess *);
static void sRemoveFileImpl(FileLoaderConcurrentAccess *, const char *);
static FileLoaderServiceFileHandle sMakeHandle(uint32_t, uint64_t);
static uint32_t sHandleToIdx(FileLoaderServiceFileHandle);
static uint64_t sHandleToGen(FileLoaderServiceFileHandle);
static bool sLoadFile(FileLoaderConcurrentAccess *, uint32_t);
static void sUnloadBuffer(FileLoaderConcurrentAccess *, uint32_t);
static void sClockEvictBuffer(FileLoaderConcurrentAccess *);
static int sHazptrAcquire(FileLoaderConcurrentAccess *);
static void sHazptrRelease(FileLoaderConcurrentAccess *, int);
static bool sIsHazardous(FileLoaderConcurrentAccess *, uintptr_t);
static void sLimboRetire(FileLoaderConcurrentAccess *, void *, size_t);
static void sLimboDrain(FileLoaderConcurrentAccess *);
static void *sMonitorThread(void *);

/* ── Handle encoding ──────────────────────────────────────────────────── */

static inline FileLoaderServiceFileHandle
sMakeHandle(uint32_t poolIdx, uint64_t gen)
{
  return ((uint64_t)poolIdx << FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT) | (gen & FILELOADERCONCURRENTACCESS_HANDLE_GEN_MASK);
}

static inline uint32_t sHandleToIdx(FileLoaderServiceFileHandle h)
{
  return (uint32_t)(h >> FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT);
}

static inline uint64_t sHandleToGen(FileLoaderServiceFileHandle h)
{
  return h & FILELOADERCONCURRENTACCESS_HANDLE_GEN_MASK;
}

/* ── Node access/sizing ───────────────────────────────────────────────── */

static inline FileLoaderConcurrentAccessNode *
sNodeAt(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
  return (FileLoaderConcurrentAccessNode*)(self->node_pool + (size_t)poolIdx * self->node_stride);
}

static size_t sNodeSize(uint32_t kw)
{
  return ((sizeof(FileLoaderConcurrentAccessNode) + (size_t)kw) + 63U) & ~((size_t)63U);
}

static uint32_t sNextPow2(uint32_t v)
{
  if (v == 0) return 1;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

/* ── Config resolution ────────────────────────────────────────────────── */

static void
sResolveConfig(FileLoaderConcurrentAccess *self, const FileLoaderServiceConcurrent *config)
{
  if (!config) {
    self->pool_capacity             = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY + 1;
    self->buffer_cache_capacity     = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ;
    self->key_width                 = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH;
    self->max_file_size_bytes       = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES;
    self->hazptr_slot_count         = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS;
    self->limbo_capacity            = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ;
    self->default_buffer_timeout_ms = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS;
    self->on_evicted                = NULL;
    self->on_evicted_ctx            = NULL;
    return;
  }
  uint32_t registry_slots = config->service_config_params.registry_size ?
                              config->service_config_params.registry_size :
                              CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY;

  /* Index 0 is the pool's null sentinel (handle 0, absent hashmap value,
   * free-stack terminator), so allocate one extra node and let registry_size
   * denote the number of usable file slots. */
  if (registry_slots == UINT32_MAX) registry_slots--;
  self->pool_capacity = registry_slots + 1;
  self->buffer_cache_capacity = config->service_config_params.loaded_size ?
                                  config->service_config_params.loaded_size :
                                  CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ;
  self->key_width                 = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH;
  self->max_file_size_bytes       = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES;
  self->hazptr_slot_count         = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS;
  self->limbo_capacity            = PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ;
  self->default_buffer_timeout_ms = CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS;
  self->on_evicted                = NULL;
  self->on_evicted_ctx            = NULL;
}

/* ── Node pool ────────────────────────────────────────────────────────── */

static inline uintptr_t sPackFreeNode(uint32_t idx, uint32_t pop) { return ((uintptr_t)pop << 32) | (uintptr_t)idx; }
static inline uint32_t  sUnpackFreeIdx(uintptr_t p) { return (uint32_t)(p & 0xFFFFFFFFU); }
static inline uint32_t  sUnpackPopCount(uintptr_t p) { return (uint32_t)(p >> 32); }

static uint32_t
sNodeAllocate(FileLoaderConcurrentAccess *self)
{
  uintptr_t head = atomic_load_explicit(&self->node_free_stack_head, memory_order_acquire);
  while (head != 0) {
    uint32_t  idx     = sUnpackFreeIdx(head), pop = sUnpackPopCount(head);
    uint32_t  next    = atomic_load_explicit(&sNodeAt(self, idx)->next_free, memory_order_relaxed);
    uintptr_t newHead = sPackFreeNode(next, pop + 1);
    if (atomic_compare_exchange_strong_explicit(&self->node_free_stack_head, &head, newHead, memory_order_acq_rel, memory_order_acquire)) {
      atomic_store_explicit(&sNodeAt(self, idx)->publish_state, FILELOADERCONCURRENTACCESS_NODE_INIT, memory_order_release);
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
  atomic_store_explicit(&node->is_loaded, false, memory_order_relaxed);
  sRetireMapping(self, node);
  sReleaseCacheSlots(self, poolIdx);
  atomic_store_explicit(&node->publish_state, FILELOADERCONCURRENTACCESS_NODE_FREE, memory_order_release);
  uintptr_t oldHead, newHead;
  do {
    oldHead         = atomic_load_explicit(&self->node_free_stack_head, memory_order_acquire);
    atomic_store_explicit(&node->next_free, sUnpackFreeIdx(oldHead), memory_order_relaxed);
    newHead         = sPackFreeNode(poolIdx, sUnpackPopCount(oldHead) + 1);
  }
  while (!atomic_compare_exchange_strong_explicit(&self->node_free_stack_head, &oldHead, newHead, memory_order_acq_rel,
                                                  memory_order_relaxed));
}

__attribute__((unused)) static uint32_t
sFreeCount(FileLoaderConcurrentAccess *self)
{
  uint32_t  c = 0;
  uintptr_t h = atomic_load_explicit(&self->node_free_stack_head, memory_order_relaxed);
  while (h) {
    c++;
    h = (uintptr_t)atomic_load_explicit(&sNodeAt(self, sUnpackFreeIdx(h))->next_free, memory_order_relaxed);
  }
  return c;
}

/* ── File loading ─────────────────────────────────────────────────────── */

static bool
sLoadFile(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
  FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
  struct stat                     st;
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "load start: idx=%u path='%s' max_file_size_bytes=%zu", poolIdx, node->path, self->max_file_size_bytes);
#endif
  if (stat(node->path, &st) != 0) goto error;

  if ((size_t)st.st_size > self->max_file_size_bytes) {
    atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_TOO_LARGE, memory_order_release);
    atomic_fetch_and_explicit(&node->flags, ~FILELOADERCONCURRENTACCESS_FLAG_LOADING, memory_order_release);
#if UF_DEBUG_BUILD
    self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "rejected oversized file: idx=%u path='%s' size=%jd max=%zu", poolIdx, node->path,
              (intmax_t)st.st_size, self->max_file_size_bytes);
#endif
    return false;
  }
  if (!(st.st_mode & S_IRUSR)) goto error;
  if (st.st_size == 0) {
#if UF_DEBUG_BUILD
    self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "empty file: idx=%u path='%s' — no mapping", poolIdx, node->path);
#endif
    sRetireMapping(self, node);
    atomic_store_explicit(&node->is_loaded, true, memory_order_release);
    atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE, memory_order_release);
    atomic_fetch_and_explicit(&node->flags, ~(FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD|FILELOADERCONCURRENTACCESS_FLAG_LOADING), memory_order_release);
    return true;
  }
  int fd = open(node->path, O_RDONLY);
  if (fd < 0) goto error;
  void *data = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);
  if (data == MAP_FAILED) goto error;
  madvise(data, (size_t)st.st_size, MADV_SEQUENTIAL);
  mprotect(data, (size_t)st.st_size, PROT_READ);

  size_t    oldSize = 0;
  uintptr_t oldBuf  = sPublishMapping(node, (uintptr_t)data, (size_t)st.st_size, &oldSize);
  if (oldBuf) sLimboRetire(self, (void*)oldBuf, oldSize);
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "loaded: idx=%u path='%s' bytes=%jd mapping=%p", poolIdx, node->path,
            (intmax_t)st.st_size, data);
#endif
  atomic_store_explicit(&node->is_loaded, true, memory_order_release);
  atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE, memory_order_release);
  atomic_fetch_and_explicit(&node->flags, ~(FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD|FILELOADERCONCURRENTACCESS_FLAG_LOADING), memory_order_release);
  return true;

error:
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "load failed: idx=%u path='%s' errno=%d", poolIdx, node->path, errno);
#endif
  atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_LOAD_ERROR, memory_order_release);
  atomic_fetch_and_explicit(&node->flags, ~FILELOADERCONCURRENTACCESS_FLAG_LOADING, memory_order_release);
  return false;
}

/* ── Eviction ─────────────────────────────────────────────────────────── */

static uint32_t
sFindVictim(FileLoaderConcurrentAccess *self)
{
  if (!self->on_evicted) return 0;
  uint32_t start = atomic_fetch_add_explicit(&self->registry_clock_hand, 1, memory_order_relaxed);
  for (uint32_t i = 0; i < self->pool_capacity * 2; i++) {
    uint32_t idx = (start + i) % self->pool_capacity;
    if (idx == 0) continue;
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, idx);
    if (!atomic_load_explicit(&node->is_active, memory_order_acquire)) continue;
    if (atomic_load_explicit(&node->reg_ref_bit, memory_order_relaxed) == 1) {
      atomic_store_explicit(&node->reg_ref_bit, 0, memory_order_relaxed);
      continue;
    }
    atomic_store_explicit(&self->registry_clock_hand, (idx+1)%self->pool_capacity, memory_order_relaxed);
#if UF_DEBUG_BUILD
    self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "victim found: idx=%u path='%s' scan_start=%u", idx, node->path, start);
#endif
    return idx;
  }
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "no victim: scan_start=%u capacity=%u", start, self->pool_capacity);
#endif
  return 0;
}

/* ── Buffer cache ─────────────────────────────────────────────────────── */

static uint32_t
sSeqBegin(FileLoaderConcurrentAccessNode *node)
{
  uint32_t seq;

  for (;;) {
    seq = atomic_load_explicit(&node->publish_seq, memory_order_relaxed);
    if (seq & 1U) {
      sched_yield();
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(&node->publish_seq, &seq, seq + 1, memory_order_acquire,
                                              memory_order_relaxed)) return seq + 1;
  }
}

static void
sSeqEnd(FileLoaderConcurrentAccessNode *node, uint32_t odd)
{
  atomic_store_explicit(&node->publish_seq, odd + 1, memory_order_release);
}

static uintptr_t
sPublishMapping(FileLoaderConcurrentAccessNode *node, uintptr_t buf, size_t size, size_t *old_size_out)
{
  uint32_t  seq = sSeqBegin(node);
  uintptr_t oldBuf = atomic_exchange_explicit(&node->buffer, buf, memory_order_relaxed);

  *old_size_out = atomic_load_explicit(&node->size_bytes, memory_order_relaxed);
  atomic_store_explicit(&node->size_bytes, size, memory_order_relaxed);
  sSeqEnd(node, seq);
  return oldBuf;
}

static void
sRetireMapping(FileLoaderConcurrentAccess *self, FileLoaderConcurrentAccessNode *node)
{
  uint32_t  seq;
  uintptr_t oldBuf;
  size_t    oldSize;

  seq     = sSeqBegin(node);
  oldBuf  = atomic_exchange_explicit(&node->buffer, 0, memory_order_relaxed);
  oldSize = atomic_load_explicit(&node->size_bytes, memory_order_relaxed);
  atomic_store_explicit(&node->size_bytes, 0, memory_order_relaxed);
  sSeqEnd(node, seq);
  if (oldBuf) sLimboRetire(self, (void*)oldBuf, oldSize);
}

static void
sUnloadBuffer(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
  FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
  atomic_store_explicit(&node->is_loaded, false, memory_order_release);
  sRetireMapping(self, node);
  atomic_fetch_and_explicit(&node->flags, ~FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE, memory_order_release);
  atomic_fetch_sub_explicit(&self->cache_loaded_count, 1, memory_order_relaxed);
}

static void
sReleaseCacheSlots(FileLoaderConcurrentAccess *self, uint32_t poolIdx)
{
  for (uint32_t i = 0; i < self->buffer_cache_capacity; i++) {
    uintptr_t packed = atomic_load_explicit(&self->buffer_cache[i].slot, memory_order_acquire);
    if ((uint32_t)(packed >> 32) != poolIdx) continue;
    if (atomic_compare_exchange_strong_explicit(&self->buffer_cache[i].slot, &packed, 0, memory_order_acq_rel,
                                                memory_order_relaxed))
      atomic_fetch_sub_explicit(&self->cache_loaded_count, 1, memory_order_relaxed);
  }
}

static void
sClockEvictBuffer(FileLoaderConcurrentAccess *self)
{
  uint32_t hand = atomic_load_explicit(&self->cache_clock_hand, memory_order_relaxed);
  for (uint32_t i = 0; i < self->buffer_cache_capacity * 2; i++) {
    uintptr_t packed  = atomic_load_explicit(&self->buffer_cache[hand].slot, memory_order_acquire);
    uint32_t  poolIdx = (uint32_t)(packed >> 32);
    uint64_t  gen     = packed & 0xFFFFFFFFU;
    if (poolIdx != 0) {
      FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
      if ((uint32_t)atomic_load_explicit(&node->generation, memory_order_acquire) != (uint32_t)gen) {
        if (atomic_compare_exchange_strong_explicit(&self->buffer_cache[hand].slot, &packed, 0, memory_order_acq_rel, memory_order_relaxed))
          atomic_fetch_sub_explicit(&self->cache_loaded_count, 1, memory_order_relaxed);
      }
      else if (atomic_load_explicit(&node->cache_ref_bit, memory_order_relaxed) == 1) atomic_store_explicit(
        &node->cache_ref_bit, 0, memory_order_relaxed);
      else {
        uintptr_t expected = packed;
        if (atomic_compare_exchange_strong_explicit(&self->buffer_cache[hand].slot, &expected, 0, memory_order_acq_rel, memory_order_relaxed)) {
          sUnloadBuffer(self, poolIdx);
          return;
        }
      }
    }
    hand = (hand + 1) % self->buffer_cache_capacity;
    atomic_store_explicit(&self->cache_clock_hand, hand, memory_order_relaxed);
  }
}

/* ── Hazard pointers ──────────────────────────────────────────────────── */

static pthread_key_t  sHazptrKey;
static pthread_once_t sHazptrKeyOnce = PTHREAD_ONCE_INIT;
static void           sMakeHazptrKey(void) { pthread_key_create(&sHazptrKey,NULL); }

struct sHazptrThreadState
{
  FileLoaderConcurrentAccess *owner;
  int                         slot;
};

static int
sHazptrAcquire(FileLoaderConcurrentAccess *self)
{
  pthread_once(&sHazptrKeyOnce, sMakeHazptrKey);
  struct sHazptrThreadState *st = (struct sHazptrThreadState*)pthread_getspecific(sHazptrKey);
  if (st && st->owner == self) return st->slot;
  for (uint32_t i = 0; i < self->hazptr_slot_count; i++) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&self->hazptr_reserved[i], &expected, true)) {
      if (!st) {
        st = (struct sHazptrThreadState*)calloc(1, sizeof(*st));
        pthread_setspecific(sHazptrKey, st);
      }
      st->owner = self;
      st->slot  = (int)i;
      return (int)i;
    }
  }
  return -1;
}

static void sHazptrRelease(FileLoaderConcurrentAccess *self, int slot)
{
  if (slot < 0 || (uint32_t)slot >= self->hazptr_slot_count)return;
  atomic_store_explicit(&self->hazptrs[slot], 0, memory_order_release);
}

static bool sIsHazardous(FileLoaderConcurrentAccess *self, uintptr_t bufPtr)
{
  for (uint32_t i = 0; i < self->hazptr_slot_count; i++) if (atomic_load_explicit(
    &self->hazptrs[i], memory_order_acquire) == bufPtr) return true;
  return false;
}

/* ── Limbo ring ───────────────────────────────────────────────────────── */

static void
sLimboRetire(FileLoaderConcurrentAccess *self, void *buf, size_t size)
{
  uint32_t wIdx;
  uint32_t rIdx;
  uint32_t nextW;

  while (atomic_exchange_explicit(&self->limbo_lock, true, memory_order_acquire)) sched_yield();
  wIdx  = atomic_load_explicit(&self->limbo_write_idx, memory_order_relaxed);
  rIdx  = atomic_load_explicit(&self->limbo_read_idx, memory_order_acquire);
  nextW = (wIdx + 1) % self->limbo_capacity;
  if (nextW == rIdx) {
    atomic_store_explicit(&self->limbo_lock, false, memory_order_release);
    munmap(buf, size);
    return;
  }
  self->limbo_ring[wIdx].ptr  = (uintptr_t)buf;
  self->limbo_ring[wIdx].size = size;
  atomic_store_explicit(&self->limbo_write_idx, nextW, memory_order_release);
  atomic_store_explicit(&self->limbo_lock, false, memory_order_release);
}

static void
sLimboDrain(FileLoaderConcurrentAccess *self)
{
  uint32_t rIdx = atomic_load_explicit(&self->limbo_read_idx, memory_order_relaxed);
  uint32_t wIdx = atomic_load_explicit(&self->limbo_write_idx, memory_order_acquire);
  while (rIdx != wIdx) {
    uintptr_t bufPtr = self->limbo_ring[rIdx].ptr;
    size_t    size   = self->limbo_ring[rIdx].size;
    if (bufPtr && sIsHazardous(self, bufPtr))break;
    if (bufPtr)munmap((void*)bufPtr, size);
    rIdx = (rIdx + 1) % self->limbo_capacity;
    atomic_store_explicit(&self->limbo_read_idx, rIdx, memory_order_release);
  }
}

/* ── Monitor thread ───────────────────────────────────────────────────── */

static void *
sMonitorThread(void *arg)
{
  FileLoaderConcurrentAccess *self = (FileLoaderConcurrentAccess*)arg;
  struct epoll_event          events[32];

  SetThreadName(self->proc_name);
  while (atomic_load_explicit(&self->running, memory_order_acquire)) {
    int nfds = epoll_wait(self->epoll_fd, events, 32,
                          (int)CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MONITOR_TIMEOUT_MS);
    (void)nfds;
    for (uint32_t i = 1; i < self->pool_capacity; i++) {
      FileLoaderConcurrentAccessNode *node = sNodeAt(self, i);
      if (!atomic_load_explicit(&node->is_active, memory_order_acquire))  continue;
      uint32_t flags = atomic_load_explicit(&node->flags, memory_order_acquire);
      if (!(flags & FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD))  continue;
      if (flags & FILELOADERCONCURRENTACCESS_FLAG_LOADING)  continue;
      uint32_t expected = FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD;
      if (!atomic_compare_exchange_strong_explicit(&node->flags, &expected, FILELOADERCONCURRENTACCESS_FLAG_LOADING,
                                                   memory_order_acq_rel, memory_order_relaxed))continue;
      uint64_t gen_at_claim = atomic_load_explicit(&node->generation, memory_order_acquire);
      if (atomic_load_explicit(&self->cache_loaded_count, memory_order_relaxed) >= self->buffer_cache_capacity)
        sClockEvictBuffer(self);
      if (!(expected & FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE)) {
        for (uint32_t c = 0; c < self->buffer_cache_capacity; c++) {
          uintptr_t empty = 0;
          if (atomic_compare_exchange_strong_explicit(&self->buffer_cache[c].slot, &empty, ((uintptr_t)i << 32) | (uint32_t)atomic_load_explicit(&node->generation, memory_order_acquire), memory_order_acq_rel, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&self->cache_loaded_count, 1, memory_order_relaxed);
            atomic_fetch_or_explicit(&node->flags, FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE, memory_order_release);
            break;
          }
        }
      }
      sLoadFile(self, i);
      if (atomic_load_explicit(&node->generation, memory_order_acquire) != gen_at_claim) {
        atomic_store_explicit(&node->is_loaded, false, memory_order_release);
        sRetireMapping(self, node);
      }
    }
    sLimboDrain(self);
    atomic_fetch_add_explicit(&self->monitor_heartbeat, 1, memory_order_relaxed);
  }
  return NULL;
}

static void
sLogSink(FileLoaderConcurrentAccess *self, UfLoggerLevel level, const char *file_ptr, int line, const char *function_ptr, const char *format_ptr, ...)
{
  (void)self;
  (void)level;
  (void)file_ptr;
  (void)line;
  (void)function_ptr;
  (void)format_ptr;
}

static void
sLogForward(FileLoaderConcurrentAccess *self, UfLoggerLevel level, const char *file_ptr, int line, const char *function_ptr, const char *format_ptr, ...)
{
  va_list args;

  va_start(args, format_ptr);
  UfLoggerVLog(self->uf_logger, level, file_ptr, line, function_ptr, format_ptr, args);
  va_end(args);
}

/* ── Public API ───────────────────────────────────────────────────────── */

static bool
sInitCommon(FileLoaderServiceConcurrent *config, UfLogger *logger_ptr)
{
  if (!config) return false;
  FileLoaderConcurrentAccess *self = (FileLoaderConcurrentAccess*)calloc(1, sizeof(*self));
  if (!self) return false;
  self->uf_logger = logger_ptr;
  self->log       = logger_ptr ? sLogForward : sLogSink;
  if (config->proc_name[0]) {
    strncpy(self->proc_name, config->proc_name, PRCTL_MAX_NAME_SZ);
    self->proc_name[PRCTL_MAX_NAME_SZ] = '\0';
  }
  else {
    strncpy(self->proc_name, "ufFLoader", PRCTL_MAX_NAME_SZ);
    self->proc_name[PRCTL_MAX_NAME_SZ] = '\0';
  }
  atomic_init(&self->monitor_reaped, false);
  atomic_init(&self->limbo_lock, false);
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: requested registry_size=%u loaded_size=%u blacklist_entries=%u logger=%s", config->service_config_params.registry_size, config->service_config_params.loaded_size, config->service_config_params.blacklist_entry_count, logger_ptr ? "injected" : "none");
#endif
  sResolveConfig(self, config);
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: resolved pool_capacity=%u buffer_cache_capacity=%u key_width=%u max_file_size_bytes=%zu", self->pool_capacity > 0 ? self->pool_capacity - 1 : 0, self->buffer_cache_capacity, self->key_width, self->max_file_size_bytes);
#endif

  uint32_t fileMapSlots = sNextPow2(
    (uint32_t)((uint64_t)self->pool_capacity * 100U / CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR));
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: file_map slots=%u", fileMapSlots);
#endif
  if (!LocklessFixedWidthHashMap_Init(&self->file_map, fileMapSlots,
                                      CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR,
                                      self->key_width)) goto fail;
  if (!LocklessFixedWidthHashMap_Init(&self->blacklist_map,
                                      sNextPow2(CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BLACKLIST_CAPACITY),
                                      CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR,
                                      self->key_width)) goto fail;

  self->node_stride = sNodeSize(self->key_width);
  self->node_pool   = (uint8_t*)calloc(self->pool_capacity, self->node_stride);
  if (!self->node_pool) goto fail;
  atomic_init(&self->node_free_stack_head, 0);
  for (uint32_t i = self->pool_capacity - 1; i > 0; i--) {
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, i);
    atomic_store_explicit(&node->next_free, sUnpackFreeIdx(atomic_load_explicit(&self->node_free_stack_head, memory_order_relaxed)), memory_order_relaxed);
    atomic_init(&node->generation, 1);
    atomic_init(&node->is_active, false);
    atomic_store_explicit(&self->node_free_stack_head,
                          sPackFreeNode(i, sUnpackPopCount(atomic_load_explicit(&self->node_free_stack_head,
                            memory_order_relaxed))+1), memory_order_release);
  }
  self->buffer_cache = (FileLoaderConcurrentAccessCacheSlot*)calloc(self->buffer_cache_capacity,
                                                                    sizeof(FileLoaderConcurrentAccessCacheSlot));
  if (!self->buffer_cache) goto fail;
  self->hazptrs = (_Atomic uintptr_t*)calloc(self->hazptr_slot_count, sizeof(_Atomic uintptr_t));
  if (!self->hazptrs) goto fail;
  self->hazptr_reserved = (_Atomic bool*)calloc(self->hazptr_slot_count, sizeof(_Atomic bool));
  if (!self->hazptr_reserved) goto fail;
  self->limbo_ring = (FileLoaderConcurrentAccessLimboEntry*)calloc(self->limbo_capacity,
                                                                   sizeof(FileLoaderConcurrentAccessLimboEntry));
  if (!self->limbo_ring) goto fail;
  mpsc_queue_init(&self->work_queue);
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: memory ready nodes=%u node_stride=%zu cache_slots=%u hazptr_slots=%u limbo_slots=%u", self->pool_capacity, self->node_stride, self->buffer_cache_capacity, self->hazptr_slot_count, self->limbo_capacity);
#endif

  /* VTable retained for internal use (not exposed to consumers) */
  memset(&self->vtable, 0, sizeof(self->vtable));

  self->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (self->inotify_fd < 0) goto fail;
  self->epoll_fd = epoll_create1(0);
  if (self->epoll_fd < 0) goto fail;
  {
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = self->inotify_fd;
    if (epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, self->inotify_fd, &ev) < 0) goto fail;
  }
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: watch ready inotify_fd=%d epoll_fd=%d", self->inotify_fd, self->epoll_fd);
#endif

  if (config->service_config_params.blacklist_entries && config->service_config_params.blacklist_entry_count > 0) {
    for (uint32_t i = 0; i < config->service_config_params.blacklist_entry_count; i++) {
      const char *blPath = config->service_config_params.blacklist_entries[i];
      if (!blPath) continue;
      bool isNewBl = false;
      LocklessFixedWidthHashMap_Insert(&self->blacklist_map, blPath, 1, &isNewBl);
    }
  }
  atomic_store_explicit(&self->running, true, memory_order_relaxed);
  atomic_store_explicit(&self->monitor_heartbeat, 0, memory_order_relaxed);
  {
    int rc = pthread_create(&self->monitor_thread,NULL, sMonitorThread, self);
    if (rc != 0) {
      syslog(LOG_ERR, "%s: pthread_create failed: %s", __func__, strerror(rc));
      goto fail;
    }
  }

#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: monitor thread started tid=%lu", (unsigned long)self->monitor_thread);
  if (self->uf_logger) {
    BufferDescriptor *description = FileLoaderServiceDescribe((const FileLoaderService*)self, NULL);
    if (description) {
      size_t len = description->size;
      while (len > 0 && description->data[len - 1] == '\n') description->data[--len] = '\0';
      self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init: describe %s", description->data);
      BufferDescriptorRelease(description);
      free(description);
    }
  }
#endif

  config->service_handle = (FileLoaderService*)self;
  return true;

fail:
#if UF_DEBUG_BUILD
  self->log(self, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "init failed: errno=%d (%s)", errno, strerror(errno));
#endif
  FileLoaderServiceDestroy((FileLoaderService*)self);
  return false;
}

PUBLIC_API bool
FileLoaderServiceInit(FileLoaderServiceConcurrent *config)
{
  return sInitCommon(config, NULL);
}

PUBLIC_API bool
FileLoaderServiceInitWithLogger(FileLoaderServiceConcurrent *config, UfLogger *logger_ptr)
{
  return sInitCommon(config, logger_ptr);
}

static void
sReapMonitorThread(FileLoaderConcurrentAccess *self)
{
  bool expected = false;

  atomic_store_explicit(&self->running, false, memory_order_release);
  if (!atomic_compare_exchange_strong_explicit(&self->monitor_reaped, &expected, true,
                                               memory_order_acq_rel, memory_order_relaxed)) return;
  if (self->monitor_thread) {
    pthread_join(self->monitor_thread, NULL);
    self->monitor_thread = 0;
  }
}

PUBLIC_API void
FileLoaderServiceDestroy(FileLoaderService *service_handle)
{
  if (!service_handle) return;
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  sReapMonitorThread(self);
  if (self->limbo_ring) {
    for (uint32_t i = 0; i < self->limbo_capacity; i++) {
      if (self->limbo_ring[i].ptr)munmap((void*)self->limbo_ring[i].ptr, self->limbo_ring[i].size);
    }
    free(self->limbo_ring);
  }
  if (self->node_pool) {
    for (uint32_t i = 1; i < self->pool_capacity; i++) {
      uintptr_t buf = atomic_load_explicit(&sNodeAt(self,i)->buffer, memory_order_relaxed);
      size_t    len = atomic_load_explicit(&sNodeAt(self,i)->size_bytes, memory_order_relaxed);
      if (buf)munmap((void*)buf, len);
    }
    free(self->node_pool);
  }
  free((void*)self->hazptrs);
  free((void*)self->hazptr_reserved);
  free(self->buffer_cache);
  if (self->epoll_fd >= 0)close(self->epoll_fd);
  if (self->inotify_fd >= 0)close(self->inotify_fd);
  LocklessFixedWidthHashMap_Destroy(&self->blacklist_map);
  LocklessFixedWidthHashMap_Destroy(&self->file_map);
  free(self);
}

static void
sResetNode(FileLoaderConcurrentAccessNode *node, uint32_t key_width)
{
  atomic_store_explicit(&node->state, 0, memory_order_relaxed);
  atomic_store_explicit(&node->buffer, 0, memory_order_relaxed);
  atomic_store_explicit(&node->size_bytes, 0, memory_order_relaxed);
  atomic_store_explicit(&node->publish_seq, 0, memory_order_relaxed);
  atomic_store_explicit(&node->flags, 0, memory_order_relaxed);
  atomic_store_explicit(&node->cache_ref_bit, 0, memory_order_relaxed);
  atomic_store_explicit(&node->reg_ref_bit, 0, memory_order_relaxed);
  atomic_store_explicit(&node->is_active, false, memory_order_relaxed);
  atomic_store_explicit(&node->is_loaded, false, memory_order_relaxed);
  atomic_store_explicit(&node->publish_state, FILELOADERCONCURRENTACCESS_NODE_INIT, memory_order_relaxed);
  node->inotify_wd = -1;
  memset(node->path, 0, key_width);
}

static void
sAwaitPublish(FileLoaderConcurrentAccessNode *node)
{
  while (atomic_load_explicit(&node->publish_state, memory_order_acquire) != FILELOADERCONCURRENTACCESS_NODE_PUBLISHED)
    sched_yield();
}

static FileLoaderServiceFileHandle
sExistingHandle(FileLoaderConcurrentAccess *self, const char *path)
{
  uint32_t existingIdx = 0;
  if (!LocklessFixedWidthHashMap_Get(&self->file_map, path, &existingIdx) || existingIdx == 0) return 0;
  FileLoaderConcurrentAccessNode *existing = sNodeAt(self, existingIdx);
  sAwaitPublish(existing);
  return sMakeHandle(existingIdx, atomic_load_explicit(&existing->generation, memory_order_acquire));
}

PUBLIC_API FileLoaderServiceFileHandle
FileLoaderServiceAddFile(FileLoaderService *service_handle, const char *path)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  if (!self || !path || !*path) return 0;
  {
    uint32_t d = 0;
    if (LocklessFixedWidthHashMap_Get(&self->blacklist_map, path, &d)) return 0;
  }
  FileLoaderServiceFileHandle existing = sExistingHandle(self, path);
  if (existing) return existing;
  bool     isNew    = false;
  uint32_t allocIdx = sNodeAllocate(self);
  while (allocIdx == 0 && self->on_evicted != NULL) {
    uint32_t v = sFindVictim(self);
    if (v == 0) break;
    sRemoveFileImpl(self, sNodeAt(self, v)->path);
    allocIdx = sNodeAllocate(self);
  }
  if (allocIdx == 0)  return 0;
  FileLoaderConcurrentAccessNode *node      = sNodeAt(self, allocIdx);
  uint64_t                        oldGen    = atomic_load_explicit(&node->generation, memory_order_relaxed);
  sResetNode(node, self->key_width);
  atomic_store_explicit(&node->generation, oldGen, memory_order_relaxed);
  strncpy(node->path, path, self->key_width - 1);
  node->path[self->key_width - 1] = '\0';
  if (!LocklessFixedWidthHashMap_Insert(&self->file_map, path, allocIdx, &isNew) || !isNew) {
    sNodeFree(self, allocIdx);
    return sExistingHandle(self, path);
  }
  uint64_t gen = atomic_fetch_add_explicit(&node->generation, 1, memory_order_acq_rel) + 1;
  node->inotify_wd = inotify_add_watch(self->inotify_fd, path,IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
  atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE, memory_order_release);
  atomic_store_explicit(&node->flags, FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD, memory_order_release);
  atomic_store_explicit(&node->is_active, true, memory_order_release);
  atomic_store_explicit(&node->publish_state, FILELOADERCONCURRENTACCESS_NODE_PUBLISHED, memory_order_release);
  return sMakeHandle(allocIdx, gen);
}

static void
sCancelPendingLoad(FileLoaderConcurrentAccessNode *node)
{
  uint32_t flags;

  for (;;) {
    flags = atomic_load_explicit(&node->flags, memory_order_acquire);
    if (flags & FILELOADERCONCURRENTACCESS_FLAG_LOADING) {
      sched_yield();
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(&node->flags, &flags,
                                              flags & ~FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD,
                                              memory_order_acq_rel, memory_order_relaxed)) return;
  }
}

static void
sRemoveFileImpl(FileLoaderConcurrentAccess *self, const char *path)
{
  uint32_t poolIdx = 0;
  if (!LocklessFixedWidthHashMap_Remove(&self->file_map, path, &poolIdx))return;
  FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
  sAwaitPublish(node);
  atomic_store_explicit(&node->is_active, false, memory_order_release);
  sCancelPendingLoad(node);
  atomic_fetch_add_explicit(&node->generation, 1, memory_order_release);
  atomic_store_explicit(&node->state, FILELOADERCONCURRENTACCESS_FILE_STATE_DELETED, memory_order_release);
  if (node->inotify_wd >= 0) {
    inotify_rm_watch(self->inotify_fd, node->inotify_wd);
    node->inotify_wd = -1;
  }
  atomic_store_explicit(&node->is_loaded, false, memory_order_release);
  sRetireMapping(self, node);
  sNodeFree(self, poolIdx);
}

PUBLIC_API void
FileLoaderServiceRemoveFile(FileLoaderService *service_handle, const char *path)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  if (self && path)sRemoveFileImpl(self, path);
}

PUBLIC_API bool
FileLoaderServiceGetFileInfo(FileLoaderService *service_handle, const char *path, FileLoaderServiceFileInfo *out)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  if (!self || !out)return false;
  memset(out, 0, sizeof(*out));
  if (!path)return false;
  strncpy(out->path, path, sizeof(out->path) - 1);
  {
    uint32_t d = 0;
    if (LocklessFixedWidthHashMap_Get(&self->blacklist_map, path, &d)) {
      out->state = FILE_LOADER_SERVICE_FILE_STATE_BLACKLISTED;
      return true;
    }
  }
  uint32_t poolIdx = 0;
  if (!LocklessFixedWidthHashMap_Get(&self->file_map, path, &poolIdx))  return true;
  FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
  if (!atomic_load_explicit(&node->is_active, memory_order_acquire))  return true;
  out->in_registry       = true;
  out->state             = (FileLoaderServiceFileState)atomic_load_explicit(&node->state, memory_order_acquire);
  out->recently_accessed = atomic_load_explicit(&node->reg_ref_bit, memory_order_relaxed) == 1;
  out->is_loaded         = atomic_load_explicit(&node->buffer, memory_order_acquire) != 0;
  out->size_bytes        = atomic_load_explicit(&node->size_bytes, memory_order_relaxed);
  return true;
}

PUBLIC_API void *
FileLoaderServiceGetFileContent(FileLoaderService *service_handle, FileLoaderServiceFileHandle h, size_t *outSize)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  if (outSize) *outSize = 0;
  if (!self) {
    errno = EINVAL;
    return NULL;
  }
  uint32_t poolIdx = sHandleToIdx(h);
  uint64_t gen     = sHandleToGen(h);
  if (poolIdx == 0 || poolIdx >= self->pool_capacity) {
    errno = EINVAL;
    return NULL;
  }
  int hazSlot = sHazptrAcquire(self);
  if (hazSlot < 0) {
    errno = ENOMEM;
    return NULL;
  }
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int       timeoutMs = (int)self->default_buffer_timeout_ms;
  while (true) {
    FileLoaderConcurrentAccessNode *node = sNodeAt(self, poolIdx);
    if (atomic_load_explicit(&node->generation, memory_order_acquire) != gen) {
      sHazptrRelease(self, hazSlot);
      errno = ESTALE;
      return NULL;
    }
    uintptr_t bufPtr = 0;
    size_t    size   = 0;
    for (;;) {
      uint32_t seq = atomic_load_explicit(&node->publish_seq, memory_order_acquire);
      if (seq & 1U) {
        sched_yield();
        continue;
      }
      bufPtr = atomic_load_explicit(&node->buffer, memory_order_relaxed);
      size   = atomic_load_explicit(&node->size_bytes, memory_order_relaxed);
      atomic_thread_fence(memory_order_acquire);
      if (atomic_load_explicit(&node->publish_seq, memory_order_relaxed) == seq) break;
    }
    if (bufPtr) {
      atomic_store_explicit(&self->hazptrs[hazSlot], bufPtr, memory_order_release);
      atomic_thread_fence(memory_order_acquire);
      if (atomic_load_explicit(&node->generation, memory_order_acquire) == gen && atomic_load_explicit(
        &node->buffer, memory_order_acquire) == bufPtr) {
        atomic_store_explicit(&node->cache_ref_bit, 1, memory_order_relaxed);
        atomic_store_explicit(&node->reg_ref_bit, 1, memory_order_relaxed);
        if (outSize)*outSize = size;
        return (void*)bufPtr;
      }
      sHazptrRelease(self, hazSlot);
      continue;
    }
    if (atomic_load_explicit(&node->is_loaded, memory_order_acquire)) {
      if (atomic_load_explicit(&node->buffer, memory_order_acquire) != 0) continue;
      if (atomic_load_explicit(&node->size_bytes, memory_order_acquire) != 0) continue;
      sHazptrRelease(self, hazSlot);
      return (void*)&sEmptyFileBuffer;
    }
    uint32_t st = atomic_load_explicit(&node->state, memory_order_acquire);
    if (st != FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE) {
      sHazptrRelease(self, hazSlot);
      errno = st == FILELOADERCONCURRENTACCESS_FILE_STATE_DELETED ? ESTALE : ENOENT;
      return NULL;
    }
    uint32_t flags = atomic_load_explicit(&node->flags, memory_order_relaxed);
    if (!(flags & (FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD | FILELOADERCONCURRENTACCESS_FLAG_LOADING)))
      atomic_fetch_or_explicit(&node->flags, FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD, memory_order_acq_rel);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (((now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L) > (long)timeoutMs) {
      sHazptrRelease(self, hazSlot);
      errno = ETIMEDOUT;
      return NULL;
    }
    usleep(10000);
  }
}

PUBLIC_API void *
FileLoaderServiceGetFileContentByPath(FileLoaderService *service_handle, const char *path, size_t *outSize)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  uint32_t                    blacklisted = 0;

  if (outSize) *outSize = 0;
  if (!self || !path || !*path) {
    errno = EINVAL;
    return NULL;
  }

  if (LocklessFixedWidthHashMap_Get(&self->blacklist_map, path, &blacklisted)) {
    errno = EACCES;
    return NULL;
  }

  FileLoaderServiceFileHandle handle = sExistingHandle(self, path);
  if (!handle) {
    errno = ENOENT;
    return NULL;
  }

  return FileLoaderServiceGetFileContent(service_handle, handle, outSize);
}

PUBLIC_API void
FileLoaderServiceReturnFile(FileLoaderService *service_handle, void *buf)
{
  FileLoaderConcurrentAccess *self = SELF(service_handle);
  struct sHazptrThreadState  *st;

  if (!self || !buf)  return;
  pthread_once(&sHazptrKeyOnce, sMakeHazptrKey);
  st = (struct sHazptrThreadState*)pthread_getspecific(sHazptrKey);
  if (st && st->owner == self) {
    atomic_store_explicit(&self->hazptrs[st->slot], 0, memory_order_release);
    return;
  }
  for (uint32_t i = 0; i < self->hazptr_slot_count; i++)
    if (atomic_load_explicit(&self->hazptrs[i], memory_order_relaxed) == (uintptr_t)buf) {
      atomic_store_explicit(&self->hazptrs[i], 0, memory_order_release);
      return;
    }
}

PUBLIC_API void FileLoaderServiceStop(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  if (f)sReapMonitorThread(f);
}

PUBLIC_API uint32_t FileLoaderServiceSize(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  return f ? LocklessFixedWidthHashMap_Size(&f->file_map) : 0;
}

PUBLIC_API uint32_t FileLoaderServiceCapacity(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  return (f && f->pool_capacity > 0) ? f->pool_capacity - 1 : 0;
}

PUBLIC_API uint64_t FileLoaderServiceHeartbeat(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  return f ? atomic_load_explicit(&f->monitor_heartbeat, memory_order_relaxed) : 0;
}

static const char *
sFileStateName(uint32_t state)
{
  switch (state) {
  case FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE: return "active";
  case FILELOADERCONCURRENTACCESS_FILE_STATE_DELETED: return "deleted";
  case FILELOADERCONCURRENTACCESS_FILE_STATE_BLACKLISTED: return "blacklisted";
  case FILELOADERCONCURRENTACCESS_FILE_STATE_TOO_LARGE: return "too_large";
  case FILELOADERCONCURRENTACCESS_FILE_STATE_LOAD_ERROR: return "load_error";
  case FILELOADERCONCURRENTACCESS_FILE_STATE_EVICTED: return "evicted";
  default: return "unknown";
  }
}

static void
sAppendJsonPath(BufferDescriptor *bd, const char *path)
{
  char   escaped[PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH * 2];
  size_t o = 0;
  for (size_t i = 0; path[i] != '\0' && o + 7 < sizeof escaped; i++) {
    unsigned char c = (unsigned char)path[i];
    if (c == '"' || c == '\\') {
      escaped[o++] = '\\';
      escaped[o++] = (char)c;
    } else if (c < 0x20) {
      int n = snprintf(escaped + o, sizeof escaped - o, "\\u%04x", (unsigned)c);
      if (n < 0) break;
      o += (size_t)n;
    } else {
      escaped[o++] = (char)c;
    }
  }
  escaped[o] = '\0';
  BufferDescriptorAppendFormatted(bd, "\"%s\"", escaped);
}

PUBLIC_API BufferDescriptor *
FileLoaderServiceDescribe(const FileLoaderService *service_handle, BufferDescriptor *provided)
{
  const FileLoaderConcurrentAccess *self = (const FileLoaderConcurrentAccess *)service_handle;

  if (!provided) {
    provided = calloc(1, sizeof(BufferDescriptor));
    if (!provided) return NULL;
    BufferDescriptorInit(provided, 256);
  }
  if (!self || !self->node_pool) {
    BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
    return provided;
  }

  BufferDescriptorAppendFormatted(provided,
    "{\"size\":%u,\"capacity\":%u,\"loaded\":%u,\"heartbeat\":%" PRIu64 ","
    "\"running\":%s,\"logger\":\"%s\",\"buffer_cache_capacity\":%u,"
    "\"key_width\":%u,\"max_file_size_bytes\":%zu,\"default_buffer_timeout_ms\":%u,"
    "\"hazptr_slots\":%u,\"limbo_capacity\":%u",
    LocklessFixedWidthHashMap_Size(&self->file_map),
    self->pool_capacity > 0 ? self->pool_capacity - 1 : 0,
    atomic_load_explicit(&self->cache_loaded_count, memory_order_relaxed),
    atomic_load_explicit(&self->monitor_heartbeat, memory_order_relaxed),
    atomic_load_explicit(&self->running, memory_order_relaxed) ? "true" : "false",
    self->uf_logger ? "enabled" : "none",
    self->buffer_cache_capacity, self->key_width,
    self->max_file_size_bytes, self->default_buffer_timeout_ms,
    self->hazptr_slot_count, self->limbo_capacity);

  BufferDescriptorAppendFormatted(provided, ",\"entries\":[");
  bool emitted = false;
  for (uint32_t i = 1; i < self->pool_capacity; i++) {
    const FileLoaderConcurrentAccessNode *node =
      (const FileLoaderConcurrentAccessNode *)(self->node_pool + (size_t)i * self->node_stride);
    if (!atomic_load_explicit(&node->is_active, memory_order_acquire)) continue;
    uint32_t node_flags = atomic_load_explicit(&node->flags, memory_order_relaxed);
    BufferDescriptorAppendFormatted(provided, "%s{\"index\":%u,\"path\":", emitted ? "," : "", i);
    sAppendJsonPath(provided, node->path);
    BufferDescriptorAppendFormatted(provided,
      ",\"state\":\"%s\",\"generation\":%" PRIu64 ",\"size_bytes\":%zu,\"loaded\":%s,\"needs_load\":%s,\"in_cache\":%s}",
      sFileStateName(atomic_load_explicit(&node->state, memory_order_relaxed)),
      atomic_load_explicit(&node->generation, memory_order_relaxed),
      atomic_load_explicit(&node->size_bytes, memory_order_relaxed),
      atomic_load_explicit(&node->buffer, memory_order_relaxed) ? "true" : "false",
      (node_flags & FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD) ? "true" : "false",
      (node_flags & FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE) ? "true" : "false");
    emitted = true;
  }
  BufferDescriptorAppendFormatted(provided, "]}\n");
  return provided;
}

PUBLIC_API pthread_t FileLoaderServiceGetThread(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  return f ? f->monitor_thread : (pthread_t)0;
}

PUBLIC_API bool FileLoaderServiceRegisterThread(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  return f && sHazptrAcquire(f) >= 0;
}

PUBLIC_API void FileLoaderServiceUnregisterThread(FileLoaderService *s)
{
  FileLoaderConcurrentAccess *f = SELF(s);
  if (!f)return;
  pthread_once(&sHazptrKeyOnce, sMakeHazptrKey);
  struct sHazptrThreadState *st = (struct sHazptrThreadState*)pthread_getspecific(sHazptrKey);
  if (st && st->owner == f) {
    sHazptrRelease(f, st->slot);
    if (st->slot >= 0 && (uint32_t)st->slot < f->hazptr_slot_count)
      atomic_store_explicit(&f->hazptr_reserved[st->slot], false, memory_order_release);
    st->owner = NULL;
    st->slot  = -1;
  }
}
