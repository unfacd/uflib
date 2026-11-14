/**
 * @file alias_map_snapshot.c
 * @brief AliasMapSnapshot — immutable copy of every group and alias.
 *
 * The snapshot is one allocation:
 *
 *   [ struct | values[G] | alias_offsets[G+1] | names[A] | string bytes ]
 *
 * Every pointer targets the trailing byte block, so AliasMapSnapshotDestroy()
 * is a single free().  The map is read once under its read lock while the copy
 * is assembled; the result outlives the lock and the map.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/cdt/alias_map/alias_map.h>

#include "alias_map_type_priv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct AliasMapSnapshot {
    size_t        group_count;
    size_t        alias_count;
    const char  **values;        /* [group_count]     -> into blob */
    size_t       *alias_offsets; /* [group_count + 1] prefix offsets into names */
    const char  **names;         /* [alias_count]     -> into blob */
    char         *blob;          /* every string, group-major */
};

static bool
sAddOverflows(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right) return false;

    *out = left + right;

    return true;
}

static bool
sMulOverflows(size_t count, size_t size, size_t *out)
{
    if (size && count > SIZE_MAX / size) return false;

    *out = count * size;

    return true;
}

PUBLIC_API AliasMapSnapshot *
AliasMapSnapshotCreate(AliasMap *map_ptr)
{
    if (!map_ptr) return NULL;

    pthread_rwlock_rdlock(&map_ptr->lock);

    size_t groups       = 0;
    size_t aliases      = 0;
    size_t string_bytes = 0;

    for (AliasMapEntry *entry = map_ptr->entries; entry; entry = entry->next_entry) {
        size_t len = strlen(entry->value) + 1;

        if (!sAddOverflows(string_bytes, len, &string_bytes) ||
            !sAddOverflows(aliases, entry->alias_count, &aliases))
            goto overflow;

        groups++;

        for (AliasMapNode *node = entry->aliases; node; node = node->next_alias) {
            len = strlen(node->key) + 1;

            if (!sAddOverflows(string_bytes, len, &string_bytes)) goto overflow;
        }
    }

    size_t offset_entries;
    size_t values_bytes;
    size_t offsets_bytes;
    size_t names_bytes;
    size_t total;

    if (!sAddOverflows(groups, 1, &offset_entries) ||
        !sMulOverflows(groups, sizeof(const char *), &values_bytes) ||
        !sMulOverflows(offset_entries, sizeof(size_t), &offsets_bytes) ||
        !sMulOverflows(aliases, sizeof(const char *), &names_bytes) ||
        !sAddOverflows(sizeof(AliasMapSnapshot), values_bytes, &total) ||
        !sAddOverflows(total, offsets_bytes, &total) ||
        !sAddOverflows(total, names_bytes, &total) ||
        !sAddOverflows(total, string_bytes, &total))
        goto overflow;

    AliasMapSnapshot *snapshot_ptr = malloc(total);

    if (!snapshot_ptr) goto overflow;

    char *cursor = (char *)(snapshot_ptr + 1);

    snapshot_ptr->group_count = groups;
    snapshot_ptr->alias_count = aliases;

    snapshot_ptr->values = (const char **)cursor;
    cursor += values_bytes;

    snapshot_ptr->alias_offsets = (size_t *)cursor;
    cursor += offsets_bytes;

    snapshot_ptr->names = (const char **)cursor;
    cursor += names_bytes;

    snapshot_ptr->blob = cursor;

    size_t group = 0;
    size_t alias = 0;
    char  *write = snapshot_ptr->blob;

    for (AliasMapEntry *entry = map_ptr->entries; entry; entry = entry->next_entry) {
        size_t len = strlen(entry->value) + 1;

        memcpy(write, entry->value, len);
        snapshot_ptr->values[group] = write;
        write += len;

        snapshot_ptr->alias_offsets[group] = alias;

        for (AliasMapNode *node = entry->aliases; node; node = node->next_alias) {
            len = strlen(node->key) + 1;

            memcpy(write, node->key, len);
            snapshot_ptr->names[alias++] = write;
            write += len;
        }

        group++;
    }

    snapshot_ptr->alias_offsets[groups] = aliases;

    pthread_rwlock_unlock(&map_ptr->lock);

    return snapshot_ptr;

overflow:
    pthread_rwlock_unlock(&map_ptr->lock);

    return NULL;
}

PUBLIC_API void
AliasMapSnapshotDestroy(AliasMapSnapshot *snapshot_ptr)
{
    free(snapshot_ptr);
}

PUBLIC_API size_t
AliasMapSnapshotGroupCount(const AliasMapSnapshot *snapshot_ptr)
{
    return snapshot_ptr ? snapshot_ptr->group_count : 0;
}

PUBLIC_API size_t
AliasMapSnapshotAliasCount(const AliasMapSnapshot *snapshot_ptr)
{
    return snapshot_ptr ? snapshot_ptr->alias_count : 0;
}

PUBLIC_API const char *
AliasMapSnapshotValueAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index)
{
    if (!snapshot_ptr || group_index >= snapshot_ptr->group_count) return NULL;

    return snapshot_ptr->values[group_index];
}

PUBLIC_API size_t
AliasMapSnapshotAliasCountAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index)
{
    if (!snapshot_ptr || group_index >= snapshot_ptr->group_count) return 0;

    return snapshot_ptr->alias_offsets[group_index + 1] - snapshot_ptr->alias_offsets[group_index];
}

PUBLIC_API const char *
AliasMapSnapshotAliasAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index,
                        size_t alias_index)
{
    if (!snapshot_ptr || group_index >= snapshot_ptr->group_count) return NULL;

    if (alias_index >= AliasMapSnapshotAliasCountAt(snapshot_ptr, group_index)) return NULL;

    return snapshot_ptr->names[snapshot_ptr->alias_offsets[group_index] + alias_index];
}
