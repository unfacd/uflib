/**
 * @file alias_map.c
 * @brief AliasMap implementation — see alias_map.h for the public contract.
 *
 * Structure:
 *
 *   buckets[] --chain--> AliasMapNode { key, hash, entry } ----+
 *                                                              v
 *   AliasMapEntry { value, aliases -> Node <-> Node <-> ..., alias_count }
 *
 *   entries --next_entry--> AliasMapEntry --next_entry--> ... (creation order)
 *
 * Every alias is a node that lives on two lists at once: its hash bucket chain
 * (singly linked) and its group's alias list (doubly linked, for O(1)
 * single-alias removal).  An entry exists iff its alias_count > 0, so there is
 * no separate reference count.  Live entries are chained on a third, doubly
 * linked list in creation order; that list is what makes the map enumerable.
 *
 * Locking: every public function takes map_ptr->lock exactly once and releases
 * it before returning.  The descriptor constructor is the exception: the map is
 * private until it returns, so it builds without locking.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "alias_map_priv.h"

#include <uflib/cdt/alias_map/alias_map.h>
#include <uflib/config_defs_uflib.h>
#include <uflib/logger/logger.h>

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ── Small helpers (no locking) ────────────────────────────────────────── */

static uint64_t
sHashStr(const char *str)
{
    uint64_t hash = 14695981039346656037ULL;

    for (; *str; str++) {
        hash ^= (unsigned char)*str;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static char *
sDupStr(const char *str)
{
    size_t len = strlen(str) + 1;
    char  *copy = malloc(len);

    if (copy) memcpy(copy, str, len);

    return copy;
}

static AliasMapNode *
sFind(const AliasMap *map_ptr, const char *key, uint64_t hash)
{
    for (AliasMapNode *node = map_ptr->buckets[hash % map_ptr->bucket_count];
         node; node = node->next)
        if (node->hash == hash && strcmp(node->key, key) == 0) return node;

    return NULL;
}

static bool
sGrow(AliasMap *map_ptr)
{
    if (map_ptr->bucket_count > SIZE_MAX / 2 / sizeof(AliasMapNode *)) return false;

    size_t         new_count   = map_ptr->bucket_count * 2;
    AliasMapNode **new_buckets = calloc(new_count, sizeof *new_buckets);

    if (!new_buckets) return false;

    for (size_t i = 0; i < map_ptr->bucket_count; i++) {
        for (AliasMapNode *node = map_ptr->buckets[i], *next; node; node = next) {
            next = node->next;
            size_t idx = node->hash % new_count;
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
        }
    }

    free(map_ptr->buckets);
    map_ptr->buckets      = new_buckets;
    map_ptr->bucket_count = new_count;

    return true;
}

/*!
 * Grow ahead of a batch so no rehash happens mid-insert.  Best effort:
 * separate chaining tolerates a load factor above 0.75.
 */
static void
sReserve(AliasMap *map_ptr, size_t extra)
{
    size_t threshold = map_ptr->bucket_count - map_ptr->bucket_count / 4;

    while (map_ptr->alias_count + extra > threshold) {
        if (!sGrow(map_ptr)) return;
        threshold = map_ptr->bucket_count - map_ptr->bucket_count / 4;
    }
}

static void
sUnlinkFromBucket(AliasMap *map_ptr, AliasMapNode *target)
{
    AliasMapNode **link = &map_ptr->buckets[target->hash % map_ptr->bucket_count];

    while (*link != target) link = &(*link)->next;

    *link = target->next;
}

static void
sUnlinkFromEntry(AliasMapNode *node)
{
    AliasMapEntry *entry = node->entry;

    if (node->prev_alias) node->prev_alias->next_alias = node->next_alias;
    else                  entry->aliases = node->next_alias;

    if (node->next_alias) node->next_alias->prev_alias = node->prev_alias;

    entry->alias_count--;
}

/* ── Entry and node lifetime ───────────────────────────────────────────── */

/*! Takes ownership of an already-duplicated value string. */
static AliasMapEntry *
sEntryNew(char *value)
{
    AliasMapEntry *entry = malloc(sizeof *entry);

    if (!entry) return NULL;

    entry->value       = value;
    entry->aliases     = NULL;
    entry->alias_count = 0;
    entry->next_entry  = NULL;
    entry->prev_entry  = NULL;

    return entry;
}

/*! Append entry to the map's creation-order list (write lock or map ownership). */
static void
sEntryLink(AliasMap *map_ptr, AliasMapEntry *entry)
{
    entry->prev_entry = map_ptr->entries_tail;
    entry->next_entry = NULL;

    if (map_ptr->entries_tail) map_ptr->entries_tail->next_entry = entry;
    else                       map_ptr->entries = entry;

    map_ptr->entries_tail = entry;
}

/*! Remove entry from the creation-order list; a no-op when it is not linked. */
static void
sEntryUnlink(AliasMap *map_ptr, AliasMapEntry *entry)
{
    if (!entry->prev_entry && map_ptr->entries != entry) return;

    if (entry->prev_entry) entry->prev_entry->next_entry = entry->next_entry;
    else                   map_ptr->entries = entry->next_entry;

    if (entry->next_entry) entry->next_entry->prev_entry = entry->prev_entry;
    else                   map_ptr->entries_tail = entry->prev_entry;

    entry->prev_entry = NULL;
    entry->next_entry = NULL;
}

/*!
 * Create an alias for entry.  Fails without side effects if the key already
 * exists anywhere in the map or if an allocation fails.
 */
static bool
sBind(AliasMap *map_ptr, const char *key, AliasMapEntry *entry)
{
    uint64_t hash = sHashStr(key);

    if (sFind(map_ptr, key, hash)) return false;

    AliasMapNode *node = malloc(sizeof *node);

    if (!node) return false;

    node->key = sDupStr(key);
    if (!node->key) {
        free(node);
        return false;
    }

    node->hash  = hash;
    node->entry = entry;

    size_t idx = hash % map_ptr->bucket_count;
    node->next            = map_ptr->buckets[idx];
    map_ptr->buckets[idx] = node;

    node->prev_alias = NULL;
    node->next_alias = entry->aliases;
    if (entry->aliases) entry->aliases->prev_alias = node;
    entry->aliases = node;
    entry->alias_count++;
    map_ptr->alias_count++;

    return true;
}

/*!
 * Unlink and free every alias of entry, then the entry itself.  Works on
 * partially built entries, including ones with zero aliases.
 *
 * @return Number of aliases removed.
 */
static size_t
sDestroyEntry(AliasMap *map_ptr, AliasMapEntry *entry)
{
    size_t removed = 0;

    sEntryUnlink(map_ptr, entry);

    for (AliasMapNode *node = entry->aliases, *next; node; node = next) {
        next = node->next_alias;
        sUnlinkFromBucket(map_ptr, node);
        free(node->key);
        free(node);
        map_ptr->alias_count--;
        removed++;
    }

    free(entry->value);
    free(entry);

    return removed;
}

/* ── Logger multiplexer ────────────────────────────────────────────────── */

/*! Silences a map that was created without a logger. */
static void
sLogSink(AliasMap *map_ptr, UfLoggerLevel level, const char *file_ptr, int line,
         const char *function_ptr, const char *format_ptr, ...)
{
    (void)map_ptr;
    (void)level;
    (void)file_ptr;
    (void)line;
    (void)function_ptr;
    (void)format_ptr;
}

/*! Forwards a record to the caller-owned logger. */
static void
sLogForward(AliasMap *map_ptr, UfLoggerLevel level, const char *file_ptr, int line,
            const char *function_ptr, const char *format_ptr, ...)
{
    va_list args;

    va_start(args, format_ptr);
    UfLoggerVLog(map_ptr->logger_ptr, level, file_ptr, line, function_ptr, format_ptr, args);
    va_end(args);
}

void
AliasMapLogBind(AliasMap *map_ptr, UfLogger *logger_ptr)
{
    map_ptr->logger_ptr = logger_ptr;
    map_ptr->log        = logger_ptr ? sLogForward : sLogSink;
}

/* ── Create-result and describe helpers ────────────────────────────────── */

static const char *
sOriginName(AliasMapOrigin origin)
{
    switch (origin) {
    case ALIASMAP_ORIGIN_INCREMENTAL: return "incremental";
    case ALIASMAP_ORIGIN_DESCRIPTOR:  return "descriptor";
    case ALIASMAP_ORIGIN_JSON:        return "json";
    default:                          return "none";
    }
}

const char *
AliasMapCreateStatusName(AliasMapCreateStatus status)
{
    switch (status) {
    case ALIASMAP_CREATE_OK:               return "ok";
    case ALIASMAP_CREATE_INVALID_ARGUMENT: return "invalid_argument";
    case ALIASMAP_CREATE_INVALID_JSON:     return "invalid_json";
    case ALIASMAP_CREATE_DUPLICATE_ALIAS:  return "duplicate_alias";
    case ALIASMAP_CREATE_NO_MEMORY:        return "no_memory";
    default:                               return "unknown";
    }
}

void
AliasMapResultFill(AliasMapCreateResult *result_ptr, AliasMapCreateStatus status,
                  size_t failed_collector, size_t failed_alias,
                  size_t conflict_collector, size_t conflict_alias,
                  size_t group_count, size_t alias_count)
{
    if (!result_ptr) return;

    *result_ptr = (AliasMapCreateResult){
        .status             = status,
        .failed_collector   = failed_collector,
        .failed_alias       = failed_alias,
        .conflict_collector = conflict_collector,
        .conflict_alias     = conflict_alias,
        .json_offset        = SIZE_MAX,
        .group_count        = group_count,
        .alias_count        = alias_count,
    };
}

static void
sAppendJsonString(BufferDescriptor *bd, const char *str_ptr)
{
    BufferDescriptorAppendFormatted(bd, "\"");

    for (const unsigned char *p = (const unsigned char *)str_ptr; *p; p++) {
        switch (*p) {
        case '"':  BufferDescriptorAppendFormatted(bd, "\\\""); break;
        case '\\': BufferDescriptorAppendFormatted(bd, "\\\\"); break;
        case '\n': BufferDescriptorAppendFormatted(bd, "\\n");  break;
        case '\r': BufferDescriptorAppendFormatted(bd, "\\r");  break;
        case '\t': BufferDescriptorAppendFormatted(bd, "\\t");  break;
        default:
            if (*p < 0x20) BufferDescriptorAppendFormatted(bd, "\\u%04x", *p);
            else           BufferDescriptorAppendFormatted(bd, "%c", (char)*p);
        }
    }

    BufferDescriptorAppendFormatted(bd, "\"");
}

/*!
 * Emit the bounded group listing.  Caller holds the read lock.
 *
 * @return true when at least one group was omitted because the cap was hit.
 */
static bool
sEmitGroupDetail(BufferDescriptor *bd, const AliasMap *map_ptr)
{
    size_t emitted   = 0;
    bool   truncated = false;

    BufferDescriptorAppendFormatted(bd, "\"groups_detail\":[");

    for (size_t i = 0; i < map_ptr->bucket_count && !truncated; i++) {
        for (AliasMapNode *node = map_ptr->buckets[i]; node; node = node->next) {
            AliasMapEntry *entry = node->entry;

            if (entry->aliases != node) continue; /* emit each group once */

            if (emitted + entry->alias_count > PRIV_CONFIG_DEFAULT_ALIASMAP_DESCRIBE_MAX_ALIASES) {
                truncated = true;
                break;
            }

            if (emitted) BufferDescriptorAppendFormatted(bd, ",");

            BufferDescriptorAppendFormatted(bd, "{\"value\":");
            sAppendJsonString(bd, entry->value);
            BufferDescriptorAppendFormatted(bd, ",\"aliases\":[");

            bool first = true;

            for (AliasMapNode *alias = entry->aliases; alias; alias = alias->next_alias) {
                if (!first) BufferDescriptorAppendFormatted(bd, ",");
                sAppendJsonString(bd, alias->key);
                first = false;
            }

            BufferDescriptorAppendFormatted(bd, "]}");

            emitted += entry->alias_count;
        }
    }

    BufferDescriptorAppendFormatted(bd, "]");

    return truncated;
}

/* ── Public API: lifecycle ─────────────────────────────────────────────── */

PUBLIC_API AliasMap *
AliasMapCreate(void)
{
    return AliasMapCreateInternal(NULL, ALIASMAP_ORIGIN_INCREMENTAL);
}

PUBLIC_API AliasMap *
AliasMapCreateWithLogger(UfLogger *logger_ptr)
{
    AliasMapDescriptor descriptor = {
        .collectors_ptr  = NULL,
        .collector_count = 0,
        .logger_ptr      = logger_ptr,
    };

    return AliasMapCreateInternal(&descriptor, ALIASMAP_ORIGIN_INCREMENTAL);
}

PUBLIC_API AliasMap *
AliasMapCreateWithDescriptor(AliasMapDescriptor *descriptor_ptr)
{
    return AliasMapCreateInternal(descriptor_ptr, ALIASMAP_ORIGIN_DESCRIPTOR);
}

PUBLIC_API AliasMapOrigin
AliasMapGetOrigin(AliasMap *map_ptr)
{
    if (!map_ptr) return ALIASMAP_ORIGIN_NONE;

    pthread_rwlock_rdlock(&map_ptr->lock);

    AliasMapOrigin origin = map_ptr->origin;

    pthread_rwlock_unlock(&map_ptr->lock);

    return origin;
}

/*! Pick a bucket count that keeps the load factor at or below 0.75. */
static size_t
sBucketCountFor(size_t alias_count)
{
    if (alias_count > SIZE_MAX / 4) return 0;

    size_t needed = alias_count * 4 / 3;
    size_t count  = PRIV_CONFIG_DEFAULT_ALIASMAP_INITIAL_BUCKET_COUNT;

    while (count < needed) {
        if (count > SIZE_MAX / 2) return 0;
        count *= 2;
    }

    return count;
}

/*! One alias reference, used to detect duplicates inside a descriptor. */
typedef struct {
    const char *alias_ptr;
    size_t      collector_index;
    size_t      alias_index;
} AliasMapAliasRef;

static int
sCompareAliasRef(const void *left_ptr, const void *right_ptr)
{
    const AliasMapAliasRef *left  = left_ptr;
    const AliasMapAliasRef *right = right_ptr;

    int cmp = strcmp(left->alias_ptr, right->alias_ptr);

    if (cmp) return cmp;

    if (left->collector_index != right->collector_index)
        return left->collector_index < right->collector_index ? -1 : 1;

    if (left->alias_index != right->alias_index)
        return left->alias_index < right->alias_index ? -1 : 1;

    return 0;
}

AliasMap *
AliasMapCreateInternal(AliasMapDescriptor *descriptor_ptr, AliasMapOrigin origin)
{
    const AliasMapCollector *collectors_ptr  = descriptor_ptr ? descriptor_ptr->collectors_ptr : NULL;
    size_t                   collector_count = descriptor_ptr ? descriptor_ptr->collector_count : 0;
    UfLogger                *logger_ptr      = descriptor_ptr ? descriptor_ptr->logger_ptr : NULL;

    /* Same multiplexer shape as FileLoaderConcurrentAccess: the call sites never
     * test the logger pointer, they call through map_ptr->log. */
    AliasMap log_ctx = {0};

    AliasMapLogBind(&log_ctx, logger_ptr);

    AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_OK,
                      SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);

    if (collector_count && !collectors_ptr) {
        AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_INVALID_ARGUMENT,
                          0, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
        log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "alias map rejected: status=%s (no collector array)",
                        AliasMapCreateStatusName(ALIASMAP_CREATE_INVALID_ARGUMENT));
        return NULL;
    }

    size_t total_aliases = 0;

    for (size_t c = 0; c < collector_count; c++) {
        const AliasMapCollector *collector_ptr = &collectors_ptr[c];

        if (!collector_ptr->value_ptr || !collector_ptr->aliases_ptr ||
            collector_ptr->alias_count == 0) {
            AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_INVALID_ARGUMENT,
                              c, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
            log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                            "alias map rejected: status=%s collector=%zu (missing value or aliases)",
                            AliasMapCreateStatusName(ALIASMAP_CREATE_INVALID_ARGUMENT), c);
            return NULL;
        }

        /* The sum is checked before the alias array is walked: an alias_count
         * large enough to overflow is not a real array, so iterating it would
         * read past whatever the caller passed. */
        if (collector_ptr->alias_count > SIZE_MAX - total_aliases) {
            AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_INVALID_ARGUMENT,
                              c, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
            log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                            "alias map rejected: status=%s collector=%zu (alias count overflow)",
                            AliasMapCreateStatusName(ALIASMAP_CREATE_INVALID_ARGUMENT), c);
            return NULL;
        }

        total_aliases += collector_ptr->alias_count;
    }

    for (size_t c = 0; c < collector_count; c++) {
        const AliasMapCollector *collector_ptr = &collectors_ptr[c];

        for (size_t a = 0; a < collector_ptr->alias_count; a++) {
            if (!collector_ptr->aliases_ptr[a]) {
                AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_INVALID_ARGUMENT,
                                  c, a, SIZE_MAX, SIZE_MAX, 0, 0);
                log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                                "alias map rejected: status=%s collector=%zu alias=%zu (NULL alias)",
                                AliasMapCreateStatusName(ALIASMAP_CREATE_INVALID_ARGUMENT), c, a);
                return NULL;
            }
        }
    }

    /* Duplicates are rejected before anything is allocated. */
    if (total_aliases > 1) {
        if (total_aliases > SIZE_MAX / sizeof(AliasMapAliasRef)) {
            AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_NO_MEMORY,
                              SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
            log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "alias map rejected: status=%s (alias count overflow)",
                            AliasMapCreateStatusName(ALIASMAP_CREATE_NO_MEMORY));
            return NULL;
        }

        AliasMapAliasRef *refs_ptr = malloc(total_aliases * sizeof *refs_ptr);

        if (!refs_ptr) {
            AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_NO_MEMORY,
                              SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
            log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "alias map rejected: status=%s (index allocation)",
                            AliasMapCreateStatusName(ALIASMAP_CREATE_NO_MEMORY));
            return NULL;
        }

        size_t ref_count = 0;

        for (size_t c = 0; c < collector_count; c++)
            for (size_t a = 0; a < collectors_ptr[c].alias_count; a++)
                refs_ptr[ref_count++] = (AliasMapAliasRef){
                    .alias_ptr       = collectors_ptr[c].aliases_ptr[a],
                    .collector_index = c,
                    .alias_index     = a,
                };

        qsort(refs_ptr, ref_count, sizeof *refs_ptr, sCompareAliasRef);

        for (size_t i = 1; i < ref_count; i++) {
            if (strcmp(refs_ptr[i - 1].alias_ptr, refs_ptr[i].alias_ptr) != 0) continue;

            const AliasMapAliasRef *conflict = &refs_ptr[i - 1];
            const AliasMapAliasRef *failed   = &refs_ptr[i];

            AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_DUPLICATE_ALIAS,
                              failed->collector_index, failed->alias_index,
                              conflict->collector_index, conflict->alias_index, 0, 0);
            log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                            "alias map rejected: status=%s alias='%s' collector=%zu alias=%zu "
                            "conflicts with collector=%zu alias=%zu",
                            AliasMapCreateStatusName(ALIASMAP_CREATE_DUPLICATE_ALIAS), failed->alias_ptr,
                            failed->collector_index, failed->alias_index,
                            conflict->collector_index, conflict->alias_index);
            free(refs_ptr);
            return NULL;
        }

        free(refs_ptr);
    }

    size_t bucket_count = sBucketCountFor(total_aliases);

    if (bucket_count == 0) {
        AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_NO_MEMORY,
                          SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);
        log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "alias map rejected: status=%s (bucket sizing overflow)",
                        AliasMapCreateStatusName(ALIASMAP_CREATE_NO_MEMORY));
        return NULL;
    }

    AliasMap       *map_ptr          = calloc(1, sizeof *map_ptr);
    AliasMapEntry **staged_entries   = collector_count
                                         ? calloc(collector_count, sizeof *staged_entries)
                                         : NULL;
    size_t          failed_collector = SIZE_MAX;
    size_t          failed_alias     = SIZE_MAX;

    if (!map_ptr || (collector_count && !staged_entries)) goto no_memory;

    map_ptr->logger_ptr   = logger_ptr;
    map_ptr->log          = log_ctx.log;
    map_ptr->origin       = origin;
    map_ptr->bucket_count = bucket_count;
    map_ptr->buckets      = calloc(bucket_count, sizeof *map_ptr->buckets);

    if (!map_ptr->buckets) goto no_memory;

    for (size_t c = 0; c < collector_count; c++) {
        char *value_copy = sDupStr(collectors_ptr[c].value_ptr);

        if (!value_copy) {
            failed_collector = c;
            goto no_memory;
        }

        staged_entries[c] = sEntryNew(value_copy);

        if (!staged_entries[c]) {
            free(value_copy);
            failed_collector = c;
            goto no_memory;
        }
    }

    for (size_t c = 0; c < collector_count; c++) {
        AliasMapEntry *entry = staged_entries[c];

        for (size_t a = 0; a < collectors_ptr[c].alias_count; a++) {
            const char *alias_ptr = collectors_ptr[c].aliases_ptr[a];

            failed_collector = c;
            failed_alias     = a;

            AliasMapNode *node = malloc(sizeof *node);

            if (!node) goto no_memory;

            node->key = sDupStr(alias_ptr);

            if (!node->key) {
                free(node);
                goto no_memory;
            }

            node->hash  = sHashStr(alias_ptr);
            node->entry = entry;

            size_t idx = node->hash % bucket_count;

            node->next            = map_ptr->buckets[idx];
            map_ptr->buckets[idx] = node;

            node->prev_alias = NULL;
            node->next_alias = entry->aliases;
            if (entry->aliases) entry->aliases->prev_alias = node;
            entry->aliases = node;
            entry->alias_count++;
            map_ptr->alias_count++;
        }
    }

    for (size_t c = 0; c < collector_count; c++) sEntryLink(map_ptr, staged_entries[c]);

    if (pthread_rwlock_init(&map_ptr->lock, NULL) != 0) {
        failed_collector = SIZE_MAX;
        failed_alias     = SIZE_MAX;
        goto no_memory;
    }

    free(staged_entries);
    staged_entries = NULL;

    map_ptr->logger_ptr = logger_ptr;

    AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_OK,
                      SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                      collector_count, total_aliases);

    if (UfLoggerIsLevelEnabled(map_ptr->logger_ptr, UF_LOGGER_LEVEL_DEBUG)) {
        BufferDescriptor *description = AliasMapDescribe(map_ptr, NULL);

        if (description) {
            map_ptr->log(map_ptr, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                         "created %s", description->data);
            BufferDescriptorRelease(description);
            free(description);
        }
    }

    return map_ptr;

no_memory:
    if (map_ptr && map_ptr->buckets) {
        for (size_t i = 0; i < map_ptr->bucket_count; i++) {
            for (AliasMapNode *node = map_ptr->buckets[i], *next; node; node = next) {
                next = node->next;
                free(node->key);
                free(node);
            }
        }
    }

    if (staged_entries) {
        for (size_t c = 0; c < collector_count; c++) {
            if (!staged_entries[c]) continue;

            free(staged_entries[c]->value);
            free(staged_entries[c]);
        }
    }

    free(staged_entries);

    if (map_ptr) free(map_ptr->buckets);

    free(map_ptr);

    AliasMapResultFill(descriptor_ptr ? &descriptor_ptr->result : NULL, ALIASMAP_CREATE_NO_MEMORY,
                      failed_collector, failed_alias,
                      SIZE_MAX, SIZE_MAX, 0, 0);
    log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "alias map rejected: status=%s collector=%zu alias=%zu",
                    AliasMapCreateStatusName(ALIASMAP_CREATE_NO_MEMORY), failed_collector, failed_alias);

    return NULL;
}

PUBLIC_API void
AliasMapDestroy(AliasMap *map_ptr)
{
    if (!map_ptr) return;

    /* No other thread may be using the map, so no locking is needed. */
    for (size_t i = 0; i < map_ptr->bucket_count; i++) {
        for (AliasMapNode *node = map_ptr->buckets[i], *next; node; node = next) {
            next = node->next;

            AliasMapEntry *entry = node->entry;

            if (--entry->alias_count == 0) { /* last alias frees the group */
                free(entry->value);
                free(entry);
            }

            free(node->key);
            free(node);
        }
    }

    pthread_rwlock_destroy(&map_ptr->lock);
    free(map_ptr->buckets);
    free(map_ptr);
}

/* ── Public API: groups ────────────────────────────────────────────────── */

PUBLIC_API bool
AliasMapAdd(AliasMap *map_ptr, const char *const *aliases_ptr, size_t alias_count,
            const char *value_ptr)
{
    if (!map_ptr || !aliases_ptr || !value_ptr || alias_count == 0) return false;

    for (size_t i = 0; i < alias_count; i++)
        if (!aliases_ptr[i]) return false;

    /* Everything that allocates runs outside the critical section. */
    char *value = sDupStr(value_ptr);

    if (!value) {
        map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "add failed: out of memory (value)");
        return false;
    }

    AliasMapEntry *entry = sEntryNew(value);

    if (!entry) {
        free(value);
        map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "add failed: out of memory (group)");
        return false;
    }

    pthread_rwlock_wrlock(&map_ptr->lock);

    sEntryLink(map_ptr, entry);

    sReserve(map_ptr, alias_count);

    bool   ok        = true;
    bool   duplicate = false;
    char  *failed_ptr = NULL;

    for (size_t i = 0; i < alias_count; i++) {
        if (!sBind(map_ptr, aliases_ptr[i], entry)) {
            ok         = false;
            failed_ptr = (char *)aliases_ptr[i];
            duplicate  = sFind(map_ptr, failed_ptr, sHashStr(failed_ptr)) != NULL;
            break;
        }
    }

    if (!ok) sDestroyEntry(map_ptr, entry); /* roll back; frees entry too */

    pthread_rwlock_unlock(&map_ptr->lock);

    if (!ok) {
        if (duplicate)
            map_ptr->log(map_ptr, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "add rejected: alias '%s' already exists", failed_ptr);
        else
            map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "add failed: out of memory (alias '%s')", failed_ptr);
    }

    return ok;
}

PUBLIC_API bool
AliasMapAddAlias(AliasMap *map_ptr, const char *existing_alias_ptr,
                 const char *new_alias_ptr)
{
    if (!map_ptr || !existing_alias_ptr || !new_alias_ptr) return false;

    pthread_rwlock_wrlock(&map_ptr->lock);

    AliasMapNode *node      = sFind(map_ptr, existing_alias_ptr, sHashStr(existing_alias_ptr));
    bool          ok        = false;
    bool          duplicate = false;

    if (node) {
        sReserve(map_ptr, 1);
        ok = sBind(map_ptr, new_alias_ptr, node->entry);

        if (!ok) duplicate = sFind(map_ptr, new_alias_ptr, sHashStr(new_alias_ptr)) != NULL;
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    if (node && !ok) {
        if (duplicate)
            map_ptr->log(map_ptr, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__, "add alias rejected: '%s' already exists", new_alias_ptr);
        else
            map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "add alias failed: out of memory ('%s')", new_alias_ptr);
    }

    return ok;
}

/* ── Public API: lookup ────────────────────────────────────────────────── */

PUBLIC_API char *
AliasMapGetCopy(AliasMap *map_ptr, const char *alias_ptr)
{
    if (!map_ptr || !alias_ptr) return NULL;

    uint64_t hash = sHashStr(alias_ptr); /* hashing needs no lock */

    pthread_rwlock_rdlock(&map_ptr->lock);

    AliasMapNode *node  = sFind(map_ptr, alias_ptr, hash);
    char         *value = node ? sDupStr(node->entry->value) : NULL;

    pthread_rwlock_unlock(&map_ptr->lock);

    if (node && !value) map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "get failed: out of memory (value)");

    return value;
}

PUBLIC_API bool
AliasMapGetInto(AliasMap *map_ptr, const char *alias_ptr, char *buffer_ptr,
                size_t buffer_sz, size_t *out_len_ptr)
{
    if (out_len_ptr) *out_len_ptr = 0;

    if (!map_ptr || !alias_ptr || !buffer_ptr || buffer_sz == 0) {
        errno = EINVAL;
        return false;
    }

    uint64_t hash = sHashStr(alias_ptr);

    pthread_rwlock_rdlock(&map_ptr->lock);

    AliasMapNode *node = sFind(map_ptr, alias_ptr, hash);
    bool          ok   = false;

    if (!node) {
        errno = ENOENT;
    }
    else {
        size_t len = strlen(node->entry->value);

        if (len + 1 > buffer_sz) {
            errno = ENOSPC;
        }
        else {
            memcpy(buffer_ptr, node->entry->value, len + 1);
            if (out_len_ptr) *out_len_ptr = len;
            ok = true;
        }
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    return ok;
}

PUBLIC_API bool
AliasMapContains(AliasMap *map_ptr, const char *alias_ptr)
{
    if (!map_ptr || !alias_ptr) return false;

    uint64_t hash = sHashStr(alias_ptr);

    pthread_rwlock_rdlock(&map_ptr->lock);

    bool found = sFind(map_ptr, alias_ptr, hash) != NULL;

    pthread_rwlock_unlock(&map_ptr->lock);

    return found;
}

/* ── Public API: mutation ──────────────────────────────────────────────── */

PUBLIC_API bool
AliasMapSetValue(AliasMap *map_ptr, const char *alias_ptr, const char *value_ptr)
{
    if (!map_ptr || !alias_ptr || !value_ptr) return false;

    char *value = sDupStr(value_ptr); /* allocate outside the critical section */

    if (!value) {
        map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "set value failed: out of memory");
        return false;
    }

    uint64_t hash  = sHashStr(alias_ptr);
    char    *old   = NULL;
    bool     found = false;

    pthread_rwlock_wrlock(&map_ptr->lock);

    AliasMapNode *node = sFind(map_ptr, alias_ptr, hash);

    if (node) {
        old = node->entry->value;
        node->entry->value = value;
        found = true;
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    if (!found) {
        free(value);
        return false;
    }

    free(old); /* free outside the lock */

    return true;
}

PUBLIC_API bool
AliasMapRemove(AliasMap *map_ptr, const char *alias_ptr)
{
    if (!map_ptr || !alias_ptr) return false;

    uint64_t hash = sHashStr(alias_ptr);

    pthread_rwlock_wrlock(&map_ptr->lock);

    AliasMapNode  *node       = sFind(map_ptr, alias_ptr, hash);
    char          *dead_value = NULL;
    AliasMapEntry *dead_entry = NULL;

    if (node) {
        AliasMapEntry *entry = node->entry;

        sUnlinkFromBucket(map_ptr, node);
        sUnlinkFromEntry(node);
        map_ptr->alias_count--;

        if (entry->alias_count == 0) { /* that was the last alias */
            sEntryUnlink(map_ptr, entry);
            dead_value = entry->value;
            dead_entry = entry;
        }
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    if (!node) return false;

    free(dead_value);
    free(dead_entry);
    free(node->key);
    free(node);

    return true;
}

PUBLIC_API size_t
AliasMapRemoveValue(AliasMap *map_ptr, const char *alias_ptr)
{
    if (!map_ptr || !alias_ptr) return 0;

    pthread_rwlock_wrlock(&map_ptr->lock);

    AliasMapNode *start   = sFind(map_ptr, alias_ptr, sHashStr(alias_ptr));
    size_t        removed = start ? sDestroyEntry(map_ptr, start->entry) : 0;

    pthread_rwlock_unlock(&map_ptr->lock);

    return removed;
}

PUBLIC_API char **
AliasMapAliasesOf(AliasMap *map_ptr, const char *alias_ptr, size_t *out_count_ptr)
{
    if (out_count_ptr) *out_count_ptr = 0;

    if (!map_ptr || !alias_ptr) return NULL;

    uint64_t hash = sHashStr(alias_ptr);
    char   **out  = NULL;

    pthread_rwlock_rdlock(&map_ptr->lock);

    AliasMapNode *start = sFind(map_ptr, alias_ptr, hash);

    if (start) {
        AliasMapEntry *entry = start->entry;

        size_t bytes = entry->alias_count * sizeof *out;

        for (AliasMapNode *node = entry->aliases; node; node = node->next_alias)
            bytes += strlen(node->key) + 1;

        out = malloc(bytes);

        if (out) {
            char  *p = (char *)(out + entry->alias_count); /* strings follow pointers */
            size_t i = 0;

            for (AliasMapNode *node = entry->aliases; node; node = node->next_alias) {
                size_t len = strlen(node->key) + 1;
                memcpy(p, node->key, len);
                out[i++] = p;
                p += len;
            }

            if (out_count_ptr) *out_count_ptr = i;
        }
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    if (start && !out) map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "aliases describe failed: out of memory");

    return out;
}

/* ── Public API: enumeration ───────────────────────────────────────────── */

PUBLIC_API bool
AliasMapForEach(AliasMap *map_ptr, AliasMapForEachFn fn_ptr, void *context_ptr)
{
    if (!map_ptr || !fn_ptr) return false;

    pthread_rwlock_rdlock(&map_ptr->lock);

    bool completed = true;

    for (AliasMapEntry *entry = map_ptr->entries; entry && completed; entry = entry->next_entry)
        for (AliasMapNode *node = entry->aliases; node; node = node->next_alias)
            if (!fn_ptr(context_ptr, entry->value, node->key)) {
                completed = false;
                break;
            }

    pthread_rwlock_unlock(&map_ptr->lock);

    return completed;
}

/*! A node that is not yet in the table.  entry == NULL marks it as fresh. */
static AliasMapNode *
sNodeNew(const char *key)
{
    AliasMapNode *node = calloc(1, sizeof *node);

    if (!node) return NULL;

    node->key = sDupStr(key);

    if (!node->key) {
        free(node);
        return NULL;
    }

    node->hash = sHashStr(key);

    return node;
}

PUBLIC_API bool
AliasMapReplace(AliasMap *map_ptr, const char *const *aliases_ptr, size_t alias_count,
                const char *value_ptr)
{
    if (!map_ptr || !aliases_ptr || !value_ptr || alias_count == 0) return false;

    for (size_t i = 0; i < alias_count; i++) {
        if (!aliases_ptr[i]) return false;

        for (size_t j = 0; j < i; j++) /* reject duplicates */
            if (strcmp(aliases_ptr[i], aliases_ptr[j]) == 0) {
                map_ptr->log(map_ptr, UF_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                                "replace rejected: duplicate alias '%s' in list", aliases_ptr[i]);
                return false;
            }
    }

    /* Everything that can fail and needs no lock happens first. */
    char          *value = sDupStr(value_ptr);
    AliasMapEntry *entry = value ? sEntryNew(value) : NULL;
    AliasMapNode **slots = calloc(alias_count, sizeof *slots);

    if (!entry || !slots) {
        free(slots);
        free(entry);
        free(value);
        map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "replace failed: out of memory (staging)");
        return false;
    }

    pthread_rwlock_wrlock(&map_ptr->lock);

    sReserve(map_ptr, alias_count); /* may rehash; bucket_count is stable after this */

    /* Phase 1 (may fail): reuse existing nodes, allocate only the fresh ones. */
    bool ok = true;

    for (size_t i = 0; i < alias_count && ok; i++) {
        slots[i] = sFind(map_ptr, aliases_ptr[i], sHashStr(aliases_ptr[i]));

        if (!slots[i]) ok = (slots[i] = sNodeNew(aliases_ptr[i])) != NULL;
    }

    if (!ok) {
        for (size_t i = 0; i < alias_count; i++)
            if (slots[i] && !slots[i]->entry) {
                free(slots[i]->key);
                free(slots[i]);
            }

        pthread_rwlock_unlock(&map_ptr->lock);
        free(slots);
        free(entry);
        free(value);

        map_ptr->log(map_ptr, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__, "replace failed: out of memory (node)");

        return false;
    }

    /* Phase 2 (cannot fail): relink everything and free emptied old groups. */
    sEntryLink(map_ptr, entry);

    for (size_t i = 0; i < alias_count; i++) {
        AliasMapNode *node = slots[i];

        if (node->entry) { /* existing: leave its old group */
            AliasMapEntry *old = node->entry;

            sUnlinkFromEntry(node);

            if (old->alias_count == 0) {
                sEntryUnlink(map_ptr, old);
                free(old->value);
                free(old);
            }
        }
        else { /* fresh: join the table */
            size_t idx = node->hash % map_ptr->bucket_count;

            node->next            = map_ptr->buckets[idx];
            map_ptr->buckets[idx] = node;
            map_ptr->alias_count++;
        }

        node->entry      = entry;
        node->prev_alias = NULL;
        node->next_alias = entry->aliases;

        if (entry->aliases) entry->aliases->prev_alias = node;

        entry->aliases = node;
        entry->alias_count++;
    }

    pthread_rwlock_unlock(&map_ptr->lock);

    free(slots);

    return true;
}

PUBLIC_API size_t
AliasMapAliasCount(AliasMap *map_ptr)
{
    if (!map_ptr) return 0;

    pthread_rwlock_rdlock(&map_ptr->lock);

    size_t count = map_ptr->alias_count;

    pthread_rwlock_unlock(&map_ptr->lock);

    return count;
}

/* ── Public API: describe ──────────────────────────────────────────────── */

PUBLIC_API BufferDescriptor *
AliasMapDescribe(AliasMap *map_ptr, BufferDescriptor *provided_ptr)
{
    BufferDescriptor *bd = provided_ptr;

    if (!bd) {
        bd = calloc(1, sizeof *bd);

        if (!bd) return NULL;

        BufferDescriptorInit(bd, 256);
    }

    if (!map_ptr) {
        BufferDescriptorAppendFormatted(bd, "{\"error\":\"null handle\"}\n");
        return bd;
    }

    pthread_rwlock_rdlock(&map_ptr->lock);

    size_t group_count = 0;
    size_t min_alias   = 0;
    size_t max_alias   = 0;
    size_t max_chain   = 0;
    size_t key_bytes   = 0;
    size_t value_bytes = 0;

    for (size_t i = 0; i < map_ptr->bucket_count; i++) {
        size_t chain = 0;

        for (AliasMapNode *node = map_ptr->buckets[i]; node; node = node->next) {
            chain++;
            key_bytes += strlen(node->key);

            if (node->entry->aliases != node) continue; /* group head only */

            size_t aliases = node->entry->alias_count;

            if (group_count == 0 || aliases < min_alias) min_alias = aliases;
            if (aliases > max_alias) max_alias = aliases;

            value_bytes += strlen(node->entry->value);
            group_count++;
        }

        if (chain > max_chain) max_chain = chain;
    }

    double load_factor  = map_ptr->bucket_count
                            ? (double)map_ptr->alias_count / (double)map_ptr->bucket_count
                            : 0.0;
    double mean_aliases = group_count
                            ? (double)map_ptr->alias_count / (double)group_count
                            : 0.0;

    BufferDescriptorAppendFormatted(bd,
        "{\"groups\":%zu,\"aliases\":%zu,\"buckets\":%zu,\"load_factor\":%.2f,"
        "\"groups_min\":%zu,\"groups_max\":%zu,\"groups_mean\":%.2f,"
        "\"max_chain\":%zu,\"hash\":\"fnv1a-64\",\"logger\":\"%s\","
        "\"origin\":\"%s\",\"bulk\":%s,\"key_bytes\":%zu,\"value_bytes\":%zu,",
        group_count, map_ptr->alias_count, map_ptr->bucket_count, load_factor,
        min_alias, max_alias, mean_aliases, max_chain,
        map_ptr->logger_ptr ? "enabled" : "none",
        sOriginName(map_ptr->origin),
        (map_ptr->origin == ALIASMAP_ORIGIN_DESCRIPTOR ||
         map_ptr->origin == ALIASMAP_ORIGIN_JSON) ? "true" : "false",
        key_bytes, value_bytes);

    bool truncated = sEmitGroupDetail(bd, map_ptr);

    pthread_rwlock_unlock(&map_ptr->lock);

    BufferDescriptorAppendFormatted(bd, ",\"truncated\":%s}\n", truncated ? "true" : "false");

    return bd;
}
