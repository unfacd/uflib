/**
 * @file ufconfig_type_priv.h
 * @brief The ufconfig module's internal types: the parsed node, the arena that
 *        owns it, the handle, and the schema descriptor.
 *
 * Not installed, and not reachable from a consumer.  Nothing here appears in an
 * installed header, and a consumer that could see these types would be able to
 * depend on the parse tree's shape — which is precisely what the public
 * opaque-handle interface exists to prevent.
 *
 * ## Why the node is shaped the way it is
 *
 * A node is one of four kinds and, when it is a leaf, holds exactly one value.
 * That value is held in a tagged union: @ref UfNode.v carries the four
 * representations, @ref UfNode.vkind names the one that is live, and exactly one
 * arm is ever populated.  Within the live arm the raw value is what the document
 * said and the effective value is what the schema's transform made of it, so a
 * read site is the same whichever stage produced the value it wants.
 *
 * The union replaced a flat layout that carried all four representations side
 * by side, 200 bytes a node against 160.  The flat layout's stated defence was
 * that "both are live at once" — true of the text arm, where @c s.eff is a
 * separate buffer that a transform writes (@ref sApplyStringField), but not of
 * the three scalar arms: every assignment to a scalar @c eff is an assignment of
 * its own @c raw, so those arms held one value in two fields.
 *
 * The cost of the union is that a read of a dead arm is undefined and silent.
 * Every read site is reached through a @c vkind test; that is a precondition of
 * this layout, not a convention.  A read that is not so guarded is a defect
 * under the flat layout too — it returned a zeroed field rather than a garbage
 * one — so the union narrows the margin for error rather than introducing it.
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

#ifndef UFLIB_UFCONFIG_UFCONFIG_TYPE_PRIV_H
#define UFLIB_UFCONFIG_UFCONFIG_TYPE_PRIV_H

#include <stdatomic.h>
#include <pthread.h>

#include <uflib/ufconfig/ufconfig.h>
#include <uflib/utils_secrets.h>

#include "ufconfig_defs_priv.h"

/*!
 * What a node represents.
 */
typedef enum UfNodeKind
{
  UF_NODE_SCOPE = 1, ///< a table
  UF_NODE_LEAF,      ///< a scalar value
  UF_NODE_ARRAY,     ///< a table whose children are consecutive 0-based indices
  UF_NODE_ALIAS      ///< a binding that names another table
} UfNodeKind;

/*!
 * A bump allocator, in a chain of blocks.
 *
 * The whole tree, every string in it, and every string a transform produces are
 * allocated here, so destroying a configuration is releasing a handful of
 * blocks rather than walking the tree.  Blocks are never freed individually.
 */
typedef struct UfArena
{
  char *          buf;  ///< block base
  size_t          cap;  ///< block capacity in bytes
  size_t          used; ///< bytes handed out so far
  struct UfArena *next; ///< next block, or NULL
} UfArena;

typedef struct UfNode UfNode;

/*!
 * One node of the parsed tree.
 *
 * @c name and @c path are both stored.  @c path is derivable by walking to the
 * root, but it is materialised because the schema is applied and the index is
 * built by path, and recomputing it per lookup would cost more than the eight
 * bytes it occupies.
 */
struct UfNode
{
  UfNodeKind                      kind;         ///< what this node represents
  char *                          name;         ///< this node's own name
  char *                          path;         ///< full dotted path from the root
  const struct UfConfigFieldDesc *desc;         ///< schema descriptor, or NULL
  int                             line;         ///< 1-based source line
  int                             column;       ///< 1-based source column
  UfNode *                        parent;       ///< enclosing node, or NULL at the root
  UfNode *                        first_child;  ///< first child, or NULL
  UfNode *                        next_sibling; ///< next sibling, or NULL

  UfConfigValueKind vkind; ///< which arm of @c v is the live one

  /*!
   * The value.  Exactly one arm is live, and @c vkind names it — a node holds
   * one value, so the four representations do not need to co-exist.  A read
   * must be reached through the tag; reading a dead arm is undefined and the
   * compiler will not diagnose it.
   *
   * @c raw is what the document gave, @c eff what the schema's transform made
   * of it.  Both are kept per kind, so a read site is unchanged by which stage
   * produced the value it wants.
   */
  union
  {
    struct { int64_t raw, eff; } i;                                      ///< integer value
    struct { double  raw, eff; } f;                                      ///< float value
    struct { bool    raw, eff; } b;                                      ///< boolean value
    struct { char *raw; size_t raw_len; char *eff; size_t eff_len; } s;   ///< text value
  } v;

  unsigned char digest[32];   ///< canonical digest of this subtree
  char *        alias_target; ///< name an alias points at, or NULL
  UfNode *      resolved;     ///< the node an alias resolved to, or NULL
  size_t        array_count;  ///< number of elements, for an array node
};

/*!
 * The superseded whole-document backend seam.
 *
 * Retained while callers migrate to @ref UfConfigDriver.  It can only replace a
 * document wholesale, in text, so nothing reaching a store this way can be read
 * or written per field.
 */
typedef struct UfConfigBackendOps
{
  UfConfigStatus (*read_all)(void *ctx, const char *ident, char **buf, size_t *len);
  UfConfigStatus (*write_all)(void *ctx, const char *ident, const char *buf, size_t len);
  void *           ctx;
} UfConfigBackendOps;

/*!
 * An open configuration.
 *
 * @c by_id indexes the tree by schema field identifier, so a caller reading a
 * declared field by index reaches its node without a walk.  It is rebuilt on
 * every successful load.
 *
 * @c retired holds arenas from previous loads.  A reload moves the arena that
 * was current into this list rather than releasing it, so a string a reader
 * borrowed before the reload is still backed by live memory.  Retirement is
 * what makes concurrent reload safe for readers; a pin held across a reload
 * keeps the retired arena alive.
 */
/*!
 * One slot of the undeclared-path index.
 *
 * @c node is NULL when the slot is free, which is also how an empty slot is
 * recognised — the hash alone cannot say, since a real path can hash to zero.
 */
typedef struct UfLenientSlot
{
  uint64_t hash; ///< FNV-1a of the node's path; meaningless when @c node is NULL
  UfNode * node; ///< the node, or NULL when the slot is free
} UfLenientSlot;

struct UfConfig
{
  /*!
   * The schema this handle was opened against.
   *
   * Copied from the caller's descriptor at initialisation, so the caller's
   * struct may go out of scope.  The table and the function it points at must
   * outlive the handle — they are the consumer's generated objects.
   *
   * A handle from @ref ConfigCreate -- the internal raw allocator -- carries
   * a zeroed descriptor and @c is_described is false: it can parse and
   * serialise, but it has no schema to validate against, and says so rather
   * than pretending.  No consumer can reach that state: UfConfigCreate always
   * binds a schema.
   */
  UfConfigDescriptor descriptor;
  int                is_described; ///< whether @c descriptor carries a schema

  UfArena *            arena;           ///< the arena the current tree lives in
  UfArena *            retired;         ///< arenas from previous loads, not yet released
  UfNode *             root;            ///< the parsed tree, or NULL before a load
  UfNode **            by_id;           ///< node per schema field id, or NULL
  int                  by_id_n;         ///< length of @c by_id

  /*!
   * Index of the paths the schema does not declare, keyed by full path.
   *
   * The injected perfect hash answers a declared path in one probe, but a path
   * it does not know falls through to a walk of the tree, and that walk scans
   * siblings linearly.  A document loaded leniently may carry a scope of any
   * width, so that scan is unbounded in what the document's author chose.
   *
   * This covers exactly the paths the hash cannot, so a strict load — where
   * every leaf is declared — holds only the intermediate scopes, which the
   * schema bounds.  A lenient load holds the undeclared remainder as well,
   * which is proportional to the part of the document that does not conform
   * rather than to the document.  Nothing here is proportional to declared
   * content, and nothing is allocated at all when a document has no scopes.
   */
  UfLenientSlot *      unlisted;      ///< undeclared-path index, or NULL
  size_t               unlisted_n;    ///< slots in @c unlisted; a power of two, or 0
  size_t               unlisted_used; ///< occupied slots
  UfConfigBackendOps   backend;         ///< superseded write-through seam
  char *               source_path;     ///< where this was loaded from, or NULL
  /*!
   * The parsed secrets file, or NULL when the schema declares no secret.
   *
   * Only opened when UfConfigDescriptor.fields carries a field with
   * @c is_encrypted, so a configuration with no secrets never touches the
   * filesystem here and @ref UfConfigCreate keeps its freedom from I/O.
   *
   * Keys are held once, here, and looked up by path when a field is decrypted
   * rather than being copied per field onto the handle: one copy is one thing
   * to wipe, and it is wiped when this is closed.  The lookup is a scan of a
   * handful of entries, on the load path rather than the read path.
   */
  UflibSecretFile *    secrets;
  unsigned char        digest[32];      ///< canonical digest of the whole tree
  unsigned char        file_sha[32];    ///< digest of the source bytes as read
  int                  is_file_sha_set; ///< whether @c file_sha carries a value
  uint64_t             stamp;           ///< source mtime+size token, for cheap reload tests
  atomic_uint_fast64_t generation;      ///< bumped on every successful mutation
  atomic_int           readers;         ///< readers currently inside the handle
  pthread_rwlock_t     lock;            ///< guards the tree against structural change

  /*!
   * Serialises writers against each other, and only writers.
   *
   * A build is exclusive against other writers because they contend for
   * @c arena -- every writer allocates, no reader does.  It does not need to be
   * exclusive against readers, because everything it touches before the publish
   * is unpublished: it works in its own arena and leaves @c root, @c by_id and
   * @c unlisted alone until the swap.  Holding the rwlock for the whole build
   * instead is what made a reload block every lookup for ~318 us.
   *
   * Order is always build_lock then @c lock, never the reverse.
   */
  pthread_mutex_t      build_lock;
  UfConfigLoadOptions  load_opt;        ///< options the current tree was loaded with
  int                  is_load_opt_set; ///< whether @c load_opt carries a value
  UfConfigLoadReport   last_report;     ///< notes from the most recent load
  UfConfigReloadEntry *reload_store;    ///< backing store for reload reports
  size_t               reload_store_n;  ///< entries used in @c reload_store
};

/* The schema's types -- UfFieldType and UfConfigFieldDesc -- are part of the
   public contract and live in <uflib/ufconfig/ufconfig_type.h>.  They are the
   consumer's schema, generated from the consumer's files and injected at
   initialisation, so they are not this module's to keep private. */

#endif /* UFLIB_UFCONFIG_UFCONFIG_TYPE_PRIV_H */
