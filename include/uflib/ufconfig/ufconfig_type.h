/**
 * @file ufconfig_type.h
 * @brief The ufconfig module's public types: an opaque handle, the values a
 *        caller reads back, and the driver interface a backend implements.
 *
 * This header is type-only.  It carries no function declarations — those are in
 * @ref ufconfig.h — and no implementation state, which is in the private
 * @c src/ufconfig/ufconfig_type_priv.h.
 *
 * ## What a caller is given, and what it is not
 *
 * A caller is given a handle it cannot look inside, a value union describing
 * what a field holds, and a report describing what a load or a reload did.  It
 * is not given the node representation, the arena, the index, the parsed tree,
 * or the schema descriptor's layout.  Those exist, but they are the module's
 * business.
 *
 * The test applied to every field below is: *is this the caller's, or the
 * implementation's?*  A field's value and type are the caller's.  The recorded
 * digest of a subtree is the caller's, because drift detection is built on it.
 * The arena a node was allocated from is not.
 *
 * ## What a backend is given
 *
 * The driver types are the one part of this header a caller does not use and a
 * backend implementor does.  They carry the whole of the storage contract: a
 * pair-set crosses the boundary and nothing else does.  Ordering, structural
 * inference and canonicalisation stay on this side of it, so a driver is never
 * asked to reproduce them — which is what makes a new backing store a matter of
 * implementing seven operations rather than of understanding this module.
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

#ifndef UFLIB_UFCONFIG_UFCONFIG_TYPE_H
#define UFLIB_UFCONFIG_UFCONFIG_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <uflib/ufconfig/ufconfig_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Outcome of an operation.
 *
 * Every entry point returns one of these.  A status is not a diagnostic: the
 * detail — which field, which validator, which line — is retrieved afterwards
 * through @ref UfConfigLastError, so that a caller that only wants to branch on
 * success is not made to carry a struct around.
 */
typedef enum UfConfigStatus {
    UF_CONFIG_OK = 0,                        ///< completed successfully
    UF_CONFIG_NO_CHANGE,                     ///< completed, nothing differed
    UF_CONFIG_ERR_NOFIELD,                   ///< no such field in this instance
    UF_CONFIG_ERR_TYPE_MISMATCH,             ///< value does not fit the field's type
    UF_CONFIG_ERR_REQUIRED,                  ///< a required field is absent
    UF_CONFIG_ERR_VALIDATION,                ///< a validator or format rejected the value
    UF_CONFIG_ERR_IMMUTABLE,                 ///< field is not mutable at runtime
    UF_CONFIG_ERR_UNKNOWN_FIELD,             ///< present in the document, absent from the schema
    UF_CONFIG_ERR_PARSE,                     ///< the document is not well-formed
    UF_CONFIG_ERR_BACKEND,                   ///< the backing store failed
    UF_CONFIG_ERR_LIMIT_EXCEEDED,            ///< a compile-time ceiling was hit
    UF_CONFIG_ERR_SUBST_UNRESOLVED,          ///< a substitution named nothing
    UF_CONFIG_ERR_DIGEST_MISMATCH,           ///< stored digest does not match the content
    UF_CONFIG_ERR_NO_MEMORY,                 ///< allocation failed
    UF_CONFIG_ERR_UNIMPLEMENTED,             ///< declared but not built in this configuration
    UF_CONFIG_ERR_INVALID_ARG,               ///< a NULL or out-of-range argument
    UF_CONFIG_ERR_CYCLE,                     ///< alias chain is circular
    UF_CONFIG_ERR_ALIAS_UNRESOLVED,          ///< an alias names a binding that does not exist
    UF_CONFIG_ERR_NOT_A_TABLE,               ///< a table was required where a scalar stands
    UF_CONFIG_ERR_ALIAS_DEPTH_EXCEEDED,      ///< alias chain exceeded its ceiling
    UF_CONFIG_ERR_NOT_CONFIGURED,            ///< no backend or schema was supplied
    UF_CONFIG_ERR_STORE_VERSION,             ///< the store's format version is not understood
    UF_CONFIG_ERR_NOT_ATOMIC,               ///< the driver cannot do this atomically
    /*!
     * A secret could not be turned into a value.
     *
     * Distinct from @c VALIDATION because the causes are not the document's
     * content: a key that is absent from the secrets file, a secrets file that
     * cannot be read, an envelope whose tag does not verify, or a value standing
     * on an @c encrypted field that is not an envelope at all.  A caller
     * diagnosing this is looking at key provisioning rather than at a mistyped
     * value, and the two want different responses.
     */
    UF_CONFIG_ERR_SECRET
} UfConfigStatus;

/*!
 * The type a field's value holds.
 *
 * A kind describes the value as the caller receives it — after any transform
 * the schema declares.  The pre-transform value is carried alongside in
 * @ref UfConfigValue.raw when a caller needs to see what the document said.
 */
typedef enum UfConfigValueKind {
    UF_CONFIG_KIND_INT = 0,   ///< signed 64-bit integer
    UF_CONFIG_KIND_FLOAT,     ///< double
    UF_CONFIG_KIND_BOOL,      ///< boolean
    UF_CONFIG_KIND_STRING,    ///< UTF-8 text, not necessarily terminated
    UF_CONFIG_KIND_BYTES,     ///< opaque bytes, possibly containing NUL
    UF_CONFIG_KIND_ARRAY,     ///< ordered sequence, see @c as.array
    UF_CONFIG_KIND_SCOPE,     ///< a table, read through the namespace API
    UF_CONFIG_KIND_ABSENT     ///< no such field; @c present is false
} UfConfigValueKind;

struct UfConfigFieldDesc;

/*!
 * A field's value, as read back from the handle.
 *
 * Strings and byte runs in @c as and @c raw are **borrowed**, not owned: they
 * stay valid for as long as the handle does, and stay valid across a reload if
 * the path's value did not change.  A caller that needs them to outlive the
 * handle, or to survive an unrelated reload, must copy them.
 */
typedef struct UfConfigValue {
    UfConfigValueKind kind;      ///< which member of @c as is meaningful
    bool              present;   ///< false when the field is absent
    union {
        int64_t       i;         ///< KIND_INT
        double        f;         ///< KIND_FLOAT
        bool          b;         ///< KIND_BOOL
        struct { const char *ptr; size_t len; } str;      ///< KIND_STRING
        struct { const uint8_t *ptr; size_t len; } bytes; ///< KIND_BYTES
        struct { const struct UfConfigValue *elems; size_t count; } array; ///< KIND_ARRAY
    } as;
    const struct UfConfigFieldDesc *desc;  ///< schema descriptor; NULL if undeclared
    /*!
     * The instance-layer value before any transform was applied.
     *
     * Borrowed on the same terms as @c as.  Present so that a caller can tell
     * what the document literally said from what the schema made of it — a
     * distinction that matters when a transform is lossy.
     */
    union {
        int64_t i;               ///< pre-transform integer
        double  f;               ///< pre-transform float
        bool    b;               ///< pre-transform boolean
        struct { const char *ptr; size_t len; } str;  ///< pre-transform text
    } raw;
} UfConfigValue;

/*!
 * What happened to one field across a reload.
 */
typedef enum UfConfigReloadStatus {
    UF_CONFIG_RELOAD_UNCHANGED = 0,     ///< same value and digest
    UF_CONFIG_RELOAD_CHANGED,           ///< value differs
    UF_CONFIG_RELOAD_ADDED,             ///< absent before, present now
    UF_CONFIG_RELOAD_REMOVED,           ///< present before, absent now
    UF_CONFIG_RELOAD_REJECTED,          ///< present, but failed validation
    UF_CONFIG_RELOAD_CHANGED_VIA_ALIAS, ///< differs, and the path is an alias
    UF_CONFIG_RELOAD_UNREFERENCED_BINDING, ///< a binding nothing reaches
    UF_CONFIG_RELOAD_CYCLE              ///< an alias chain became circular
} UfConfigReloadStatus;

/*!
 * One field's reload outcome.
 *
 * @c path is the defining path.  When the change arrived through an alias,
 * @c via_path names the path the caller actually reads, so that a caller
 * watching a field is not left comparing digests that moved for a reason it
 * cannot see.
 */
typedef struct UfConfigReloadEntry {
    const char           *path;        ///< defining path of the field
    UfConfigReloadStatus  status;      ///< what happened to it
    unsigned char         old_digest[32]; ///< digest before, zero when added
    unsigned char         new_digest[32]; ///< digest after, zero when removed
    const char           *diagnostic;   ///< why, when rejected; may be NULL
    const char           *via_path;     ///< alias path, or NULL
} UfConfigReloadEntry;

/*!
 * The result of a reload.
 *
 * @c entries is owned by the handle and stays valid until the next reload or
 * until the handle is destroyed.  A caller must not free it.
 */
typedef struct UfConfigReloadReport {
    size_t                count;    ///< number of entries
    UfConfigReloadEntry  *entries;  ///< owned by the handle
} UfConfigReloadReport;

/*!
 * What a load noted without failing.
 */
typedef enum UfConfigLoadNoteKind {
    UF_CONFIG_NOTE_UNKNOWN = 0,          ///< unrecognised
    UF_CONFIG_NOTE_UNREFERENCED_BINDING  ///< a top-level binding nothing reaches
} UfConfigLoadNoteKind;

/*!
 * One non-fatal observation made during a load.
 *
 * A document may define a table and never reference it.  That is legal — the
 * root may be an explicit @c return block that does not mention it — but it is
 * usually a mistake, so it is reported rather than silently dropped.
 */
typedef struct UfConfigLoadNote {
    UfConfigLoadNoteKind  kind;    ///< what was noted
    const char           *path;    ///< where
    int                   line;    ///< 1-based line in the document
    int                   column;  ///< 1-based column
} UfConfigLoadNote;

/*!
 * Non-fatal observations from a load.
 *
 * Owned by the handle on the same terms as @ref UfConfigReloadReport.
 */
typedef struct UfConfigLoadReport {
    size_t            note_count;  ///< number of notes
    UfConfigLoadNote *notes;       ///< owned by the handle
} UfConfigLoadReport;

/*!
 * Detail for the most recent failure, on the calling thread.
 *
 * The struct is thread-local and is overwritten by the next operation on the
 * same thread, so a caller that wants to keep it must copy it.
 */
typedef struct UfConfigError {
    UfConfigStatus  status;      ///< mirrors the returned status
    const char     *field_path;  ///< offending field, or NULL
    const char     *validator;   ///< offending validator or transform, or NULL
    int             line;        ///< 1-based line, 0 when not applicable
    int             column;      ///< 1-based column, 0 when not applicable
    char            message[512];///< human-readable detail
} UfConfigError;

/*!
 * What a namespace entry holds.
 *
 * A table reached through an alias is distinguished from one written inline
 * because the two are not interchangeable to a caller walking a tree: the alias
 * form may be shared with another path, so mutating through it is not the
 * local edit it appears to be.
 */
typedef enum UfConfigNsEntryKind {
    UF_CONFIG_NS_VALUE = 0,      ///< a scalar; see @c value
    UF_CONFIG_NS_TABLE_INLINE,   ///< a table written here; see @c table
    UF_CONFIG_NS_TABLE_ALIAS,    ///< a table defined elsewhere; see @c alias_target
    UF_CONFIG_NS_ARRAY           ///< an array
} UfConfigNsEntryKind;

typedef struct UfConfigNamespace UfConfigNamespace;

/*!
 * One entry of a namespace.
 *
 * @c name is the entry's own name, not a full path.  Which of @c value,
 * @c table and @c alias_target is meaningful is decided by @c kind.
 */
typedef struct UfConfigNamespaceEntry {
    const char          *name;         ///< entry name, relative to its namespace
    UfConfigNsEntryKind  kind;         ///< which of the members below applies
    UfConfigValue        value;        ///< when kind is VALUE or ARRAY
    UfConfigNamespace   *table;        ///< when kind is TABLE_INLINE
    const char          *alias_target; ///< when kind is TABLE_ALIAS
} UfConfigNamespaceEntry;

/*!
 * How strictly a document is held to the schema.
 *
 * Strict is the default, and the default is the safe direction: a document
 * naming a field the schema does not declare is almost always a typo or a
 * version skew, and accepting it silently is how a configuration appears to
 * take effect while doing nothing.
 */
typedef enum UfConfigLoadMode {
    UF_CONFIG_LOAD_STRICT = 0,  ///< an undeclared field is an error
    UF_CONFIG_LOAD_LENIENT      ///< an undeclared field is carried and reported
} UfConfigLoadMode;

/*!
 * A load's options.
 *
 * @c size must be set to @c sizeof(UfConfigLoadOptions) so that fields added in
 * a later version can be told apart from fields a caller left zero.
 */
typedef struct UfConfigLoadOptions {
    size_t             size;               ///< sizeof(UfConfigLoadOptions)
    int                version;            ///< options version; use 1
    UfConfigLoadMode   mode;               ///< strict or lenient
    const char *const *schema_files;       ///< additional schema files, or NULL
    size_t             schema_file_count;  ///< number of entries in schema_files
    /*!
     * Resolves a substitution name to its value, or NULL when unknown.
     *
     * Called with @c subst_ctx.  Supplying it enables the @c varsub transform;
     * leaving it NULL makes a document using one fail with
     * @c UF_CONFIG_ERR_SUBST_UNRESOLVED rather than substituting an empty
     * string, because a silently blank value is harder to notice than a failure.
     */
    const char *(*subst_get)(void *ctx, const char *name);
    void              *subst_ctx;          ///< passed to subst_get
    bool               subst_recursive;    ///< whether substitutions recurse
} UfConfigLoadOptions;

/*!
 * Options for serialising a handle back out.
 */
typedef struct UfConfigSerialiseOptions {
    size_t size;        ///< sizeof(UfConfigSerialiseOptions)
    int    version;     ///< options version; use 1
    bool   pretty;      ///< indent and break lines
    bool   expand;      ///< expand aliases to their contents
    bool   emit_defaults; ///< include fields that only carry a default
} UfConfigSerialiseOptions;

/*!
 * Which backing store a driver serves.
 *
 * The kind is an identity, not a capability: two drivers of the same kind may
 * declare different capabilities, and a caller choosing between them should
 * read @ref UfConfigDriverCaps rather than assume from the kind.
 */
typedef enum UfConfigBackendKind {
    UF_CONFIG_BACKEND_FILE = 1,  ///< a file on the local filesystem
    UF_CONFIG_BACKEND_MEM,       ///< process memory
    UF_CONFIG_BACKEND_REDIS,     ///< a Redis hash
    UF_CONFIG_BACKEND_SQL        ///< a relational table
} UfConfigBackendKind;

/*!
 * The type of one value in a pair.
 *
 * This is the type that crosses the driver boundary, which is why it is a
 * separate enumeration from @ref UfConfigValueKind: a store has to distinguish
 * an empty table from an absent one, and has no use for the caller-facing
 * distinction between a table and a scope.
 */
typedef enum UfConfigPairType {
    UF_PAIR_INT = 0,     ///< integer, encoded as decimal text
    UF_PAIR_FLOAT,       ///< float, encoded as text
    UF_PAIR_BOOL,        ///< boolean, encoded as "0"/"1"
    UF_PAIR_STRING,      ///< string
    UF_PAIR_EMPTY_TABLE  ///< a table with no children
} UfConfigPairType;

/*!
 * One field, in the form that crosses the driver boundary.
 *
 * @c encoded is always the canonical-form scalar text, never a structure.  A
 * driver stores it verbatim and never interprets it, which is what keeps
 * canonicalisation on this side of the boundary.
 */
typedef struct UfConfigFieldPair {
    const char      *path;     ///< full dotted path
    UfConfigPairType vtype;    ///< type of the encoded value
    const char      *encoded;  ///< canonical-form scalar text
} UfConfigFieldPair;

/*!
 * Store-level metadata that travels with a pair-set.
 *
 * A driver fills in what its store can express and leaves the rest zero,
 * setting @c is_digest_set only when it actually has a digest to report.
 */
typedef struct UfConfigMeta {
    unsigned fmt;             ///< store format version
    uint64_t version;         ///< store's own monotonic version
    char     digest_hex[65];  ///< canonical digest, hex, NUL-terminated
    uint64_t schema_hash;     ///< hash of the schema the content was checked against
    int64_t  updated_at;      ///< seconds since the epoch, or 0
    bool     is_digest_set;   ///< whether digest_hex carries a value
} UfConfigMeta;

/*!
 * A cheap change token.
 *
 * A driver returns one from its @c stat operation so a caller can tell whether
 * anything changed without reading the content back.  The value is opaque and
 * only ever compared for equality with another token from the same store.
 */
typedef struct UfConfigStamp {
    uint64_t token;  ///< opaque; compare, never interpret
} UfConfigStamp;

/*!
 * Where a driver should connect, and under what identity.
 *
 * The spec is a union because the addressing a store needs differs completely
 * between a file and a server.  @c kind selects the member.
 */
typedef struct UfConfigBackendSpec {
    UfConfigBackendKind kind;  ///< selects the member of @c u
    union {
        struct {
            const char *path;      ///< file path
        } file;
        struct {
            const char *host;        ///< server host
            unsigned    port;        ///< server port
            unsigned    db;          ///< database index
            const char *username;    ///< ACL user, or NULL
            const char *password;    ///< ACL password, or NULL
            const char *key_prefix;  ///< prefix applied to every key
            const char *ns;          ///< namespace, typically the user
            const char *config_name; ///< which configuration within the namespace
            bool        use_tls;     ///< negotiate TLS
            unsigned    timeout_ms;  ///< connect and command timeout
        } redis;
        struct {
            const char *dsn;          ///< connection string
            const char *table_prefix; ///< prefix applied to every table
            const char *ns;           ///< namespace, typically the user
            const char *config_name;  ///< which configuration within the namespace
        } sql;
        struct {
            const char *name;  ///< identifies a registered in-memory instance
        } mem;
    } u;
} UfConfigBackendSpec;

/*!
 * What a driver can do, declared rather than discovered.
 *
 * A driver states its capabilities up front so that the core can refuse an
 * operation the store cannot perform atomically, instead of attempting it and
 * leaving a half-written configuration behind.
 *
 * The declaration is also what makes a partial backend usable: a store offering
 * only bulk read and write is a legitimate driver and is used as one, rather
 * than being forced to emulate the operations it lacks.
 */
typedef struct UfConfigDriverCaps {
    uint32_t    api_version;        ///< must equal UF_CONFIG_DRIVER_API_VERSION
    /*!
     * Which origin this driver serves.
     *
     * A property of the driver rather than a name the core matches against, so
     * that a consumer can supply a driver for an origin the library already
     * has a stand-in for.  Registering one for a kind already served replaces
     * that driver — the later registration wins, which is what lets an
     * application substitute a real store for a built-in placeholder without
     * the library having to be rebuilt.
     */
    UfConfigBackendKind kind;
    bool        atomic_write_all;   ///< write_all either lands wholly or not at all
    bool        atomic_write_one;   ///< write_one is atomic
    bool        per_field_read;     ///< read_one is supported
    bool        native_version;     ///< the store maintains a change token
    bool        enumeration;        ///< the store can list its configurations
    bool        multi_config_txn;   ///< several configurations can be written together
    bool        write_one_creates;  ///< write_one creates a missing field
    uint32_t    max_path_bytes;     ///< longest path the store accepts, 0 if unbounded
    uint32_t    max_value_bytes;    ///< largest value the store accepts, 0 if unbounded
    uint32_t    max_fields;         ///< most fields the store accepts, 0 if unbounded
    const char *name;               ///< stable name, for lookup and diagnostics
} UfConfigDriverCaps;

/*!
 * The seven operations a backing store implements.
 *
 * This is the whole of the storage contract.  A driver receives a
 * @ref UfConfigBackendSpec, hands back and accepts unordered pair-sets, and
 * never sees a tree, an ordering or a canonical form.  Everything the module
 * guarantees about ordering and structure is established before a pair reaches
 * a driver and re-established after one comes back, so a new backing store
 * needs to satisfy the store's own semantics and nothing of this module's.
 *
 * A driver that cannot perform an operation says so through its capabilities
 * rather than failing at call time.  @c close is called at most once, and only
 * after a successful @c open.
 */
typedef struct UfConfigDriver {
    UfConfigDriverCaps caps;  ///< declared before any operation is attempted

    /*!
     * Opens the store described by @c spec and stores its handle in @c *ctx.
     */
    UfConfigStatus (*open)(void **ctx, const UfConfigBackendSpec *spec);

    /*!
     * Reads every pair in the configuration.
     *
     * The order pairs are returned in is not significant and is not required to
     * be stable; the core imposes an order of its own.
     */
    UfConfigStatus (*read_all)(void *ctx, UfConfigFieldPair **out, size_t *n, UfConfigMeta *meta);

    /*!
     * Replaces the configuration with @c in, atomically when the driver
     * declares @c atomic_write_all.
     */
    UfConfigStatus (*write_all)(void *ctx, const UfConfigFieldPair *in, size_t n, const UfConfigMeta *meta);

    /*!
     * Reads a single pair.  Only called when @c per_field_read is declared.
     */
    UfConfigStatus (*read_one)(void *ctx, const char *path, UfConfigFieldPair *out);

    /*!
     * Writes a single pair and bumps the store's version.
     */
    UfConfigStatus (*write_one)(void *ctx, const char *path, const UfConfigFieldPair *in);

    /*!
     * Reports a change token.  Only called when @c native_version is declared.
     */
    UfConfigStatus (*stat)(void *ctx, UfConfigStamp *out);

    /*!
     * Releases the handle from @c open.
     */
    void (*close)(void *ctx);
} UfConfigDriver;

/*! Opaque.  Forward-declared rather than included: this header is reached by
    every consumer of the module, and pulling the logger's whole interface in
    for the sake of one pointer would make ufconfig's public contract depend on
    the logger's. */
struct UfLogger;

/*!
 * The type a schema field declares.
 */
typedef enum UfFieldType {
    UF_FT_INT,    ///< integer
    UF_FT_FLOAT,  ///< float
    UF_FT_BOOL,   ///< boolean
    UF_FT_STR,    ///< string
    UF_FT_ARR,    ///< array
    UF_FT_TAB     ///< table
} UfFieldType;

/*!
 * One field's schema declaration.
 *
 * This is the consumer's type, not the library's.  The schema describes the
 * consumer's configuration — which fields exist, what they default to, what
 * range or format they must satisfy — and that is business logic belonging to
 * the application, not to a general-purpose config loader.  The library
 * supplies the machinery that acts on a schema; the schema itself is injected.
 *
 * @c id is a plain @c int rather than the generated @c UfConfigFieldId.  The
 * descriptor must be declarable by a consumer that has no generated header in
 * scope — the generator emits the table positionally, so the two are compatible
 * either way, and keeping the enum out of this struct is what lets this header
 * stand alone.
 *
 * The @c is_* / @c has_* flags exist because zero is a meaningful value for
 * every bound below, so presence has to be recorded separately from the value.
 */
typedef struct UfConfigFieldDesc {
    int             id;            ///< index of this field; generator-assigned
    const char     *path;          ///< full dotted path, e.g. "ufsrv.server_id"
    UfFieldType     type;          ///< declared type
    int             is_required;   ///< the document must state it
    int             is_mutable;    ///< may be written at runtime
    int             has_default;   ///< a default is declared
    long long       d_int;         ///< default, when the type is integer
    double          d_float;       ///< default, when the type is float
    int             d_bool;        ///< default, when the type is boolean
    const char     *d_str;         ///< default, when the type is text
    int             has_min;       ///< a lower bound is declared
    int             has_max;       ///< an upper bound is declared
    long long       vmin;          ///< lower bound
    long long       vmax;          ///< upper bound
    const char     *transforms;    ///< comma-separated transform names, or NULL
    const char     *one_of;        ///< comma-separated permitted values, or NULL
    const char     *fmt;           ///< format validator name, or NULL
    int             has_len_min;   ///< a minimum length is declared
    int             has_len_max;   ///< a maximum length is declared
    long long       len_min;       ///< minimum length
    long long       len_max;       ///< maximum length
    int             has_array_max; ///< an array ceiling is declared
    long long       array_max;     ///< array ceiling
    const char     *doc;           ///< documentation string, or NULL
    const char     *regex;         ///< regular-expression validator, or NULL
    /*!
     * Whether the document's value is an envelope the module must decrypt.
     *
     * Declared in the schema as @c encrypted = true.  When set, the document
     * holds `enc:v1:<nonce>:<ciphertext>:<tag>` and a caller reading the field
     * receives the plaintext instead — the envelope stays in @c raw, in every
     * serialised form, and in anything written back to a store.
     *
     * Appended rather than placed beside @c is_mutable so that the positional
     * initialisers the generator emits keep their existing order.
     */
    int             is_encrypted;
} UfConfigFieldDesc;

/*!
 * Where a store lives, and who to connect as.
 *
 * Deliberately one shape for every networked store.  Redis and SQL differ in
 * how they are addressed, but a caller filling this in is answering the same
 * four questions either way, and a single struct means the descriptor does not
 * need a union whose layout depends on which member is live.
 *
 * Every field is optional except @c address.  A store that needs no
 * credentials is given NULL rather than empty strings, so "not supplied" is
 * distinguishable from "supplied as empty".
 */
typedef struct UfConfigBackend {
    const char *address;    ///< host name or address; required
    int         port;       ///< 0 to let the store's own default apply
    const char *username;   ///< NULL when the store needs none
    const char *password;   ///< NULL when the store needs none
    /*!
     * The consumer's namespace within the store.
     *
     * What separates one tenant's configuration from another's.  For a
     * per-user configuration this is the user; leaving it NULL means the
     * store's default namespace, not "everyone".
     */
    /*! Named @c ns rather than @c namespace: the latter is a keyword in C++,
        and a public header that cannot be included from C++ is not a public
        header.  It matches @c UfConfigBackendSpec.redis.ns, which had the same
        constraint for the same reason. */
    const char *ns;

    /*!
     * Which configuration within the namespace.
     *
     * A namespace may hold more than one: a server keeps its listener
     * configuration and its backend configuration under the same user.  This
     * names the one this handle is bound to.
     *
     * Required by the stores that key on a namespace — leaving it NULL is
     * refused at load rather than defaulted, because a wrong guess here reads
     * a different configuration's contents rather than failing.
     *
     * Lowercase letters, digits, underscore and hyphen.
     */
    const char *config_name;
} UfConfigBackend;

/*!
 * Everything the module needs, supplied by the consumer at initialisation.
 *
 * The library holds no schema of its own.  It is machinery: a parser, a
 * validator, a canonicaliser and a driver layer, none of which is meaningful
 * without a schema to act on.  The schema is generated from the consumer's own
 * files by the generation pipeline and compiled into the consumer, and this
 * struct is how it reaches the library.
 *
 * ## What is required
 *
 * @c fields, @c field_count and @c lookup are all required.  A descriptor
 * missing any of them is refused at initialisation rather than accepted and
 * failed later: a handle that cannot validate is not a handle worth returning,
 * and the failure would otherwise surface on the first field access.
 *
 * @c lookup must resolve a path to the field's index in @c fields, or -1 when
 * the path is not declared.  The generator emits one; a consumer with no
 * generated index can supply a linear search, which is correct if slower.
 *
 * ## What is optional
 *
 * @c uf_logger may be NULL, in which case the module logs nothing.  It is
 * supplied rather than created because a consumer running several libraries
 * wants one log with one destination and one severity floor, not one per
 * module — and because the lifetime of that logger is the consumer's business,
 * so the module must never take ownership of it.
 *
 * @code{.c}
 * // Produced by the generation pipeline and compiled into the application:
 * extern const struct UfConfigFieldDesc g_ufconfig_fields[];
 * extern const int                      g_ufconfig_field_count;
 * extern int UfConfigLookupPath(const char *path);
 *
 * UfConfigDescriptor descriptor = {
 *     .fields      = g_ufconfig_fields,
 *     .field_count = (size_t)g_ufconfig_field_count,
 *     .lookup      = UfConfigLookupPath,
 *     .uf_logger   = app_logger,      // or NULL
 * };
 *
 * UfConfig *h = NULL;
 * if (UfConfigCreate(&h, &descriptor) != UF_CONFIG_OK) { ... }
 * @endcode
 */
typedef struct UfConfigDescriptor {
    const struct UfConfigFieldDesc *fields;      ///< the schema's field table
    size_t                          field_count; ///< entries in @c fields
    /*! Resolves a path to its index in @c fields, or -1 if undeclared. */
    int                           (*lookup)(const char *path);
    struct UfLogger                *uf_logger;   ///< optional; NULL logs nothing

    /*!
     * Where the configuration is read from and written back to.
     *
     * Filled in here so a caller states it once, at create, rather than
     * re-supplying it on every load — and so that reload and persist
     * unambiguously know where the content came from.
     */
    UfConfigBackendKind kind;      ///< which endpoint below applies

    UfConfigBackend    redis;     ///< read when @c kind is REDIS
    UfConfigBackend    sql;       ///< read when @c kind is SQL

    /*!
     * The file mapping a field's full node path to its decryption key.
     *
     * Only consulted when the schema declares at least one field with
     * @c encrypted = true — a schema with no such field never opens it, so
     * supplying this is unnecessary for every configuration that has no
     * secrets.  One entry per line, `ufsrv.ufnet.db_backend.password=<64 hex>`.
     *
     * When NULL, `ufconfig.secrets` in the process's current directory is used
     * instead.  A path that is supplied but unreadable is an error and does
     * *not* fall back: honouring a default after an explicit path failed would
     * let anyone able to write into the working directory substitute the keys.
     *
     * Copied at @ref UfConfigCreate, so the caller's buffer may go out of scope
     * once the call returns.
     */
    const char        *config_file_secrets;
} UfConfigDescriptor;

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_UFCONFIG_UFCONFIG_TYPE_H */
