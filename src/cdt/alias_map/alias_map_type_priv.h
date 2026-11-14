/**
 * @file alias_map_type_priv.h
 * @brief AliasMap — type definitions.  Never installed; colocated with the source.
 *
 * The public header exposes an opaque AliasMap handle only.  Everything defined
 * here is implementation detail and may change without notice.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_TYPE_PRIV_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_TYPE_PRIV_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/logger/logger_type.h>

struct AliasMapEntry;

typedef struct AliasMap AliasMap;

typedef void (*AliasMapLogFn)(AliasMap *map_ptr, UfLoggerLevel level, const char *file_ptr,
    int line, const char *function_ptr, const char *format_ptr, ...)
    __attribute__((format(printf, 6, 7)));

/*!
 * One alias.  Every node lives on two lists at once: its hash bucket chain
 * (singly linked through next) and its group's alias list (doubly linked
 * through next_alias / prev_alias, so a single alias unlinks in O(1)).
 */
typedef struct AliasMapNode {
    char                *key;
    uint64_t             hash;
    struct AliasMapEntry *entry;
    struct AliasMapNode *next;
    struct AliasMapNode *next_alias;
    struct AliasMapNode *prev_alias;
} AliasMapNode;

/*!
 * One group: the shared value and the aliases that reach it.  An entry exists
 * exactly while alias_count > 0, so no separate reference count is needed.
 * Live entries are also chained, in creation order, on the map's entry list.
 */
typedef struct AliasMapEntry {
    char          *value;
    AliasMapNode  *aliases;
    size_t         alias_count;
    struct AliasMapEntry *next_entry;
    struct AliasMapEntry *prev_entry;
} AliasMapEntry;

/*!
 * The map.  bucket_count is always a power of two and at least
 * PRIV_CONFIG_DEFAULT_ALIASMAP_INITIAL_BUCKET_COUNT; every node sits in the
 * chain at hash % bucket_count.  All fields are guarded by lock.
 */
struct AliasMap {
    pthread_rwlock_t  lock;
    AliasMapNode    **buckets;
    size_t            bucket_count;
    size_t            alias_count;
    AliasMapEntry    *entries;      ///< head of the creation-order entry list
    AliasMapEntry    *entries_tail; ///< tail, for O(1) append
    UfLogger         *logger_ptr;   ///< borrowed; never freed by the map
    AliasMapOrigin    origin;       ///< fixed at construction

    /*! Logger multiplexer, bound once at construction: sLogForward when a logger
     * was supplied, sLogSink otherwise.  Call sites never test logger_ptr. */
    AliasMapLogFn     log;
};

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_TYPE_PRIV_H */
