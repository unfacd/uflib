/**
 * @file alias_map.h
 * @brief AliasMap — thread-safe many-to-one map from alias strings to a shared value.
 *
 * AliasMap partitions a set of alias strings into groups; every group maps to
 * exactly one value string.  Aliases are handles to the shared value: adding or
 * removing one alias never disturbs the other members of its group, and a group
 * exists exactly while it has at least one alias.
 *
 * The map is opaque.  All state lives behind an opaque @c AliasMap handle;
 * no pointer into the map ever escapes a call.  Lookups return a private copy,
 * either @c malloc'd (AliasMapGetCopy()) or written into a caller-supplied
 * buffer (AliasMapGetInto()).
 *
 * Thread safety: one pthread read/write lock per map.  Reads run concurrently;
 * writes are exclusive; every public call is linearizable.  AliasMapDestroy()
 * is the exception — the caller must guarantee no other thread is using the map
 * or waiting on its lock.
 *
 * String semantics: aliases and values are byte-wise, case-sensitive C strings
 * with no normalisation.  The empty string is a legal alias and a legal value.
 * The map copies every alias and value it retains.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/cdt/alias_map/alias_map_descriptor_type.h>
#include <uflib/cdt/alias_map/alias_map_json_descriptor_type.h>
#include <uflib/cdt/alias_map/alias_map_origin_enum.h>
#include <uflib/uflib_defs.h>

typedef struct AliasMap AliasMap;
typedef struct AliasMapSnapshot AliasMapSnapshot;

/**
 * @brief Create an empty map.
 *
 * @return A new map, or NULL on allocation or lock-initialisation failure.
 *
 * @code{.c}
 * AliasMap *map_ptr = AliasMapCreate();
 * if (!map_ptr) return -1;
 * @endcode
 */
PUBLIC_API AliasMap *
AliasMapCreate(void);

/**
 * @brief Create an empty map with a logger bound.
 *
 * For callers that populate the map with AliasMapAdd() rather than a descriptor
 * but still want diagnostics.  The logger is bound once, at construction: there
 * is no setter, so a map's logging destination never changes behind the back of
 * a concurrent reader.
 *
 * @param logger_ptr Caller-owned logger; must outlive the map.  NULL creates a
 *                   silent map, exactly like AliasMapCreate().
 * @return The new map, or NULL when allocation fails.
 *
 * @code{.c}
 * AliasMap *map_ptr = AliasMapCreateWithLogger(logger_ptr);
 * AliasMapAdd(map_ptr, aliases, count, "/opt/etc/ufsrv/assets/greeting.txt");
 * @endcode
 */
PUBLIC_API AliasMap *
AliasMapCreateWithLogger(UfLogger *logger_ptr);

/**
 * @brief Create a map in bulk from a descriptor.
 *
 * The map is private until this call returns, so no lock is taken while it is
 * populated: either the whole descriptor is applied and a fully formed map is
 * returned, or NULL is returned and nothing is left allocated.  The resulting
 * map is an ordinary map — every other function in this header works on it
 * unchanged.
 *
 * Duplicate aliases within the descriptor are rejected before any allocation;
 * there is no pre-existing state to conflict with.  Every alias and value is
 * copied, so the descriptor and its strings may be released immediately.
 *
 * @param descriptor_ptr Descriptor to apply; NULL builds an empty map.  Its
 *                       @c result field is always written, including on
 *                       success, so failures can be reported precisely.
 * @return The new map, or NULL when the descriptor is invalid, contains a
 *         duplicate alias, or an allocation fails.
 *
 * @code{.c}
 * static const char *const kGreeting[] = { "greeting", "hello" };
 * static const AliasMapCollector kCollectors[] = {
 *     { .value_ptr = "/opt/etc/ufsrv/assets/greeting.txt",
 *       .aliases_ptr = kGreeting, .alias_count = 2 },
 * };
 *
 * AliasMapDescriptor descriptor = {
 *     .collectors_ptr  = kCollectors,
 *     .collector_count = 1,
 *     .logger_ptr      = logger_ptr,
 * };
 *
 * AliasMap *map_ptr = AliasMapCreateWithDescriptor(&descriptor);
 * if (!map_ptr) {
 *     // descriptor.result.status says why
 * }
 * @endcode
 */
PUBLIC_API AliasMap *
AliasMapCreateWithDescriptor(AliasMapDescriptor *descriptor_ptr);

/**
 * @brief Create a map in bulk from a JSON document.
 *
 * The document is an array of group objects, the same shape the configuration
 * file uses per registry entry:
 *
 * @code{.json}
 * [
 *   {"value": "/opt/etc/ufsrv/assets/greeting.txt", "keys": ["greeting", "hello"]},
 *   {"value": "/opt/etc/ufsrv/assets/motd.txt",     "keys": ["motd"]}
 * ]
 * @endcode
 *
 * The contract is strict: an array of objects; each object has exactly one
 * `value` string and one non-empty `keys` array of strings; unknown fields,
 * repeated fields, wrong types and trailing data are rejected; `\u` escapes
 * and surrogate pairs are decoded.  An empty array builds an empty map.
 *
 * As with AliasMapCreateWithDescriptor() the map is private until the call
 * returns, every string is copied, and the result field is always written —
 * including the byte offset for JSON and shape errors.
 *
 * @param descriptor_ptr Descriptor holding the document and optional logger.
 * @return The new map, or NULL when the document is malformed, off-contract,
 *         contains a duplicate alias, or an allocation fails.
 *
 * @code{.c}
 * AliasMapJsonDescriptor descriptor = {
 *     .json_ptr   = "[{\"value\":\"/p/greeting.txt\",\"keys\":[\"greeting\"]}]",
 *     .logger_ptr = logger_ptr,
 * };
 *
 * AliasMap *map_ptr = AliasMapCreateWithJsonDescriptor(&descriptor);
 * if (!map_ptr) {
 *     // descriptor.result.status / json_offset say why
 * }
 * @endcode
 */
PUBLIC_API AliasMap *
AliasMapCreateWithJsonDescriptor(AliasMapJsonDescriptor *descriptor_ptr);

/**
 * @brief Destroy a map and everything it owns.
 *
 * @param map_ptr Map to destroy; NULL is a no-op.
 *
 * @warning Does not lock.  No other thread may be using the map, or blocked on
 *          its lock, when this is called.
 */
PUBLIC_API void
AliasMapDestroy(AliasMap *map_ptr);

/**
 * @brief Create one new group reachable through every supplied alias.
 *
 * All-or-nothing: fails and leaves the map unchanged if any alias already
 * exists, if the same alias appears twice in @p aliases_ptr, or if an
 * allocation fails.
 *
 * @param map_ptr      Initialised map.
 * @param aliases_ptr  Array of alias_count non-NULL C strings.
 * @param alias_count  Number of aliases; must be at least 1.
 * @param value_ptr    Value shared by the new group.
 * @return true on success, false otherwise.
 *
 * @code{.c}
 * const char *ll[] = { "ll", "lsl", "longlist" };
 * if (!AliasMapAdd(map_ptr, ll, 3, "ls -la")) return -1;
 * @endcode
 */
PUBLIC_API bool
AliasMapAdd(AliasMap *map_ptr, const char *const *aliases_ptr, size_t alias_count,
            const char *value_ptr);

/**
 * @brief Attach new_alias_ptr to the group that existing_alias_ptr belongs to.
 *
 * Not idempotent: fails if new_alias_ptr already exists anywhere in the map,
 * including in that same group.
 *
 * @param map_ptr            Initialised map.
 * @param existing_alias_ptr Alias whose group receives the new member.
 * @param new_alias_ptr      Alias to attach; must not already exist.
 * @return true on success, false otherwise.
 */
PUBLIC_API bool
AliasMapAddAlias(AliasMap *map_ptr, const char *existing_alias_ptr,
                 const char *new_alias_ptr);

/**
 * @brief Copy the value reachable through alias_ptr into a new allocation.
 *
 * @param map_ptr   Initialised map.
 * @param alias_ptr Alias to resolve.
 * @return A malloc'd copy of the value, or NULL if the alias is absent or on
 *         allocation failure.  The caller must free() the result.  An empty
 *         value is returned as "" — never NULL.
 */
PUBLIC_API char *
AliasMapGetCopy(AliasMap *map_ptr, const char *alias_ptr);

/**
 * @brief Copy the value reachable through alias_ptr into a caller buffer.
 *
 * On success the buffer holds the value as a NUL-terminated string.  On
 * failure @p buffer_ptr is untouched and errno is set: ENOENT when the alias is
 * absent, ENOSPC when the value does not fit, EINVAL for invalid arguments.
 *
 * @param map_ptr     Initialised map.
 * @param alias_ptr   Alias to resolve.
 * @param buffer_ptr  Destination buffer.
 * @param buffer_sz   Capacity of @p buffer_ptr in bytes; must be at least 1.
 * @param out_len_ptr Receives the value length in bytes, excluding the NUL
 *                    terminator; may be NULL.
 * @return true on success, false otherwise.
 *
 * @code{.c}
 * char path[PATH_MAX];
 * if (!AliasMapGetInto(map_ptr, "greeting", path, sizeof path, NULL)) return 404;
 * @endcode
 */
PUBLIC_API bool
AliasMapGetInto(AliasMap *map_ptr, const char *alias_ptr, char *buffer_ptr,
                size_t buffer_sz, size_t *out_len_ptr);

/**
 * @brief Test whether an alias exists.
 *
 * @param map_ptr   Initialised map.
 * @param alias_ptr Alias to test.
 * @return true when the alias is present, false otherwise (including NULL
 *         arguments).  Never allocates.
 */
PUBLIC_API bool
AliasMapContains(AliasMap *map_ptr, const char *alias_ptr);

/**
 * @brief Replace the value of the whole group that alias_ptr belongs to.
 *
 * Every alias in the group observes the new value at the same instant.  Fails
 * and leaves the map unchanged if the alias is absent or on allocation
 * failure.
 *
 * @param map_ptr   Initialised map.
 * @param alias_ptr Any member of the group.
 * @param value_ptr New value for the group.
 * @return true on success, false otherwise.
 */
PUBLIC_API bool
AliasMapSetValue(AliasMap *map_ptr, const char *alias_ptr, const char *value_ptr);

/**
 * @brief Remove a single alias.
 *
 * The other aliases of the group stay valid.  Removing the last alias destroys
 * the group and its value.
 *
 * @param map_ptr   Initialised map.
 * @param alias_ptr Alias to remove.
 * @return true when the alias was present and removed, false otherwise.
 */
PUBLIC_API bool
AliasMapRemove(AliasMap *map_ptr, const char *alias_ptr);

/**
 * @brief Remove the group that alias_ptr belongs to, with all of its aliases.
 *
 * Allocates nothing and so cannot fail midway.
 *
 * @param map_ptr   Initialised map.
 * @param alias_ptr Any member of the group to destroy.
 * @return Number of aliases removed; 0 when the alias is absent.
 */
PUBLIC_API size_t
AliasMapRemoveValue(AliasMap *map_ptr, const char *alias_ptr);

/**
 * @brief Take a snapshot of every alias in alias_ptr's group, including itself.
 *
 * The pointer array and the string bytes live in ONE allocation:
 * @verbatim [ char*0 | char*1 | ... | char*k-1 | "str0\0" "str1\0" ... ] @endverbatim
 * Release it with a single free() on the returned pointer.  The array is not
 * NULL-terminated; use @p out_count_ptr.  Order is most recently attached first.
 *
 * @param map_ptr       Initialised map.
 * @param alias_ptr     Any member of the group.
 * @param out_count_ptr Receives the number of aliases; set to 0 on every
 *                      failure path; may be NULL.
 * @return The snapshot allocation, or NULL when the alias is absent or on
 *         allocation failure.
 */
PUBLIC_API char **
AliasMapAliasesOf(AliasMap *map_ptr, const char *alias_ptr, size_t *out_count_ptr);

/**
 * @brief Atomically make all supplied aliases resolve to a new group.
 *
 * An alias that already exists is moved out of its old group into the new one;
 * the remaining members of that old group are untouched, and a group left with
 * no aliases is freed.  All-or-nothing: fails and leaves the map unchanged on
 * invalid arguments, a duplicate alias within the list, or allocation failure.
 *
 * @param map_ptr      Initialised map.
 * @param aliases_ptr  Array of alias_count distinct non-NULL C strings.
 * @param alias_count  Number of aliases; must be at least 1.
 * @param value_ptr    Value shared by the new group.
 * @return true on success, false otherwise.
 *
 * @code{.c}
 * const char *detach[] = { "lsl" };
 * AliasMapReplace(map_ptr, detach, 1, "ls -l");
 * @endcode
 */
PUBLIC_API bool
AliasMapReplace(AliasMap *map_ptr, const char *const *aliases_ptr, size_t alias_count,
                const char *value_ptr);

/**
 * @brief Total number of aliases in the map, not the number of groups.
 *
 * @param map_ptr Initialised map.
 * @return Alias count; 0 for a NULL map.
 */
PUBLIC_API size_t
AliasMapAliasCount(AliasMap *map_ptr);

/**
 * @brief Report how the map was instantiated.
 *
 * Fixed at construction and never changed: a map that was bulk loaded through
 * a descriptor or a JSON document reports that origin for its whole life, even
 * if more aliases are added afterwards.
 *
 * @param map_ptr Map to inspect.
 * @return The origin, or ALIASMAP_ORIGIN_NONE for a NULL map.
 */
PUBLIC_API AliasMapOrigin
AliasMapGetOrigin(AliasMap *map_ptr);

/**
 * @brief Copy every group and alias into an immutable, caller-owned snapshot.
 *
 * Takes the read lock once, copies every value and every alias, and releases
 * the lock before returning, so the snapshot stays valid after the map is
 * mutated or destroyed.  Groups appear in creation order; aliases appear most
 * recently attached first, the order AliasMapAliasesOf() reports.  The whole
 * snapshot is one allocation: release it with AliasMapSnapshotDestroy().
 *
 * @param map_ptr Initialised map.
 * @return The snapshot, or NULL for a NULL map or allocation failure.
 *
 * @code{.c}
 * AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);
 *
 * if (snapshot_ptr) {
 *     for (size_t g = 0; g < AliasMapSnapshotGroupCount(snapshot_ptr); g++) {
 *         printf("%s\n", AliasMapSnapshotValueAt(snapshot_ptr, g));
 *
 *         for (size_t a = 0; a < AliasMapSnapshotAliasCountAt(snapshot_ptr, g); a++)
 *             printf("  %s\n", AliasMapSnapshotAliasAt(snapshot_ptr, g, a));
 *     }
 *
 *     AliasMapSnapshotDestroy(snapshot_ptr);
 * }
 * @endcode
 */
PUBLIC_API AliasMapSnapshot *
AliasMapSnapshotCreate(AliasMap *map_ptr);

/**
 * @brief Release a snapshot.
 *
 * @param snapshot_ptr Snapshot from AliasMapSnapshotCreate(); NULL-safe.
 */
PUBLIC_API void
AliasMapSnapshotDestroy(AliasMapSnapshot *snapshot_ptr);

/**
 * @brief Number of groups captured by the snapshot.
 *
 * @param snapshot_ptr Snapshot to read; may be NULL.
 * @return Group count; 0 for a NULL snapshot.
 */
PUBLIC_API size_t
AliasMapSnapshotGroupCount(const AliasMapSnapshot *snapshot_ptr);

/**
 * @brief Total number of aliases captured by the snapshot.
 *
 * @param snapshot_ptr Snapshot to read; may be NULL.
 * @return Alias count; 0 for a NULL snapshot.
 */
PUBLIC_API size_t
AliasMapSnapshotAliasCount(const AliasMapSnapshot *snapshot_ptr);

/**
 * @brief Value shared by one group.
 *
 * @param snapshot_ptr Snapshot to read.
 * @param group_index  Zero-based group index.
 * @return The value, or NULL when the index is out of range.
 */
PUBLIC_API const char *
AliasMapSnapshotValueAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index);

/**
 * @brief Number of aliases in one group.
 *
 * @param snapshot_ptr Snapshot to read.
 * @param group_index  Zero-based group index.
 * @return Alias count; 0 when the index is out of range.
 */
PUBLIC_API size_t
AliasMapSnapshotAliasCountAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index);

/**
 * @brief One alias name within one group.
 *
 * @param snapshot_ptr Snapshot to read.
 * @param group_index  Zero-based group index.
 * @param alias_index  Zero-based position within the group.
 * @return The alias, or NULL when either index is out of range.
 */
PUBLIC_API const char *
AliasMapSnapshotAliasAt(const AliasMapSnapshot *snapshot_ptr, size_t group_index,
                        size_t alias_index);

/**
 * @brief Visitor invoked once per alias by AliasMapForEach().
 *
 * @param context_ptr Caller context, passed through unchanged.
 * @param value_ptr   Value shared by the alias's group.
 * @param alias_ptr   The alias itself.
 * @return true to continue the walk, false to stop it.
 *
 * Both strings are borrowed from the map and are valid only for the duration of
 * the call; the visitor runs under the map's read lock, so it must not call any
 * AliasMap function.
 */
typedef bool (*AliasMapForEachFn)(void *context_ptr, const char *value_ptr,
                                  const char *alias_ptr);

/**
 * @brief Visit every alias in the map without allocating or copying.
 *
 * The streaming counterpart of AliasMapSnapshotCreate(): same order and same
 * contents, one indirect call per alias and no memory of its own.  Groups are
 * visited in creation order and every alias of a group is contiguous; aliases
 * within a group are visited most recently attached first.
 *
 * The read lock is held for the whole walk, so the visitor must return quickly
 * and must not call back into the map or block.  Work that needs either — file
 * I/O, logging through the map, nested lookups — should use a snapshot instead.
 *
 * @param map_ptr      Initialised map.
 * @param fn_ptr       Visitor to invoke per alias.
 * @param context_ptr  Passed to every visit unchanged; may be NULL.
 * @return true when every alias was visited, false when the map or visitor is
 *         NULL or the visitor stopped the walk.
 *
 * @code{.c}
 * static bool print(void *context_ptr, const char *value_ptr, const char *alias_ptr)
 * {
 *     printf("%s -> %s\n", alias_ptr, value_ptr);
 *     return true; /\* false stops the walk *\/
 * }
 *
 * AliasMapForEach(map_ptr, print, NULL);
 * @endcode
 */
PUBLIC_API bool
AliasMapForEach(AliasMap *map_ptr, AliasMapForEachFn fn_ptr, void *context_ptr);

/**
 * @brief Describe the map's metadata as JSON.
 *
 * Reports the construction origin, group and alias counts, key/value byte
 * totals, bucket geometry and group-size statistics, followed by a bounded
 * listing of the groups.  The listing is capped at
 * PRIV_CONFIG_DEFAULT_ALIASMAP_DESCRIBE_MAX_ALIASES aliases; when the map is
 * larger the payload carries @c "truncated":true.  Aliases and values are
 * JSON-escaped.
 *
 * Takes the read lock and walks every bucket, so the call is O(aliases +
 * buckets) and blocks writers for that duration.  Intended for diagnostics and
 * operator tooling, not for a hot path.
 *
 * @param map_ptr         Map to describe; NULL produces an error object.
 * @param provided_ptr    Optional caller-owned descriptor to append to.  When
 *                        NULL a descriptor is allocated and initialised, which
 *                        the caller releases with BufferDescriptorRelease()
 *                        and then free().
 * @return The populated descriptor, or NULL only when a fresh one could not be
 *         allocated.
 *
 * @code{.c}
 * BufferDescriptor *description = AliasMapDescribe(map_ptr, NULL);
 * if (description) {
 *     printf("%s\n", description->data);
 *     BufferDescriptorRelease(description);
 *     free(description);
 * }
 * @endcode
 */
PUBLIC_API BufferDescriptor *
AliasMapDescribe(AliasMap *map_ptr, BufferDescriptor *provided_ptr);

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_H */
