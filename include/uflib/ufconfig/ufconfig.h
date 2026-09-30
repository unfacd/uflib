/**
 * @file ufconfig.h
 * @brief The ufconfig module's application-facing interface.
 *
 * This header is the whole of what a consumer needs.  Include it and the public
 * types, the schema contract, the field identifiers generated from the
 * consumer's own schema, and the setters for mutable fields come with it.
 *
 * The library holds no schema.  It is machinery — a parser, a validator, a
 * canonicaliser and a driver layer — and the schema is generated from the
 * consumer's files, compiled into the consumer, and handed across at
 * @ref UfConfigCreate.
 *
 * @code{.c}
 * #include <uflib/ufconfig/ufconfig.h>
 *
 * UfConfigDescriptor descriptor = {
 *     .fields      = g_ufconfig_fields,
 *     .field_count = (size_t)g_ufconfig_field_count,
 *     .lookup      = UfConfigLookupPath,
 *     .uf_logger   = NULL,
 * };
 * UfConfig *h = NULL;
 * if (UfConfigCreate(&h, &descriptor) != UF_CONFIG_OK) { ... }
 *
 * UfConfigLoadOptions opt = { .size = sizeof(opt), .version = 1,
 *                             .mode = UF_CONFIG_LOAD_STRICT };
 * UfConfigStatus st = UfConfigLoadFile(h, "etc/ufsrvwebsock.config.lua", &opt, NULL);
 * if (st != UF_CONFIG_OK) {
 *     const UfConfigError *e = UfConfigLastError();
 *     fprintf(stderr, "%s: %s (%s:%d)\n", UfConfigStatusString(st),
 *             e->message, e->field_path ? e->field_path : "-", e->line);
 * }
 *
 * UfConfigValue v;
 * if (UfConfigGetField(h, "ufsrv.main_listener_port", &v) == UF_CONFIG_OK) {
 *     printf("port %lld\n", (long long)v.as.i);
 * }
 *
 * UfConfigDestroy(h);
 * @endcode
 *
 * ## What a consumer never sees
 *
 * No implementation detail crosses this boundary.  The node representation, the
 * arena that owns it, the index that finds a field, the parsed tree shape, the
 * canonical-form rules and the digest algorithm are all private to
 * @c src/ufconfig/ and are not reachable from an installed header — @c src/ is
 * on the library's PRIVATE include path, and the install rule copies
 * @c include/ and nothing else.
 *
 * The driver interface in @ref ufconfig_type.h is the deliberate exception, and
 * it is an interface rather than an implementation: it is what a new backing
 * store is written against.  Nothing in it describes how this module parses,
 * orders or canonicalises a configuration, which is the property that lets a
 * store be added without touching the core.
 *
 * ## Threading
 *
 * A handle may be read concurrently: @ref UfConfigGetField,
 * @ref UfConfigGetFieldByIndex, the namespace accessors and the serialisers are
 * safe to call from several threads at once.
 *
 * @ref UfConfigReload may run concurrently with readers.  While a reader is
 * inside a call, nothing it holds is freed underneath it.
 *
 * A value borrowed out of one call and used after that call returns is a
 * different matter, and the difference is a pin.  Hold one with
 * @ref UfConfigPin across the window in which the borrowed bytes are used, and
 * a reload retires the arena rather than releasing it, so the pointer stays
 * backed.  Without a pin the module has no way to know that a pointer escaped,
 * and a reload may release what backs it — which is a use-after-free in the
 * caller, a long way from the reload that caused it.  This was previously
 * written as though the guarantee were unconditional, and a reader that took it
 * that way is what the stress harness was aborting on.
 *
 * @ref UfConfigDestroy is not safe against any concurrent use, and neither are
 * the setters against each other.  The obligation is the ordinary one for a C
 * object handed out by pointer: quiesce the other threads first.
 *
 * ## Failure
 *
 * Every entry point returns a status.  Detail for the most recent failure is
 * retrieved with @ref UfConfigLastError, which is thread-local and overwritten
 * by the next operation on that thread.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UFLIB_UFCONFIG_UFCONFIG_H
#define UFLIB_UFCONFIG_UFCONFIG_H

#include <uflib/ufconfig/ufconfig_defs.h>
#include <uflib/ufconfig/ufconfig_type.h>

/* Field identifiers and the mutable-field setters, both generated from the
   schema at build time.  They are part of the public contract: a caller may
   index a field by its identifier, and the setters are how a mutable field is
   written.

   Quoted, not angle-bracketed: the generator emits them flat and the consumer
   places them where its own include path resolves them -- beside this header
   when they are installed, or in a generated directory the consumer adds to
   its include path.  The generator does not decide that, so this header cannot
   assume a path either.

   Guarded, because this header is also compiled by the library itself, and the
   library has no schema.  Without the guard, uflib could not build its own
   public header -- it would be requiring an artefact that by design belongs to
   the consumer.  A consumer that has generated its schema gets both; one that
   has not gets a header that compiles and a clear failure at the first use of
   an identifier it did not generate. */
#if defined(__has_include)
#  if __has_include("ufconfig_fields_generated.h")
#    include "ufconfig_fields_generated.h"
#    include "ufconfig_setters_stub_generated.h"
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * An open configuration handle.
 *
 * Opaque by design.  A caller holds a pointer to one and never its layout, so
 * the representation is free to change without recompiling a consumer.
 */
typedef struct UfConfig UfConfig;

/*!
 * @brief Creates a handle bound to the caller's schema.
 *
 * This is the entry point an application uses.  The library holds no schema of
 * its own — it is machinery, and a parser or validator with nothing to act on
 * is not a configuration library.  The schema is generated from the
 * application's own files by the generation pipeline, compiled into the
 * application, and handed across here.
 *
 * @param[out] out             Receives the handle.
 * @param[in]  descriptor_ptr  The injected schema.  Copied, not retained, so
 *                             the caller's struct need not outlive the call;
 *                             the arrays and function it points at must.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_INVALID_ARG if either argument is NULL.
 * @return UF_CONFIG_ERR_NOT_CONFIGURED if @c fields, @c field_count or
 *         @c lookup is missing from the descriptor.
 *
 * @note This is the only constructor a consumer sees, so every handle in an
 *       application's hands carries a schema.  There is no way to obtain one
 *       that cannot validate, and therefore no schema-less state for a caller
 *       to handle.
 *
 * @code{.c}
 * UfConfigDescriptor descriptor = {
 *     .fields      = g_ufconfig_fields,
 *     .field_count = (size_t)g_ufconfig_field_count,
 *     .lookup      = UfConfigLookupPath,
 *     .uf_logger   = app_logger,
 * };
 *
 * UfConfig *h = NULL;
 * UfConfigStatus st = UfConfigCreate(&h, &descriptor);
 * if (st != UF_CONFIG_OK) {
 *     fprintf(stderr, "config: %s\n", UfConfigStatusString(st));
 *     return EXIT_FAILURE;
 * }
 * UfConfigDestroy(h);
 * @endcode
 */
PUBLIC_API UfConfigStatus UfConfigCreate(UfConfig **out,
                                         const UfConfigDescriptor *descriptor_ptr);

/*!
 * @brief Releases a handle and everything it owns.
 *
 * @param[in] h  The handle.  NULL is accepted and does nothing.
 *
 * @note Any value previously borrowed from this handle — a string, a byte run,
 *       a reload report — is invalid afterwards.  A handle destroyed early does
 *       not disturb any other handle.
 */
PUBLIC_API void UfConfigDestroy(UfConfig *h);

/*!
 * @brief Loads a configuration from a file.
 *
 * @param[in]  h     An open handle.
 * @param[in]  path  Path to the document.
 * @param[in]  opt   Load options, or NULL for strict defaults.
 * @param[out] report  Optional; receives non-fatal observations.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_PARSE if the document is not well-formed.
 * @return UF_CONFIG_ERR_VALIDATION if a value failed its validator or format.
 * @return UF_CONFIG_ERR_REQUIRED if a required field is absent.
 * @return UF_CONFIG_ERR_UNKNOWN_FIELD if a field is undeclared and the mode is
 *         strict.
 * @return UF_CONFIG_ERR_BACKEND if the file could not be read.
 *
 * @note A failed load leaves the handle's previous configuration untouched, so
 *       a failed reload is not a way to lose a working configuration.
 */
PUBLIC_API UfConfigStatus UfConfigLoadFile(UfConfig *h, const char *path,
                                           const UfConfigLoadOptions *opt,
                                           UfConfigLoadReport *report);

/*!
 * @brief Loads a configuration from a memory buffer.
 *
 * @param[in]  h     An open handle.
 * @param[in]  buf   Document bytes.  Need not be NUL-terminated.
 * @param[in]  len   Number of bytes at @c buf.
 * @param[in]  opt   Load options, or NULL for strict defaults.
 * @param[out] report  Optional; receives non-fatal observations.
 *
 * @return As @ref UfConfigLoadFile.
 *
 * @note The buffer is borrowed for the duration of the call only.
 */
PUBLIC_API UfConfigStatus UfConfigLoadBuffer(UfConfig *h, const char *buf, size_t len,
                                             const UfConfigLoadOptions *opt,
                                             UfConfigLoadReport *report);

/*!
 * @brief Loads from the origin the handle was created with.
 *
 * This is the entry point for a configuration held anywhere other than a file
 * named at the call site.  It reads @ref UfConfigDescriptor.kind and the
 * endpoint that goes with it — Redis, SQL or memory — resolves the driver for
 * that kind, opens it, and installs what comes back.
 *
 * @param[in]  h       An open handle.
 * @param[in]  opt     Load options, or NULL for strict defaults.
 * @param[out] report  Optional; receives non-fatal observations.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOT_CONFIGURED if the descriptor names no origin, names
 *         one incompletely, or names a kind no driver serves.
 * @return UF_CONFIG_ERR_BACKEND if the store could not be reached or refused
 *         the read.
 * @return Otherwise as @ref UfConfigLoadFile — the content is validated against
 *         the same schema and refuses the same ways.
 *
 * @note For @c UF_CONFIG_BACKEND_FILE the origin is the path recorded by the
 *       last @ref UfConfigLoadFile, because a path is per-load rather than
 *       per-handle: one handle may be pointed at different documents.  A handle
 *       that has never loaded has no path and is refused.
 *
 * @note Ordering and structure are re-established here, not by the store.
 *       Nothing a driver returns is trusted to be ordered, complete, or in any
 *       particular shape — which is what lets a store be added without teaching
 *       it anything about this module.
 *
 * @code{.c}
 * UfConfigBackend redis = { .address = "10.0.0.5", .port = 6379,
 *                           .ns = "alice", .config_name = "ufsrvwebsock" };
 * UfConfigDescriptor d = { .fields = g_ufconfig_fields,
 *                          .field_count = (size_t)g_ufconfig_field_count,
 *                          .lookup = UfConfigLookupPath,
 *                          .kind = UF_CONFIG_BACKEND_REDIS, .redis = redis };
 *
 * UfConfig *h = NULL;
 * UfConfigCreate(&h, &d);
 * if (UfConfigLoad(h, &opt, NULL) != UF_CONFIG_OK) {
 *     const UfConfigError *e = UfConfigLastError();
 *     fprintf(stderr, "load: %s\n", e->message);
 * }
 * UfConfigDestroy(h);
 * @endcode
 */
PUBLIC_API UfConfigStatus UfConfigLoad(UfConfig *h, const UfConfigLoadOptions *opt,
                                       UfConfigLoadReport *report);

/*!
 * @brief Re-reads the source a handle was loaded from and reports the drift.
 *
 * @param[in]  h       An open handle that has a source.
 * @param[out] report  Receives the per-field outcome.  Owned by the handle.
 *
 * @return UF_CONFIG_OK if the reload succeeded, whether or not anything
 *         changed.
 * @return UF_CONFIG_NO_CHANGE if the source is byte-identical to the last load.
 * @return UF_CONFIG_ERR_BACKEND if the source could not be read.
 * @return UF_CONFIG_ERR_VALIDATION if the new content failed validation; the
 *         previous configuration remains in force.
 *
 * @note Values borrowed before this call stay valid for paths whose value did
 *       not change, so a reader holding a pointer is not invalidated by a
 *       reload that did not touch what it is reading.
 */
PUBLIC_API UfConfigStatus UfConfigReload(UfConfig *h, UfConfigReloadReport *report);

/*!
 * @brief Writes the current configuration back to its source.
 *
 * @param[in] h  An open handle that has a source.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOT_CONFIGURED if the handle has no source or no
 *         backend that can write.
 * @return UF_CONFIG_ERR_BACKEND if the write failed.
 */
PUBLIC_API UfConfigStatus UfConfigPersist(UfConfig *h);

/*!
 * @brief Reads one field by its dotted path.
 *
 * @param[in]  h           An open handle.
 * @param[in]  field_path  Full dotted path, e.g. @c "ufsrv.main_listener_port".
 * @param[out] out         Receives the value.  Borrowed; see @ref UfConfigValue.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if no such field is present.
 * @return UF_CONFIG_ERR_LIMIT_EXCEEDED if the path exceeds
 *         @ref CONFIG_DEFAULT_UFCONFIG_PATH_MAX.
 *
 * @note This is the general lookup and works for any path, declared or not.
 *       When the path is a schema field, prefer
 *       @ref UfConfigGetFieldByIndex, which resolves without a walk.
 */
PUBLIC_API UfConfigStatus UfConfigGetField(const UfConfig *h, const char *field_path,
                                           UfConfigValue *out);

/*!
 * @brief Reads one field by its generated identifier.
 *
 * @param[in]  h    An open handle.
 * @param[in]  id   A @c UF_CONFIG_FIELD_* constant, or the same value as a plain
 *                  @c int.  Plain deliberately: the generated enum belongs to the
 *                  consumer's schema and this header must compile without one,
 *                  so the contract cannot be expressed in terms of it.
 * @param[out] out  Receives the value.  Borrowed.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if the field is absent or @c id is out of range.
 *
 * @note Constant time.  Use this in a hot path where the path is known at
 *       compile time.
 */
PUBLIC_API UfConfigStatus UfConfigGetFieldByIndex(const UfConfig *h, int id,
                                                  UfConfigValue *out);

/*!
 * @brief Writes an integer field.
 *
 * @param[in] h    An open handle.
 * @param[in] path Field path.
 * @param[in] v    Value to store.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if the field is not in this instance.
 * @return UF_CONFIG_ERR_IMMUTABLE if the schema marks the field immutable.
 * @return UF_CONFIG_ERR_VALIDATION if the value fails its range or validator.
 */
PUBLIC_API UfConfigStatus UfConfigSetInt(UfConfig *h, const char *path, int64_t v);

/*!
 * @brief Writes a float field.  See @ref UfConfigSetInt for the returns.
 */
PUBLIC_API UfConfigStatus UfConfigSetFloat(UfConfig *h, const char *path, double v);

/*!
 * @brief Writes a string field.  See @ref UfConfigSetInt for the returns.
 *
 * @note @c v is copied.  A transform the schema declares is applied on write,
 *       so a later read returns the transformed value.
 */
PUBLIC_API UfConfigStatus UfConfigSetString(UfConfig *h, const char *path, const char *v);

/*!
 * @brief Writes a boolean field.  See @ref UfConfigSetInt for the returns.
 */
PUBLIC_API UfConfigStatus UfConfigSetBool(UfConfig *h, const char *path, bool v);

/*!
 * @brief Writes an opaque byte field.
 *
 * @param[in] h  An open handle.
 * @param[in] path Field path.
 * @param[in] p  Bytes to store.  May contain NUL.
 * @param[in] n  Number of bytes at @c p.
 */
PUBLIC_API UfConfigStatus UfConfigSetBytes(UfConfig *h, const char *path, const uint8_t *p, size_t n);

/*!
 * @brief Removes a field from the instance.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_REQUIRED if the schema marks the field required.
 * @return UF_CONFIG_ERR_NOFIELD if the field is not present.
 */
PUBLIC_API UfConfigStatus UfConfigUnset(UfConfig *h, const char *path);

/*!
 * @brief Empties an array field, leaving it present but with no elements.
 *
 * @return UF_CONFIG_ERR_TYPE_MISMATCH if the field is not an array.
 */
PUBLIC_API UfConfigStatus UfConfigArrayClear(UfConfig *h, const char *path);

/*!
 * @brief Appends one string element to an array field.
 *
 * @return UF_CONFIG_ERR_TYPE_MISMATCH if the field is not an array.
 * @return UF_CONFIG_ERR_LIMIT_EXCEEDED if the array is at its declared maximum.
 */
PUBLIC_API UfConfigStatus UfConfigArrayAppendString(UfConfig *h, const char *path, const char *v);

/*!
 * @brief Computes the handle's canonical digest.
 *
 * @param[in]  h    An open handle.
 * @param[out] out  Receives 32 bytes.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if the handle holds no configuration.
 *
 * @note The digest is taken over the canonical form, not the document text, so
 *       two documents that differ only in key order, whitespace or comments
 *       produce the same digest.  A document that expresses a table inline and
 *       one that reaches the same table through an alias likewise agree.
 */
PUBLIC_API UfConfigStatus UfConfigDigest(const UfConfig *h, unsigned char out[32]);

/*!
 * @brief Returns a counter bumped on every successful mutation.
 *
 * @return The current generation.  Monotonic within a handle's life.
 *
 * @note A cheap change test for a caller that does not need the digest.
 */
PUBLIC_API uint64_t UfConfigGeneration(const UfConfig *h);

/*!
 * @brief Returns detail for the most recent failure on this thread.
 *
 * @return A pointer to thread-local storage, valid until the next operation on
 *         the same thread.  Never NULL.
 */
PUBLIC_API const UfConfigError *UfConfigLastError(void);

/*!
 * @brief Returns a stable, human-readable name for a status.
 *
 * @param[in] s  Any status value.
 *
 * @return A static string.  Never NULL; an unknown value yields a placeholder
 *         rather than reading out of bounds.
 */
PUBLIC_API const char *UfConfigStatusString(UfConfigStatus s);

/*!
 * @brief Opens a namespace by path.
 *
 * @param[in]  h     An open handle.
 * @param[in]  path  Path to the table, or "" for the root.
 * @param[out] out   Receives a newly allocated namespace handle.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if the path does not name a table.
 *
 * @note **The caller owns the returned handle and must free() it**, and
 *       likewise any nested handle reached through
 *       @ref UfConfigNamespaceEntry.table.  The strings the handle points at —
 *       entry names, paths, values — are borrowed from the configuration and
 *       stay valid only while it does, so an open namespace must not outlive a
 *       reload or the handle itself.
 *
 * @code{.c}
 * UfConfigNamespace *ns = NULL;
 * if (UfConfigNamespaceGet(h, "ufsrv", &ns) == UF_CONFIG_OK) {
 *     size_t n = UfConfigNamespaceEntryCount(ns);
 *     for (size_t i = 0; i < n; i++) {
 *         UfConfigNamespaceEntry e;
 *         if (UfConfigNamespaceEntryAt(ns, i, &e) != UF_CONFIG_OK) continue;
 *         if (e.table) free(e.table);   // a nested table is also the caller's
 *     }
 *     free(ns);
 * }
 * @endcode
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceGet(const UfConfig *h, const char *path,
                                               UfConfigNamespace **out);

/*!
 * @brief Opens the root namespace.
 *
 * @return As @ref UfConfigNamespaceGet with an empty path.  The caller owns the
 *         returned handle and must free() it.
 */
PUBLIC_API UfConfigStatus UfConfigRootGet(const UfConfig *h, UfConfigNamespace **out);

/*!
 * @brief Returns the number of entries directly under a namespace.
 */
PUBLIC_API size_t UfConfigNamespaceEntryCount(const UfConfigNamespace *ns);

/*!
 * @brief Reads the entry at a position.
 *
 * @param[in]  ns   A namespace handle.
 * @param[in]  idx  Position, 0-based.
 * @param[out] out  Receives the entry.  Names and values are borrowed.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_NOFIELD if @c idx is out of range.
 *
 * @note Entry order is not defined.  Use @ref UfConfigNamespaceEntryByName to
 *       reach a named entry.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceEntryAt(const UfConfigNamespace *ns, size_t idx,
                                                   UfConfigNamespaceEntry *out);

/*!
 * @brief Reads an entry by name.
 *
 * @return UF_CONFIG_ERR_NOFIELD if the namespace has no such entry.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceEntryByName(const UfConfigNamespace *ns,
                                                       const char *name,
                                                       UfConfigNamespaceEntry *out);

/*!
 * @brief Resolves an entry that is an alias to the table it names.
 *
 * @param[in]  ns   A namespace handle.
 * @param[out] out  Receives the resolved namespace.  The caller owns it and
 *                  must free() it.
 *
 * @return UF_CONFIG_OK on success, including when @p ns is already a plain
 *         table — in that case the result describes the same table, so a caller
 *         need not test the entry's kind before resolving it.
 * @return UF_CONFIG_ERR_ALIAS_UNRESOLVED if the alias names a table that does
 *         not exist.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceResolve(const UfConfigNamespace *ns,
                                                   UfConfigNamespace **out);

/*!
 * @brief Returns the defining path of a namespace.
 */
PUBLIC_API const char *UfConfigNamespacePath(const UfConfigNamespace *ns);

/*!
 * @brief Returns the path this namespace was reached by, which may be an alias.
 *
 * @note Differs from @ref UfConfigNamespacePath when the namespace was reached
 *       through an alias: the defining path is where the table is written, this
 *       is where the caller asked for it.
 */
PUBLIC_API const char *UfConfigNamespacePathVia(const UfConfigNamespace *ns);

/*!
 * @brief Serialises a namespace as JSON.
 *
 * @param[in]  ns   A namespace handle.
 * @param[out] buf  Destination, or NULL to size the result.
 * @param[in]  cap  Capacity of @c buf.
 * @param[out] needed  Receives the bytes required, including the terminator.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_LIMIT_EXCEEDED if @c cap is too small; @c needed then
 *         reports what is required, so a caller can size and retry.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceToJson(const UfConfigNamespace *ns,
                                                  char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises a namespace as YAML.  See @ref UfConfigNamespaceToJson.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceToYaml(const UfConfigNamespace *ns,
                                                  char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises a namespace as INI.  See @ref UfConfigNamespaceToJson.
 */
PUBLIC_API UfConfigStatus UfConfigNamespaceToIni(const UfConfigNamespace *ns,
                                                 char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises the whole configuration as JSON.
 *
 * @param[in]  h    An open handle.
 * @param[out] buf  Destination, or NULL to size the result.
 * @param[in]  cap  Capacity of @c buf.
 * @param[out] needed  Receives the bytes required, including the terminator.
 */
PUBLIC_API UfConfigStatus UfConfigToJson(const UfConfig *h, char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises the whole configuration as JSON into a fresh allocation.
 *
 * @param[out] out  Receives a NUL-terminated string the caller must free().
 */
PUBLIC_API UfConfigStatus UfConfigToJsonAlloc(const UfConfig *h, char **out);

/*!
 * @brief Serialises the whole configuration as YAML.  See @ref UfConfigToJson.
 */
PUBLIC_API UfConfigStatus UfConfigToYaml(const UfConfig *h, char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises the whole configuration as YAML into a fresh allocation.
 */
PUBLIC_API UfConfigStatus UfConfigToYamlAlloc(const UfConfig *h, char **out);

/*!
 * @brief Serialises the whole configuration as INI.  See @ref UfConfigToJson.
 */
PUBLIC_API UfConfigStatus UfConfigToIni(const UfConfig *h, char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises the whole configuration as INI into a fresh allocation.
 */
PUBLIC_API UfConfigStatus UfConfigToIniAlloc(const UfConfig *h, char **out);

/*!
 * @brief Serialises the whole configuration in the Lua-styled document syntax.
 *
 * @note The output is a document in the syntax this module parses, so it can be
 *       read back.  It is not a Lua program and is not evaluated as one — the
 *       format mimics Lua's table syntax and carries no expressions.
 */
PUBLIC_API UfConfigStatus UfConfigToLuaStyle(const UfConfig *h, char *buf, size_t cap, size_t *needed);

/*!
 * @brief Serialises in the Lua-styled syntax into a fresh allocation.
 */
PUBLIC_API UfConfigStatus UfConfigToLuaStyleAlloc(const UfConfig *h, char **out);

/*!
 * @brief Installs a write-through backend for the handle's source.
 *
 * @param[in] h          An open handle.
 * @param[in] read_all   Reads the whole source, or NULL to leave unchanged.
 * @param[in] write_all  Writes the whole source, or NULL to leave unchanged.
 * @param[in] ctx        Passed to both.
 *
 * @note This is the older, replace-the-whole-document seam.  It predates the
 *       driver interface and is retained while callers migrate; new backends
 *       should implement @ref UfConfigDriver instead.  Unlike a driver, it
 *       cannot express per-field access or a change token, and it carries the
 *       document as text, so anything reaching the store this way is parsed
 *       again on the way back in.
 */
PUBLIC_API void UfConfigSetBackend(UfConfig *h,
    UfConfigStatus (*read_all)(void *ctx, const char *ident, char **buf, size_t *len),
    UfConfigStatus (*write_all)(void *ctx, const char *ident, const char *buf, size_t len),
    void *ctx);

/*!
 * @brief Pins the handle against reload retirement.
 *
 * @return The pin count after incrementing.
 *
 * @note While any pin is held, a reload retires the previous arena rather than
 *       releasing it, so pointers a reader still holds cannot be freed
 *       underneath it.  Pair every pin with an unpin.
 */
PUBLIC_API uint64_t UfConfigPin(UfConfig *h);

/*!
 * @brief Releases a pin taken with @ref UfConfigPin.
 */
PUBLIC_API void UfConfigUnpin(UfConfig *h);

/*!
 * @brief Re-reads one field through the backend.
 *
 * @return UF_CONFIG_ERR_NOT_CONFIGURED if no backend supports single-field
 *         reads.
 */
PUBLIC_API UfConfigStatus UfConfigRefreshField(UfConfig *h, const char *path,
                                               UfConfigValue *out);

/*!
 * @brief Releases a pair array returned by @ref UfConfigFlattenHandle or
 *        @ref UfConfigPairsFromLua.
 */
PUBLIC_API void UfConfigFreePairs(UfConfigFieldPair *p, size_t n);

/*!
 * @brief Flattens a handle to the pair-set that crosses the driver boundary.
 *
 * @param[in]  h    An open handle.
 * @param[out] out  Receives an allocated pair array; free with
 *                  @ref UfConfigFreePairs.
 * @param[out] np   Receives the pair count.
 * @param[out] meta Optional; receives store metadata for the content.
 *
 * @note Pairs are unordered.  This is the bulk-read direction of the driver
 *       contract, exposed so a backend can be exercised without a store.
 */
PUBLIC_API UfConfigStatus UfConfigFlattenHandle(const struct UfConfig *h,
                                                UfConfigFieldPair **out, size_t *np,
                                                UfConfigMeta *meta);

/*!
 * @brief Parses a document straight to a pair-set, without a handle.
 *
 * @param[in]  buf  Document bytes.
 * @param[in]  n    Number of bytes at @c buf.
 * @param[out] out  Receives an allocated pair array; free with
 *                  @ref UfConfigFreePairs.
 * @param[out] np   Receives the pair count.
 */
PUBLIC_API UfConfigStatus UfConfigPairsFromLua(const char *buf, size_t n,
                                               UfConfigFieldPair **out, size_t *np);

/*!
 * @brief Builds a handle's configuration from a pair-set.
 *
 * @param[in] h     An open handle.
 * @param[in] pairs Pairs to install.  Order is not significant.
 * @param[in] n     Number of pairs.
 * @param[in] meta  Optional store metadata, or NULL.
 * @param[in] opt   Load options, or NULL for strict defaults.
 *
 * @note This is the bulk-write direction of the driver contract.  Ordering and
 *       structure are re-established here, so a driver may return pairs in any
 *       order it likes and is never asked to preserve one.
 */
PUBLIC_API UfConfigStatus UfConfigLoadPairs(struct UfConfig *h,
                                            const UfConfigFieldPair *pairs, size_t n,
                                            const UfConfigMeta *meta,
                                            const UfConfigLoadOptions *opt,
                                            UfConfigLoadReport *report);

/*!
 * @brief Registers a driver so it can be found by kind or by name.
 *
 * @param[in] drv  A driver whose @c caps.api_version matches
 *                 @ref UF_CONFIG_DRIVER_API_VERSION.
 *
 * @return UF_CONFIG_OK on success.
 * @return UF_CONFIG_ERR_INVALID_ARG if @c drv or its name is NULL.
 * @return UF_CONFIG_ERR_UNIMPLEMENTED if the driver's API version is not the
 *         one this build implements.  (Not @c UF_CONFIG_ERR_STORE_VERSION: that
 *         status describes a store whose *content* format is unreadable, which
 *         is a different failure from a driver compiled against another ABI.)
 * @return UF_CONFIG_ERR_LIMIT_EXCEEDED if the registry is full.
 *
 * @note The driver struct must outlive every handle that uses it.  Registration
 *       is not required for a driver the module already builds in.
 */
PUBLIC_API UfConfigStatus UfConfigRegisterDriver(const UfConfigDriver *drv);

/*!
 * @brief Finds a registered driver by the kind it serves.
 *
 * @return The driver, or NULL if none is registered for that kind.
 */
PUBLIC_API const UfConfigDriver *UfConfigDriverFindKind(UfConfigBackendKind kind);

/*!
 * @brief Finds a registered driver by its declared name.
 *
 * @return The driver, or NULL if no driver carries that name.
 */
PUBLIC_API const UfConfigDriver *UfConfigDriverFindName(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_UFCONFIG_UFCONFIG_H */
