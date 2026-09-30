/**
 * @file ufconfig.c
 * @brief The module's core: the handle's lifecycle, schema application, and the field
 *        accessors and mutators.
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

#include "ufconfig_priv.h"

#include <uflib/logger/logger.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A consumer that injected no logger gets silence -- not a crash, and not a
   default destination it did not choose.  The logger macros capture __FILE__
   and __LINE__ at the call site, which is why these are macros and not
   functions. */
#define UFCONFIG_LOG_INFO(h, ...)  do { if ((h)->descriptor.uf_logger) \
    UF_LOGGER_INFO((h)->descriptor.uf_logger, __VA_ARGS__); } while (0)
#define UFCONFIG_LOG_WARN(h, ...)  do { if ((h)->descriptor.uf_logger) \
    UF_LOGGER_WARN((h)->descriptor.uf_logger, __VA_ARGS__); } while (0)
#define UFCONFIG_LOG_ERROR(h, ...) do { if ((h)->descriptor.uf_logger) \
    UF_LOGGER_ERROR((h)->descriptor.uf_logger, __VA_ARGS__); } while (0)

/* The schema this handle was opened against.  There is no global one: the
   module is machinery, and the schema is the consumer's, injected through
   UfConfigCreate.  These three access the injected copy. */
static const UfConfigFieldDesc *sFields(const UfConfig *h)
{
  return h->is_described ? h->descriptor.fields : NULL;
}

static size_t sFieldCount(const UfConfig *h)
{
  return h->is_described ? h->descriptor.field_count : 0;
}

static int sLookupField(const UfConfig *h, const char *path)
{
  if (!h->is_described || !h->descriptor.lookup) return -1;
  return h->descriptor.lookup(path);
}

static void sRebuildIndex(UfConfig *h);

UfConfigStatus ConfigCreate(UfConfig **out)
{
  if (!out) return UF_CONFIG_ERR_INVALID_ARG;
  UfConfig *h = (UfConfig*)calloc(1, sizeof(*h));
  if (!h) return UF_CONFIG_ERR_NO_MEMORY;
  pthread_rwlock_init(&h->lock, NULL);
  pthread_mutex_init(&h->build_lock, NULL);
  atomic_store(&h->generation, 1);
  h->backend.read_all  = ConfigFileReadAllDefault;
  h->backend.write_all = ConfigFileWriteAllDefault;
  *out                 = h;
  return UF_CONFIG_OK;
}

/*!
 * @brief Whether the schema declares a field that carries an envelope.
 *
 * The trigger for the whole secrets mechanism, and deliberately a property of
 * the *schema* rather than of any document.  Not every field is mandatory, so a
 * document may legitimately omit an encrypted field — and a handle is created
 * before any document has been read at all.  Asking the schema is therefore the
 * only question that can be answered at both points, and the only one whose
 * answer does not change when a document is edited.
 */
static int sSchemaDeclaresSecret(const UfConfigDescriptor *d)
{
  for (size_t i = 0; i < d->field_count; i++) {
    if (d->fields[i].is_encrypted) return 1;
  }
  return 0;
}

/*!
 * @brief Opens the secrets file and checks every declared secret is keyed.
 *
 * Resolved once, at create, rather than per load: a server that cannot read its
 * keys should not start, the keys are then read once instead of on every
 * reload, and the load path stays free of filesystem access.
 */
static UfConfigStatus sResolveSecrets(UfConfig *h)
{
  const UfConfigDescriptor *d = &h->descriptor;

  const char *path       = d->config_file_secrets;
  int         is_default = 0;
  if (!path || !*path) {
    path       = PRIV_CONFIG_DEFAULT_UFCONFIG_SECRETS_FILE;
    is_default = 1;
  }

  UflibSecretFile *f   = NULL;
  UflibSecretStatus sst = UflibSecretFileOpen(path, &f);
  if (sst != UFLIB_SECRET_OK) {
    /* No fallback to the default here even when the caller named a path that
       merely could not be opened.  Falling back would mean that anyone able to
       create a file in the working directory could substitute the keys for a
       deployment that had already said where its keys are.  An explicit path is
       authoritative, or there is no configuration. */
    if (is_default) {
      ConfigSetError(UF_CONFIG_ERR_SECRET, path, "secrets", 0, 0,
                     "no secrets file: '%s' is unreadable (%s), and the schema declares "
                     "an encrypted field", path, UflibSecretStatusString(sst));
    }
    else {
      ConfigSetError(UF_CONFIG_ERR_SECRET, path, "secrets", 0, 0,
                     "secrets file '%s' is unreadable (%s)", path, UflibSecretStatusString(sst));
    }
    return UF_CONFIG_ERR_SECRET;
  }

  /* Every declared secret needs a key before the handle exists, so a missing one
     is reported against the field that lacks it rather than surfacing later as
     an authentication failure on a value that looks perfectly well-formed. */
  for (size_t i = 0; i < d->field_count; i++) {
    if (!d->fields[i].is_encrypted) continue;
    if (!UflibSecretFileHas(f, d->fields[i].path)) {
      ConfigSetError(UF_CONFIG_ERR_SECRET, d->fields[i].path, "secrets", 0, 0,
                     "%s: declared encrypted, but '%s' holds no key for it",
                     d->fields[i].path, UflibSecretFilePath(f));
      UflibSecretFileClose(f);
      return UF_CONFIG_ERR_SECRET;
    }
  }

  h->secrets = f;
  UFCONFIG_LOG_INFO(h, "config: secrets read from %s%s", path,
                    is_default ? " (default location)" : "");
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigCreate(UfConfig **out, const UfConfigDescriptor *descriptor_ptr)
{
  if (!out || !descriptor_ptr) return UF_CONFIG_ERR_INVALID_ARG;

  /* Refused here rather than accepted and failed later.  A handle that cannot
     validate is not a handle worth returning: the failure would otherwise
     surface as an inexplicable NOFIELD on the first field read, a long way
     from the descriptor that caused it. */
  if (!descriptor_ptr->fields || descriptor_ptr->field_count == 0 || !descriptor_ptr->lookup) {
    return UF_CONFIG_ERR_NOT_CONFIGURED;
  }

  UfConfigStatus st = ConfigCreate(out);
  if (st != UF_CONFIG_OK) return st;

  /* Copied, so the caller's struct may go out of scope.  What it points at
     may not: the table and the lookup are the consumer's generated objects
     and must outlive every handle opened against them. */
  (*out)->descriptor   = *descriptor_ptr;
  (*out)->is_described = 1;

  /* Only reached when the schema actually declares a secret, so a configuration
     with none performs no I/O here and cannot fail for an environmental reason
     — which is every existing consumer. */
  if (sSchemaDeclaresSecret(&(*out)->descriptor)) {
    st = sResolveSecrets(*out);
    if (st != UF_CONFIG_OK) {
      /* Destroy rather than a bare free: the handle owns a lock and two
         allocated indexes by now, and this is the first failure path in this
         function that has any of that to release. */
      UfConfigDestroy(*out);
      *out = NULL;
      return st;
    }
  }

  UFCONFIG_LOG_INFO(*out, "config: initialised with %zu declared fields", descriptor_ptr->field_count);
  return UF_CONFIG_OK;
}

PUBLIC_API void UfConfigDestroy(UfConfig *h)
{
  if (!h) return;
  ConfigArenaFree(h->arena);
  ConfigArenaFree(h->retired);
  free(h->by_id);
  free(h->unlisted);
  free(h->source_path);
  /* Wipes every key it holds before releasing them; a freed block keeps its
     contents, and a core dump would carry them out of the process. */
  UflibSecretFileClose(h->secrets);
  if (h->reload_store) {
    for (size_t i = 0; i < h->reload_store_n; i++) free((char*)h->reload_store[i].path);
    free(h->reload_store);
  }
  pthread_rwlock_destroy(&h->lock);
  pthread_mutex_destroy(&h->build_lock);
  free(h);
}

static uint64_t sFileStampPath(const char *path)
{
  struct stat sb;
  if (stat(path, &sb) != 0) return 0;
#if defined(__APPLE__)
  return (uint64_t)sb.st_mtime * 1000000000ull + (uint64_t)sb.st_mtimespec.tv_nsec;
#else
  return (uint64_t)sb.st_mtim.tv_sec * 1000000000ull + (uint64_t)sb.st_mtim.tv_nsec;
#endif
}

UfConfigStatus ConfigFileReadAll(const char *path, char **out, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    ConfigSetError(UF_CONFIG_ERR_BACKEND, path, NULL, 0, 0, "file backend: cannot open %s", path);
    return UF_CONFIG_ERR_BACKEND;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return UF_CONFIG_ERR_BACKEND;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return UF_CONFIG_ERR_BACKEND;
  }
  if ((size_t)sz > PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_FILE_BYTES) {
    fclose(f);
    ConfigSetError(UF_CONFIG_ERR_LIMIT_EXCEEDED, path, NULL, 0, 0, "file too large");
    return UF_CONFIG_ERR_LIMIT_EXCEEDED;
  }
  rewind(f);
  char *buf = (char*)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return UF_CONFIG_ERR_NO_MEMORY;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = 0;
  *out   = buf;
  *len   = n;
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigFileReadAllDefault(void *ctx, const char *path, char **out, size_t *len)
{
  (void)ctx;
  return ConfigFileReadAll(path, out, len);
}

UfConfigStatus ConfigFileWriteAllDefault(void *ctx, const char *path, const char *buf, size_t len)
{
  (void)ctx;
  FILE *f = fopen(path, "wb");
  if (!f) return UF_CONFIG_ERR_BACKEND;
  if (len && fwrite(buf, 1, len, f) != len) {
    fclose(f);
    return UF_CONFIG_ERR_BACKEND;
  }
  fclose(f);
  return UF_CONFIG_OK;
}

static void sAssignPaths(UfArena **a, UfNode *n, const char *prefix)
{
  char buf[CONFIG_DEFAULT_UFCONFIG_PATH_MAX];
  if (n->name && n->name[0] && strcmp(n->name, "#bindings") != 0) {
    if (prefix && prefix[0]) snprintf(buf, sizeof(buf), "%s.%s", prefix, n->name);
    else snprintf(buf, sizeof(buf), "%s", n->name);
    n->path = ConfigArenaStrndup(a, buf, strlen(buf));
  }
  else {
    n->path = ConfigArenaStrndup(a, prefix ? prefix : "", prefix ? strlen(prefix) : 0);
  }
  const char *p = n->path ? n->path : "";
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      sAssignPaths(a, c, "");
      continue;
    }
    sAssignPaths(a, c, p);
  }
}

static const UfConfigFieldDesc *sDescFor(const UfConfig *h, const char *path)
{
  if (!path || !h->is_described) return NULL;
  int id = sLookupField(h, path);
  if (id < 0 || (size_t)id >= sFieldCount(h)) return NULL;
  return &sFields(h)[id];
}

static UfConfigStatus sCollectUnknown(const UfConfig *h, UfNode *    n, UfConfigLoadMode mode, UfConfigLoadNote *notes,
                                      size_t *        nnotes, size_t cap, int *          strict_fail)
{
  if (n->name && strcmp(n->name, "#bindings") == 0) {
    for (UfNode *b = n->first_child; b; b = b->next_sibling) sCollectUnknown(
      h, b, mode, notes, nnotes, cap, strict_fail);
    return UF_CONFIG_OK;
  }
  if (n->path && n->path[0] && n->kind != UF_NODE_ALIAS) {
    const UfConfigFieldDesc *d = sDescFor(h, n->path);
    n->desc                    = d;
    if (!d && n->name && n->name[0] && strchr(n->path, '.')) {
      int numeric = 1;
      for (const char *q = n->name; *q; q++) if (*q < '0' || *q > '9') {
        numeric = 0;
        break;
      }
      int known_prefix = numeric;
      if (!known_prefix) {
        for (size_t i = 0; i < sFieldCount(h); i++) {
          const char *fp = sFields(h)[i].path;
          size_t      pl = strlen(n->path);
          if (strncmp(fp, n->path, pl) == 0 && (fp[pl] == 0 || fp[pl] == '.')) {
            known_prefix = 1;
            break;
          }
        }
      }
      int child_of_unknown = 0;
      for (size_t k = 0; k < *nnotes; k++) {
        size_t pl = strlen(notes[k].path);
        if (strncmp(n->path, notes[k].path, pl) == 0 && n->path[pl] == '.') {
          child_of_unknown = 1;
          break;
        }
      }
      if (!known_prefix && !child_of_unknown) {
        if (*nnotes < cap) {
          notes[*nnotes].kind   = UF_CONFIG_NOTE_UNKNOWN;
          notes[*nnotes].path   = n->path;
          notes[*nnotes].line   = n->line;
          notes[*nnotes].column = n->column;
          (*nnotes)++;
        }
        if (mode == UF_CONFIG_LOAD_STRICT) *strict_fail = 1;
      }
    }
  }
  for (UfNode *c = n->first_child; c; c = c->next_sibling) sCollectUnknown(h, c, mode, notes, nnotes, cap, strict_fail);
  return UF_CONFIG_OK;
}

static void sAddChildN(UfNode *parent, UfNode *c)
{
  c->parent = parent;
  if (!parent->first_child) parent->first_child = c;
  else {
    UfNode *x = parent->first_child;
    while (x->next_sibling) x = x->next_sibling;
    x->next_sibling = c;
  }
}

static UfNode *sEnsureScope(UfConfig *h, UfNode *root, const char *path)
{
  if (!path || !path[0]) return root;
  UfNode *n = ConfigFindPath(root, path);
  if (n) {
    if (n->kind == UF_NODE_ALIAS && n->resolved) return n->resolved;
    return n;
  }
  const char *dot    = strrchr(path, '.');
  UfNode *    parent = root;
  const char *name   = path;
  char        parent_path[512];
  if (dot) {
    size_t pl = (size_t)(dot - path);
    if (pl >= sizeof(parent_path)) return NULL;
    memcpy(parent_path, path, pl);
    parent_path[pl] = 0;
    parent          = sEnsureScope(h, root, parent_path);
    name            = dot + 1;
  }
  if (!parent) return NULL;
  UfNode *sc = (UfNode*)ConfigArenaAlloc(&h->arena, sizeof(UfNode));
  if (!sc) return NULL;
  memset(sc, 0, sizeof(*sc));
  sc->kind = UF_NODE_SCOPE;
  sc->name = ConfigArenaStrndup(&h->arena, name, strlen(name));
  sc->path = ConfigArenaStrndup(&h->arena, path, strlen(path));
  sAddChildN(parent, sc);
  return sc;
}

static UfNode *sMaterialiseDefault(UfConfig *h, UfNode *root, const struct UfConfigFieldDesc *d)
{
  const char *dot    = strrchr(d->path, '.');
  UfNode *    parent = root;
  const char *name   = d->path;
  if (dot) {
    char   pp[512];
    size_t pl = (size_t)(dot - d->path);
    if (pl >= sizeof(pp)) return NULL;
    memcpy(pp, d->path, pl);
    pp[pl] = 0;
    parent = sEnsureScope(h, root, pp);
    name   = dot + 1;
  }
  if (!parent) return NULL;
  UfNode *n = (UfNode*)ConfigArenaAlloc(&h->arena, sizeof(UfNode));
  if (!n) return NULL;
  memset(n, 0, sizeof(*n));
  n->kind = UF_NODE_LEAF;
  n->name = ConfigArenaStrndup(&h->arena, name, strlen(name));
  n->path = ConfigArenaStrndup(&h->arena, d->path, strlen(d->path));
  n->desc = d;
  switch (d->type) {
  case UF_FT_INT: n->vkind = UF_CONFIG_KIND_INT;
    n->v.i.raw = n->v.i.eff = d->d_int;
    break;
  case UF_FT_FLOAT: n->vkind = UF_CONFIG_KIND_FLOAT;
    n->v.f.raw = n->v.f.eff = d->d_float;
    break;
  case UF_FT_BOOL: n->vkind = UF_CONFIG_KIND_BOOL;
    n->v.b.raw = n->v.b.eff = d->d_bool;
    break;
  case UF_FT_STR:
    n->vkind = UF_CONFIG_KIND_STRING;
    n->v.s.raw     = ConfigArenaStrndup(&h->arena, d->d_str ? d->d_str : "", d->d_str ? strlen(d->d_str) : 0);
    n->v.s.raw_len = d->d_str ? strlen(d->d_str) : 0;
    n->v.s.eff     = n->v.s.raw;
    n->v.s.eff_len = n->v.s.raw_len;
    break;
  default: n->kind = UF_NODE_SCOPE;
    break;
  }
  sAddChildN(parent, n);
  return n;
}

/*!
 * @brief Turns a field's stored envelope into the plaintext it holds.
 *
 * On success @p out and @p out_len name a copy in the handle's arena, so the
 * plaintext lives exactly as long as the tree it belongs to.  The heap buffer
 * the decryption produced is wiped and released here rather than being handed
 * on: the arena is the only copy, and freeing key-derived bytes without wiping
 * them leaves them readable in the allocator.
 *
 * The diagnostic deliberately never quotes the value.  Every other rejection in
 * this module names what it refused, which is right for a configuration value
 * and wrong for a secret — the message would carry the plaintext into the error
 * struct that every caller can read.
 */
static UfConfigStatus sDecryptSecret(UfConfig *h, const struct UfConfigFieldDesc *d,
                                     const char *raw, const char **out, size_t *out_len)
{
  if (!h->secrets) {
    ConfigSetError(UF_CONFIG_ERR_SECRET, d->path, "secrets", 0, 0,
                   "%s: no secrets file is open", d->path);
    return UF_CONFIG_ERR_SECRET;
  }

  unsigned char key[UFLIB_SECRET_KEY_BYTES];
  UflibSecretStatus kst = UflibSecretFileLookup(h->secrets, d->path, key);
  if (kst != UFLIB_SECRET_OK) {
    ConfigSetError(UF_CONFIG_ERR_SECRET, d->path, "secrets", 0, 0,
                   "%s: %s in '%s'", d->path, UflibSecretStatusString(kst),
                   UflibSecretFilePath(h->secrets));
    return UF_CONFIG_ERR_SECRET;
  }

  /* A parsed leaf is always terminated -- both the parser and the pair builder
     allocate through ConfigArenaStrndup -- so the envelope can be read as a C
     string.  A value carrying an embedded NUL is therefore seen truncated, and
     a truncated envelope cannot authenticate, so it is refused rather than
     being read past its end. */
  char  *plaintext = NULL;
  size_t plain_len = 0;
  UflibSecretStatus sst = UflibSecretDecrypt(raw, key, &plaintext, &plain_len);
  UflibSecretWipe(key, sizeof(key));

  if (sst != UFLIB_SECRET_OK) {
    if (plaintext) {
      UflibSecretWipe(plaintext, plain_len);
      free(plaintext);
    }
    ConfigSetError(UF_CONFIG_ERR_SECRET, d->path, "secrets", 0, 0, "%s: %s", d->path,
                   UflibSecretStatusString(sst));
    return UF_CONFIG_ERR_SECRET;
  }

  char *copy = ConfigArenaStrndup(&h->arena, plaintext, plain_len);
  UflibSecretWipe(plaintext, plain_len);
  free(plaintext);
  if (!copy) {
    ConfigSetError(UF_CONFIG_ERR_NO_MEMORY, d->path, "secrets", 0, 0,
                   "%s: cannot hold the decrypted value", d->path);
    return UF_CONFIG_ERR_NO_MEMORY;
  }

  *out     = copy;
  *out_len = plain_len;
  return UF_CONFIG_OK;
}

static UfConfigStatus sApplyStringField(UfConfig *h, UfNode *n, const struct UfConfigFieldDesc *d)
{
  if (!n || n->kind != UF_NODE_LEAF || n->vkind != UF_CONFIG_KIND_STRING) return UF_CONFIG_OK;
  const char *cur  = n->v.s.raw;
  size_t      clen = n->v.s.raw_len;

  /* Decryption happens before the named transforms, not after.  The envelope is
     the form the document stores the value in, so it is what has to be undone
     first; a transform is a normalisation of the value itself, and normalising
     a ciphertext would be meaningless.  Everything downstream -- the transforms
     and then ConfigValidateString -- is therefore looking at the secret, which
     is what a declared length or format on the field was written to constrain. */
  if (d && d->is_encrypted) {
    UfConfigStatus ss = sDecryptSecret(h, d, cur, &cur, &clen);
    if (ss != UF_CONFIG_OK) return ss;
  }

  if (d && d->transforms && d->transforms[0]) {
    char ops[256];
    snprintf(ops, sizeof(ops), "%s", d->transforms);
    char *save = NULL;
    for (char *tok = strtok_r(ops, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      char *         produced = NULL;
      size_t         plen     = 0;
      UfConfigStatus ts       = ConfigTransformApplyNamed(&h->arena, tok, cur, clen, &produced, &plen);
      if (ts) return ts;
      cur  = produced;
      clen = plen;
    }
  }
  n->v.s.eff     = (char*)cur;
  n->v.s.eff_len = clen;
  if (d) return ConfigValidateString(d, n->v.s.eff ? n->v.s.eff : "", n->v.s.eff_len, n->line, n->column);
  return UF_CONFIG_OK;
}

/* ── names for the type check's diagnostics ─────────────────────────────── */

static const char *sTypeName(UfFieldType t)
{
  switch (t) {
  case UF_FT_INT:   return "integer";
  case UF_FT_FLOAT: return "float";
  case UF_FT_BOOL:  return "boolean";
  case UF_FT_STR:   return "string";
  case UF_FT_ARR:   return "array";
  case UF_FT_TAB:   return "table";
  default:          return "unknown";
  }
}

static const char *sKindName(UfConfigValueKind k)
{
  switch (k) {
  case UF_CONFIG_KIND_INT:    return "an integer";
  case UF_CONFIG_KIND_FLOAT:  return "a float";
  case UF_CONFIG_KIND_BOOL:   return "a boolean";
  case UF_CONFIG_KIND_STRING: return "a string";
  case UF_CONFIG_KIND_BYTES:  return "bytes";
  case UF_CONFIG_KIND_ABSENT: return "nil";
  default:                    return "of no kind";
  }
}

static const char *sNodeKindName(UfNodeKind k)
{
  switch (k) {
  case UF_NODE_LEAF:  return "a value";
  case UF_NODE_SCOPE: return "a table";
  case UF_NODE_ARRAY: return "an array";
  case UF_NODE_ALIAS: return "an alias";
  default:            return "of no kind";
  }
}

/*! @brief The value kind a field of type @p t must carry, or ABSENT if @p t is
 *         not a scalar and the kind is decided by the shape instead. */
static UfConfigValueKind sKindForType(UfFieldType t)
{
  switch (t) {
  case UF_FT_INT:   return UF_CONFIG_KIND_INT;
  case UF_FT_FLOAT: return UF_CONFIG_KIND_FLOAT;
  case UF_FT_BOOL:  return UF_CONFIG_KIND_BOOL;
  case UF_FT_STR:   return UF_CONFIG_KIND_STRING;
  default:          return UF_CONFIG_KIND_ABSENT;
  }
}

/*!
 * @brief Whether a node of kind @p k can carry a field declared as @p t.
 *
 * A declared array or table accepts either shape, because an empty one is a
 * table by construction — the parser marks a node UF_NODE_ARRAY only when its
 * children are the contiguous indices 0..n-1, and an empty table has none.  So
 * the two are not distinguishable at zero elements and the check does not try.
 */
static int sShapeMatches(UfFieldType t, UfNodeKind k)
{
  if (t == UF_FT_ARR || t == UF_FT_TAB) return k == UF_NODE_ARRAY || k == UF_NODE_SCOPE;
  return k == UF_NODE_LEAF;
}

UfConfigStatus ConfigApplySchema(UfConfig *h, UfNode *root, UfConfigLoadMode mode, UfConfigLoadReport *report)
{
  sAssignPaths(&h->arena, root, "");
  UfConfigLoadNote tmp[128];
  size_t           nn   = 0;
  int              fail = 0;
  sCollectUnknown(h, root, mode, tmp, &nn, 128, &fail);
  if (report) {
    report->note_count = nn;
    report->notes      = (UfConfigLoadNote*)ConfigArenaAlloc(&h->arena, sizeof(tmp));
    if (report->notes) memcpy(report->notes, tmp, nn * sizeof(tmp[0]));
  }
  if (fail) {
    ConfigSetError(UF_CONFIG_ERR_UNKNOWN_FIELD, tmp[0].path, NULL, tmp[0].line, tmp[0].column, "unknown field %s",
                   tmp[0].path);
    return UF_CONFIG_ERR_UNKNOWN_FIELD;
  }
  for (size_t i = 0; i < sFieldCount(h); i++) {
    const UfConfigFieldDesc *d = &sFields(h)[i];
    UfNode *                 n = ConfigFindPath(root, d->path);
    if (n && n->kind == UF_NODE_ALIAS) n = n->resolved;
    if (!n) {
      if (d->is_required) {
        ConfigSetError(UF_CONFIG_ERR_REQUIRED, d->path, "required", 0, 0, "required field %s missing", d->path);
        return UF_CONFIG_ERR_REQUIRED;
      }
      if (!d->has_default) continue;
      n = sMaterialiseDefault(h, root, d);
      if (!n) continue;
    }
    n->desc = d;

    /* The check nothing performed.  Each scalar check below is gated on the
       value already being of the right kind, so a value of the wrong kind
       satisfied none of them and fell through unexamined -- a string on an
       integer field, or an integer on a boolean, loaded cleanly in both strict
       and lenient.  The kind is what the parser saw; the declared type is what
       the schema wants; a mismatch is refused, as it is for every other rule
       here.  A nil is an absent value rather than a wrong-typed one. */
    if (!sShapeMatches(d->type, n->kind)) {
      ConfigSetError(UF_CONFIG_ERR_TYPE_MISMATCH, d->path, "type", n->line, n->column,
                     "%s: declared %s, document gave %s", d->path, sTypeName(d->type), sNodeKindName(n->kind));
      return UF_CONFIG_ERR_TYPE_MISMATCH;
    }
    if (n->kind == UF_NODE_LEAF && n->vkind != UF_CONFIG_KIND_ABSENT && n->vkind != sKindForType(d->type)) {
      ConfigSetError(UF_CONFIG_ERR_TYPE_MISMATCH, d->path, "type", n->line, n->column,
                     "%s: declared %s, document gave %s", d->path, sTypeName(d->type), sKindName(n->vkind));
      return UF_CONFIG_ERR_TYPE_MISMATCH;
    }

    /* The ceiling is on an array's element count, and an array is not a leaf.
       This used to sit inside the leaf branch below and was therefore
       unreachable: array_max was never enforced on anything. */
    if (d->has_array_max && (long long)n->array_count > d->array_max) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "array_max", n->line, n->column,
                     "%s: %zu elements, above array_max (%lld)", d->path, n->array_count, d->array_max);
      return UF_CONFIG_ERR_VALIDATION;
    }

    if (n->kind == UF_NODE_LEAF) {
      if (d->type == UF_FT_INT && n->vkind == UF_CONFIG_KIND_INT) {
        if (d->has_min && n->v.i.eff < d->vmin) {
          ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "min", n->line, n->column,
                         "%s = %lld, below min (%lld)", d->path, (long long)n->v.i.eff, d->vmin);
          return UF_CONFIG_ERR_VALIDATION;
        }
        if (d->has_max && n->v.i.eff > d->vmax) {
          ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "max", n->line, n->column,
                         "%s = %lld, above max (%lld)", d->path, (long long)n->v.i.eff, d->vmax);
          return UF_CONFIG_ERR_VALIDATION;
        }
      }
      if (d->type == UF_FT_STR && n->vkind == UF_CONFIG_KIND_STRING) {
        UfConfigStatus vs = sApplyStringField(h, n, d);
        if (vs) return vs;
      }
    }
  }
  return UF_CONFIG_OK;
}


static void sRebuildIndex(UfConfig *h)
{
  free(h->by_id);
  h->by_id_n = (int)sFieldCount(h);
  h->by_id   = (UfNode**)calloc((size_t)h->by_id_n, sizeof(UfNode*));
  if (!h->by_id) return;
  for (int i = 0; i < h->by_id_n; i++) h->by_id[i] = ConfigFindPath(h->root, sFields(h)[i].path);
}

/*!
 * @brief FNV-1a over a path.
 *
 * Deliberately the same function the generated lookup uses, so that a path and
 * its index agree on a bucket without either side knowing the other exists.
 */
static uint64_t sHashPath(const char *p)
{
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char *c = (const unsigned char *)p; *c; c++) {
    h ^= (uint64_t)*c;
    h *= 1099511628211ULL;
  }
  return h;
}

/*!
 * @brief Whether @p n belongs in the undeclared-path index.
 *
 * A node qualifies when it has a path the schema's hash does not resolve.  The
 * root's path is empty and is not addressable, and the @c #bindings holder is
 * spliced into its parent rather than being part of the document, so neither is
 * indexed.
 */
static int sIsUnlisted(const UfConfig *h, const UfNode *n)
{
  if (!n->path || !n->path[0]) return 0;
  if (n->name && strcmp(n->name, "#bindings") == 0) return 0;
  return sLookupField(h, n->path) < 0;
}

static size_t sCountUnlisted(const UfConfig *h, const UfNode *n)
{
  size_t k = sIsUnlisted(h, n) ? 1u : 0u;
  for (const UfNode *c = n->first_child; c; c = c->next_sibling) k += sCountUnlisted(h, c);
  return k;
}

static void sPutUnlisted(UfConfig *h, UfNode *n)
{
  uint64_t hh   = sHashPath(n->path);
  size_t   mask = h->unlisted_n - 1;
  size_t   i    = (size_t)hh & mask;
  while (h->unlisted[i].node) {
    /* The same path can appear twice in one tree -- the root namespace carries
       a name both as an alias and as the inline table it binds -- and the walk
       this replaces resolves the first match, so the first one in wins here
       too rather than the two routes disagreeing. */
    if (h->unlisted[i].hash == hh && strcmp(h->unlisted[i].node->path, n->path) == 0) return;
    i = (i + 1) & mask;
  }
  h->unlisted[i].hash = hh;
  h->unlisted[i].node = n;
  h->unlisted_used++;
}

/*!
 * @brief Whether @p n or anything under it is in the undeclared-path index.
 *
 * A rebuild walks the whole tree, so it is worth asking first whether the
 * subtree that is leaving held anything indexed at all.  An appended array
 * element has no path and is never indexed, so clearing an array that only
 * ever held appended elements now costs nothing rather than a full rebuild.
 */
static int sAnyUnlisted(const UfConfig *h, const UfNode *n)
{
  if (sIsUnlisted(h, n)) return 1;
  for (const UfNode *c = n->first_child; c; c = c->next_sibling)
    if (sAnyUnlisted(h, c)) return 1;
  return 0;
}

static void sFillUnlisted(UfConfig *h, UfNode *n)
{
  if (sIsUnlisted(h, n)) sPutUnlisted(h, n);
  for (UfNode *c = n->first_child; c; c = c->next_sibling) sFillUnlisted(h, c);
}

/*!
 * @brief Rebuilds the undeclared-path index from the current tree.
 *
 * Called wherever the tree changes shape, alongside @ref sRebuildIndex.  A
 * tree with nothing undeclared leaves the index NULL and allocates nothing,
 * which is the case a strict load always produces apart from its own scopes.
 */
static void sRebuildUnlisted(UfConfig *h)
{
  free(h->unlisted);
  h->unlisted      = NULL;
  h->unlisted_n    = 0;
  h->unlisted_used = 0;
  if (!h->root) return;

  size_t count = sCountUnlisted(h, h->root);
  if (!count) return;

  /* Twice the entries, rounded up to a power of two, so the probe sequence
     stays short without the table being mostly holes. */
  size_t n = 8;
  while (n < count * 2) n *= 2;
  h->unlisted = (UfLenientSlot *)calloc(n, sizeof(UfLenientSlot));
  if (!h->unlisted) return;
  h->unlisted_n = n;

  sFillUnlisted(h, h->root);
}

static UfNode *sGetUnlisted(const UfConfig *h, const char *path)
{
  if (!h->unlisted || !h->unlisted_n) return NULL;
  uint64_t hh   = sHashPath(path);
  size_t   mask = h->unlisted_n - 1;
  size_t   i    = (size_t)hh & mask;
  for (size_t probes = 0; probes < h->unlisted_n; probes++) {
    if (!h->unlisted[i].node) return NULL; /* linear probing with no deletions */
    if (h->unlisted[i].hash == hh && strcmp(h->unlisted[i].node->path, path) == 0) return h->unlisted[i].node;
    i = (i + 1) & mask;
  }
  return NULL;
}

/*!
 * @brief Resolves @p path to a node, cheapest route first.
 *
 * The schema's hash and @c by_id answer a declared field in one probe.  The
 * undeclared-path index answers everything else the tree holds, which is what
 * keeps a lenient document's unused width from costing every lookup a linear
 * scan.  The walk is the last resort: a path that exists only through an alias
 * taken mid-path is not any node's own path, so no index can hold it.
 */
/*!
 * @brief Rebuilds every index against the current tree.
 *
 * Both indexes hold pointers into the arena, so replacing the tree invalidates
 * them and they have to be rebuilt with it.  This is one call because both load
 * paths need it and only one of them made it: the text path rebuilt @c by_id
 * and the pairs path -- which is the reload path -- did not, so a reload left
 * every declared-field read and every undeclared lookup pointing into the arena
 * the reload had just released.  A reader then faulted on @c n->kind inside
 * UfConfigGetField, several frames away from the reload that caused it.
 */
void ConfigRebuildIndexes(UfConfig *h)
{
  sRebuildIndex(h);
  sRebuildUnlisted(h);
}

UfNode *ConfigResolvePath(const UfConfig *h, const char *path)
{
  if (!h || !path || !path[0]) return NULL;
  int id = sLookupField(h, path);
  if (id >= 0 && h->by_id && id < h->by_id_n && h->by_id[id]) return h->by_id[id];
  UfNode *n = sGetUnlisted(h, path);
  if (n) return n;
  return ConfigFindPath(h->root, path);
}

static UfConfigStatus sLoadBuf(UfConfig *          h, const char *buf, size_t len, const UfConfigLoadOptions *opt,
                               UfConfigLoadReport *report)
{
  ConfigClearError();
  UfNode *       root = NULL;
  UfConfigStatus st   = ConfigParseLuaStyle(h, buf, len, &root);
  if (st) return st;
  st = ConfigResolveAliases(h, root);
  if (st) return st;
  UfConfigLoadMode mode = opt ? opt->mode : UF_CONFIG_LOAD_STRICT;
  st                    = ConfigApplySchema(h, root, mode, report);
  if (st) return st;
  st = ConfigCanonAndDigest(root, h->digest);
  if (st) return st;
  h->root = root;
  if (opt) {
    h->load_opt        = *opt;
    h->is_load_opt_set = 1;
  }
  atomic_fetch_add(&h->generation, 1);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigLoadBuffer(UfConfig *h, const char *buf, size_t len, const UfConfigLoadOptions *opt,
                                             UfConfigLoadReport *report)
{
  if (!h || !buf) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfArena *     old_a    = h->arena;
  UfNode *      old_root = h->root;
  unsigned char old_d[32];
  memcpy(old_d, h->digest, 32);
  h->arena          = NULL;
  h->root           = NULL;
  UfConfigStatus st = sLoadBuf(h, buf, len, opt, report);
  if (st != UF_CONFIG_OK) {
    ConfigArenaFree(h->arena);
    h->arena = old_a;
    h->root  = old_root;
    memcpy(h->digest, old_d, 32);
  }
  else {
    if (atomic_load(&h->readers) == 0) {
      ConfigArenaFree(old_a);
      ConfigArenaFree(h->retired);
      h->retired = NULL;
    }
    else {
      if (old_a) {
        old_a->next = h->retired;
        h->retired  = old_a;
      }
    }
    ConfigRebuildIndexes(h);
  }
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return st;
}

/* Translate the caller's endpoint into the form a driver is opened with.
 *
 * This is the one place the two views of "where the store is" meet: the
 * descriptor carries one shape for every origin, because a caller filling it in
 * answers the same questions either way; a driver is handed the shape its own
 * store needs.  Keeping the translation in one function is what stops the two
 * definitions drifting apart. */
static UfConfigStatus sResolveBackend(const UfConfig *h, UfConfigBackendSpec *spec)
{
  memset(spec, 0, sizeof(*spec));
  spec->kind = h->descriptor.kind;

  switch (h->descriptor.kind) {
  case UF_CONFIG_BACKEND_FILE:
    /* The path is per-load rather than per-handle: one handle may be pointed
       at different documents.  UfConfigLoadFile records it. */
    if (!h->source_path) return UF_CONFIG_ERR_NOT_CONFIGURED;
    spec->u.file.path = h->source_path;
    return UF_CONFIG_OK;

  case UF_CONFIG_BACKEND_MEM:
    if (!h->descriptor.redis.address) return UF_CONFIG_ERR_NOT_CONFIGURED;
    spec->u.mem.name = h->descriptor.redis.address;
    return UF_CONFIG_OK;

  case UF_CONFIG_BACKEND_REDIS:
    if (!h->descriptor.redis.address) return UF_CONFIG_ERR_NOT_CONFIGURED;
    spec->u.redis.host        = h->descriptor.redis.address;
    spec->u.redis.port        = (unsigned)h->descriptor.redis.port;
    spec->u.redis.username    = h->descriptor.redis.username;
    spec->u.redis.password    = h->descriptor.redis.password;
    spec->u.redis.ns          = h->descriptor.redis.ns;
    spec->u.redis.config_name = h->descriptor.redis.config_name;
    spec->u.redis.timeout_ms  = 5000;
    return UF_CONFIG_OK;

  case UF_CONFIG_BACKEND_SQL:
    if (!h->descriptor.sql.address) return UF_CONFIG_ERR_NOT_CONFIGURED;
    spec->u.sql.dsn         = h->descriptor.sql.address;
    spec->u.sql.ns          = h->descriptor.sql.ns;
    spec->u.sql.config_name = h->descriptor.sql.config_name;
    return UF_CONFIG_OK;
  }
  return UF_CONFIG_ERR_NOT_CONFIGURED;
}

PUBLIC_API UfConfigStatus UfConfigLoad(UfConfig *h, const UfConfigLoadOptions *opt, UfConfigLoadReport *report)
{
  if (!h) return UF_CONFIG_ERR_INVALID_ARG;
  if (!h->is_described) return UF_CONFIG_ERR_NOT_CONFIGURED;

  UfConfigBackendSpec spec;
  UfConfigStatus      st = sResolveBackend(h, &spec);
  if (st != UF_CONFIG_OK) {
    ConfigSetError(st, NULL, NULL, 0, 0, "no usable origin: kind %d is not fully specified", (int)h->descriptor.kind);
    return st;
  }

  const UfConfigDriver *driver = UfConfigDriverFindKind(h->descriptor.kind);
  if (!driver) {
    ConfigSetError(UF_CONFIG_ERR_NOT_CONFIGURED, NULL, NULL, 0, 0, "no driver registered for kind %d",
                   (int)h->descriptor.kind);
    return UF_CONFIG_ERR_NOT_CONFIGURED;
  }

  void *ctx = NULL;
  st        = driver->open(&ctx, &spec);
  if (st != UF_CONFIG_OK) {
    ConfigSetError(st, NULL, driver->caps.name, 0, 0, "%s: cannot open origin: %s", driver->caps.name,
                   UfConfigStatusString(st));
    UFCONFIG_LOG_ERROR(h, "config: cannot open origin: %s", UfConfigStatusString(st));
    return st;
  }

  UfConfigFieldPair *pairs = NULL;
  size_t             n     = 0;
  UfConfigMeta       meta;
  memset(&meta, 0, sizeof(meta));
  st = driver->read_all(ctx, &pairs, &n, &meta);
  driver->close(ctx);
  if (st == UF_CONFIG_OK) h->stamp = meta.version;
  if (st != UF_CONFIG_OK) {
    /* NOT_CONFIGURED here means the store was reached and had nothing at
       this namespace — a different thing from a store that could not be
       reached, and the message has to say which. */
    ConfigSetError(st, NULL, driver->caps.name, 0, 0, "%s: nothing to read at namespace \"%s\" config \"%s\"",
                   driver->caps.name, h->descriptor.redis.ns ? h->descriptor.redis.ns : "-",
                   h->descriptor.redis.config_name ? h->descriptor.redis.config_name : "-");
    UFCONFIG_LOG_ERROR(h, "config: cannot read origin: %s", UfConfigStatusString(st));
    return st;
  }

  st = UfConfigLoadPairs(h, pairs, n, &meta, opt, report);
  UfConfigFreePairs(pairs, n);

  if (st == UF_CONFIG_OK) {
    UFCONFIG_LOG_INFO(h, "config: loaded %zu fields from kind %d", n, (int)h->descriptor.kind);
  }
  else {
    UFCONFIG_LOG_ERROR(h, "config: origin content refused: %s", UfConfigStatusString(st));
  }
  return st;
}

PUBLIC_API UfConfigStatus UfConfigLoadFile(UfConfig *          h, const char *path, const UfConfigLoadOptions *opt,
                                           UfConfigLoadReport *report)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  /* Always remember the last attempted path so Reload/Persist stay usable. */
  if (!h->source_path || strcmp(h->source_path, path) != 0) {
    free(h->source_path);
    h->source_path = strdup(path);
  }
  char *         buf = NULL;
  size_t         n   = 0;
  UfConfigStatus st  = h->backend.read_all ?
                         h->backend.read_all(h->backend.ctx, path, &buf, &n) :
                         ConfigFileReadAll(path, &buf, &n);
  if (st) return st;
  st = UfConfigLoadBuffer(h, buf, n, opt, report);
  if (st == UF_CONFIG_OK) {
    h->stamp = sFileStampPath(path);
    ConfigSha256(buf, n, h->file_sha);
    h->is_file_sha_set = 1;
  }
  free(buf);
  return st;
}

typedef struct SnapEnt
{
  char *        path;
  unsigned char dig[32];
  int           alias;
} SnapEnt;

static size_t sSnapCount(UfNode *n)
{
  if (!n) return 0;
  size_t k = 0;
  if (n->path && n->path[0] && !(n->name && strcmp(n->name, "#bindings") == 0)) k++;
  for (UfNode *c = n->first_child; c; c = c->next_sibling) k += sSnapCount(c);
  return k;
}

static void sSnapWalk(UfNode *n, SnapEnt *e, size_t *k)
{
  if (!n) return;
  if (n->path && n->path[0] && !(n->name && strcmp(n->name, "#bindings") == 0)) {
    e[*k].path = strdup(n->path);
    memcpy(e[*k].dig, n->digest, 32);
    e[*k].alias = (n->kind == UF_NODE_ALIAS);
    (*k)++;
  }
  for (UfNode *c = n->first_child; c; c = c->next_sibling) sSnapWalk(c, e, k);
}

static void sSnapFree(SnapEnt *e, size_t n)
{
  if (!e) return;
  for (size_t i = 0; i < n; i++) free(e[i].path);
  free(e);
}

PUBLIC_API UfConfigStatus UfConfigReload(UfConfig *h, UfConfigReloadReport *report)
{
  if (!h) return UF_CONFIG_ERR_INVALID_ARG;

  UfConfigBackendSpec spec;
  UfConfigStatus      rst = sResolveBackend(h, &spec);
  if (rst != UF_CONFIG_OK) return rst;

  const UfConfigDriver *driver = UfConfigDriverFindKind(h->descriptor.kind);
  if (!driver) return UF_CONFIG_ERR_NOT_CONFIGURED;

  /* Ask the store whether anything moved before reading it.  A store that
     declares a native version answers cheaply; one that does not is read. */
  if (driver->caps.native_version && driver->stat) {
    void *sctx = NULL;
    if (driver->open(&sctx, &spec) == UF_CONFIG_OK) {
      UfConfigStamp stamp;
      memset(&stamp, 0, sizeof(stamp));
      if (driver->stat(sctx, &stamp) == UF_CONFIG_OK && stamp.token != 0 && stamp.token == h->stamp) {
        driver->close(sctx);
        if (report) {
          report->count   = 0;
          report->entries = NULL;
        }
        return UF_CONFIG_NO_CHANGE;
      }
      driver->close(sctx);
    }
  }
  size_t   cold = sSnapCount(h->root);
  SnapEnt *olds = cold ? (SnapEnt*)calloc(cold, sizeof(SnapEnt)) : NULL;
  size_t   nold = 0;
  sSnapWalk(h->root, olds, &nold);
  UfConfigLoadOptions opt = h->is_load_opt_set ?
                              h->load_opt :
                              (UfConfigLoadOptions){.size = sizeof(opt), .version = 1, .mode = UF_CONFIG_LOAD_STRICT};
  UfConfigStatus st = UfConfigLoad(h, &opt, NULL);
  if (report) {
    if (h->reload_store) {
      for (size_t i = 0; i < h->reload_store_n; i++) free((char*)h->reload_store[i].path);
      free(h->reload_store);
      h->reload_store   = NULL;
      h->reload_store_n = 0;
    }
    size_t   cnew = sSnapCount(h->root);
    SnapEnt *news = cnew ? (SnapEnt*)calloc(cnew, sizeof(SnapEnt)) : NULL;
    size_t   nnew = 0;
    sSnapWalk(h->root, news, &nnew);
    size_t cap        = nold + nnew + 1;
    h->reload_store   = (UfConfigReloadEntry*)calloc(cap, sizeof(UfConfigReloadEntry));
    h->reload_store_n = 0;
    for (size_t i = 0; i < nnew; i++) {
      int found = 0;
      for (size_t j = 0; j < nold; j++) {
        if (!olds[j].path || !news[i].path || strcmp(olds[j].path, news[i].path) != 0) continue;
        found                   = 1;
        UfConfigReloadEntry *en = &h->reload_store[h->reload_store_n++];
        en->path                = strdup(news[i].path);
        memcpy(en->old_digest, olds[j].dig, 32);
        memcpy(en->new_digest, news[i].dig, 32);
        if (st != UF_CONFIG_OK) en->status = UF_CONFIG_RELOAD_REJECTED;
        else if (memcmp(olds[j].dig, news[i].dig, 32) != 0)
          en->status    = news[i].alias ? UF_CONFIG_RELOAD_CHANGED_VIA_ALIAS : UF_CONFIG_RELOAD_CHANGED;
        else en->status = UF_CONFIG_RELOAD_UNCHANGED;
        break;
      }
      if (!found && news[i].path) {
        UfConfigReloadEntry *en = &h->reload_store[h->reload_store_n++];
        en->path                = strdup(news[i].path);
        en->status              = UF_CONFIG_RELOAD_ADDED;
        memcpy(en->new_digest, news[i].dig, 32);
      }
    }
    for (size_t j = 0; j < nold; j++) {
      if (!olds[j].path) continue;
      int found = 0;
      for (size_t i = 0; i < nnew; i++) {
        if (news[i].path && strcmp(news[i].path, olds[j].path) == 0) {
          found = 1;
          break;
        }
      }
      if (!found) {
        UfConfigReloadEntry *en = &h->reload_store[h->reload_store_n++];
        en->path                = strdup(olds[j].path);
        en->status              = UF_CONFIG_RELOAD_REMOVED;
        memcpy(en->old_digest, olds[j].dig, 32);
      }
    }
    sSnapFree(news, nnew);
    report->count   = h->reload_store_n;
    report->entries = h->reload_store;
  }
  sSnapFree(olds, nold);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigPersist(UfConfig *h)
{
  if (!h) return UF_CONFIG_ERR_INVALID_ARG;
  if (!h->root) return UF_CONFIG_ERR_NOFIELD;

  UfConfigBackendSpec spec;
  UfConfigStatus      st = sResolveBackend(h, &spec);
  if (st != UF_CONFIG_OK) return st;

  const UfConfigDriver *driver = UfConfigDriverFindKind(h->descriptor.kind);
  if (!driver || !driver->write_all) return UF_CONFIG_ERR_NOT_CONFIGURED;

  /* The pair-set, not the document text.  A store receives what the contract
     says it receives; how it lays that down is the driver's business, and
     nothing on this side of the boundary assumes the text form. */
  UfConfigFieldPair *pairs = NULL;
  size_t             n     = 0;
  UfConfigMeta       meta;
  memset(&meta, 0, sizeof(meta));
  st = UfConfigFlattenHandle(h, &pairs, &n, &meta);
  if (st != UF_CONFIG_OK) return st;

  void *ctx = NULL;
  st        = driver->open(&ctx, &spec);
  if (st == UF_CONFIG_OK) {
    st = driver->write_all(ctx, pairs, n, &meta);
    /* Record where the store now stands, so a reload straight after a
       persist sees NO_CHANGE rather than reading back what it just wrote. */
    if (st == UF_CONFIG_OK && driver->caps.native_version && driver->stat) {
      UfConfigStamp stamp;
      memset(&stamp, 0, sizeof(stamp));
      if (driver->stat(ctx, &stamp) == UF_CONFIG_OK) h->stamp = stamp.token;
    }
    driver->close(ctx);
  }
  UfConfigFreePairs(pairs, n);

  if (st == UF_CONFIG_OK) {
    UFCONFIG_LOG_INFO(h, "config: wrote %zu fields to kind %d", n, (int)h->descriptor.kind);
  }
  return st;
}

PUBLIC_API UfConfigStatus UfConfigGetField(const UfConfig *h, const char *field_path, UfConfigValue *out)
{
  if (!h || !field_path || !out) return UF_CONFIG_ERR_INVALID_ARG;
  if (strlen(field_path) >= CONFIG_DEFAULT_UFCONFIG_PATH_MAX) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  if (!h->root) {
    pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
    memset(out, 0, sizeof(*out));
    out->kind    = UF_CONFIG_KIND_ABSENT;
    out->present = false;
    return UF_CONFIG_ERR_NOFIELD;
  }
  UfNode *n = ConfigResolvePath(h, field_path);
  UfConfigStatus st;
  if (!n) {
    st = UF_CONFIG_ERR_NOFIELD;
    memset(out, 0, sizeof(*out));
    out->kind    = UF_CONFIG_KIND_ABSENT;
    out->present = false;
  }
  else if (n->kind == UF_NODE_ALIAS && n->resolved) {
    st = ConfigNodeToValue(n->resolved, out);
  }
  else {
    st = ConfigNodeToValue(n, out);
  }
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigGetFieldByIndex(const UfConfig *h, int id, UfConfigValue *out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  if (id < 0 || (size_t)id >= sFieldCount(h)) return UF_CONFIG_ERR_NOFIELD;
  if (h->by_id && (int)id < h->by_id_n && h->by_id[id]) {
    pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
    UfConfigStatus st = ConfigNodeToValue(h->by_id[id], out);
    pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
    return st;
  }
  return UfConfigGetField(h, sFields(h)[id].path, out);
}

PUBLIC_API UfConfigStatus UfConfigSetInt(UfConfig *h, const char *path, int64_t v)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  if (strlen(path) >= CONFIG_DEFAULT_UFCONFIG_PATH_MAX) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || n->kind != UF_NODE_LEAF) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->desc && !n->desc->is_mutable) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_IMMUTABLE;
  }
  if (n->desc && n->desc->type == UF_FT_INT) {
    if (n->desc->has_min && v < n->desc->vmin) {
      pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, path, "min", 0, 0,
                     "%s = %lld, below min (%lld)", path, (long long)v, n->desc->vmin);
      return UF_CONFIG_ERR_VALIDATION;
    }
    if (n->desc->has_max && v > n->desc->vmax) {
      pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, path, "max", 0, 0,
                     "%s = %lld, above max (%lld)", path, (long long)v, n->desc->vmax);
      return UF_CONFIG_ERR_VALIDATION;
    }
  }
  n->v.i.raw = n->v.i.eff = v;
  n->vkind = UF_CONFIG_KIND_INT;
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigSetString(UfConfig *h, const char *path, const char *v)
{
  if (!h || !path || !v) return UF_CONFIG_ERR_INVALID_ARG;
  if (strlen(path) >= CONFIG_DEFAULT_UFCONFIG_PATH_MAX) return UF_CONFIG_ERR_LIMIT_EXCEEDED;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || n->kind != UF_NODE_LEAF) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->desc && !n->desc->is_mutable) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_IMMUTABLE;
  }
  n->v.s.raw          = ConfigArenaStrndup(&h->arena, v, strlen(v));
  n->v.s.raw_len      = strlen(v);
  n->v.s.eff          = n->v.s.raw;
  n->v.s.eff_len      = n->v.s.raw_len;
  n->vkind          = UF_CONFIG_KIND_STRING;
  UfConfigStatus vs = sApplyStringField(h, n, n->desc);
  if (vs) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return vs;
  }
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigSetFloat(UfConfig *h, const char *path, double v)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || n->kind != UF_NODE_LEAF) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->desc && !n->desc->is_mutable) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_IMMUTABLE;
  }
  n->v.f.raw = n->v.f.eff = v;
  n->vkind = UF_CONFIG_KIND_FLOAT;
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigSetBool(UfConfig *h, const char *path, bool v)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || n->kind != UF_NODE_LEAF) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->desc && !n->desc->is_mutable) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_IMMUTABLE;
  }
  n->v.b.raw = n->v.b.eff = v;
  n->vkind = UF_CONFIG_KIND_BOOL;
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigDigest(const UfConfig *h, unsigned char out[32])
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  memcpy(out, h->digest, 32);
  return UF_CONFIG_OK;
}

PUBLIC_API uint64_t UfConfigGeneration(const UfConfig *h)
{
  return h ? atomic_load(&h->generation) : 0;
}

PUBLIC_API UfConfigStatus UfConfigSetBytes(UfConfig *h, const char *path, const uint8_t *p, size_t n)
{
  if (!h || !path || !p) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *nd = ConfigResolvePath(h, path);
  if (!nd || nd->kind != UF_NODE_LEAF) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  nd->v.s.raw     = ConfigArenaStrndup(&h->arena, (const char*)p, n);
  nd->v.s.raw_len = n;
  nd->v.s.eff     = nd->v.s.raw;
  nd->v.s.eff_len = n;
  nd->vkind     = UF_CONFIG_KIND_BYTES;
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigUnset(UfConfig *h, const char *path)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || !n->parent) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  int indexed = sAnyUnlisted(h, n);
  UfNode **pp = &n->parent->first_child;
  while (*pp && *pp != n) pp = &(*pp)->next_sibling;
  if (*pp) *pp = n->next_sibling;
  atomic_fetch_add(&h->generation, 1);
  sRebuildIndex(h);
  if (indexed) sRebuildUnlisted(h);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigArrayClear(UfConfig *h, const char *path)
{
  if (!h || !path) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n || (n->kind != UF_NODE_ARRAY && n->kind != UF_NODE_SCOPE)) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_TYPE_MISMATCH;
  }
  UfNode *leaving = n->first_child;
  n->first_child  = NULL;
  n->array_count  = 0;
  n->kind         = UF_NODE_ARRAY;
  if (sAnyUnlisted(h, leaving)) sRebuildUnlisted(h);
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigArrayAppendString(UfConfig *h, const char *path, const char *v)
{
  if (!h || !path || !v) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
  UfNode *n = ConfigResolvePath(h, path);
  if (!n) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->kind == UF_NODE_SCOPE) n->kind = UF_NODE_ARRAY;
  if (n->kind != UF_NODE_ARRAY) {
    pthread_rwlock_unlock(&h->lock);
    pthread_mutex_unlock(&h->build_lock);
    return UF_CONFIG_ERR_TYPE_MISMATCH;
  }
  char name[32];
  snprintf(name, sizeof(name), "%zu", n->array_count);
  UfNode *c = (UfNode*)ConfigArenaAlloc(&h->arena, sizeof(UfNode));
  memset(c, 0, sizeof(*c));
  c->kind      = UF_NODE_LEAF;
  c->vkind     = UF_CONFIG_KIND_STRING;
  c->name      = ConfigArenaStrndup(&h->arena, name, strlen(name));
  c->v.s.raw     = ConfigArenaStrndup(&h->arena, v, strlen(v));
  c->v.s.raw_len = c->v.s.eff_len = strlen(v);
  c->v.s.eff     = c->v.s.raw;
  c->parent    = n;
  if (!n->first_child) n->first_child = c;
  else {
    UfNode *x = n->first_child;
    while (x->next_sibling) x = x->next_sibling;
    x->next_sibling = c;
  }
  n->array_count++;
  /* No index rebuild: the appended child gets a name but never a path --
     sAssignPaths runs only at load -- so sIsUnlisted can never accept it and
     the index cannot go stale by its addition. */
  atomic_fetch_add(&h->generation, 1);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API void UfConfigSetBackend(UfConfig *h, UfConfigStatus (*read_all)(void *, const char *, char **, size_t *),
                                   UfConfigStatus (*write_all)(void *, const char *, const char *, size_t), void *ctx)
{
  if (!h) return;
  if (read_all) h->backend.read_all = read_all;
  if (write_all) h->backend.write_all = write_all;
  h->backend.ctx = ctx;
}

PUBLIC_API uint64_t UfConfigPin(UfConfig *h)
{
  if (!h) return 0;
  atomic_fetch_add(&h->readers, 1);
  return atomic_load(&h->generation);
}

PUBLIC_API void UfConfigUnpin(UfConfig *h)
{
  if (!h) return;
  int left = atomic_fetch_sub(&h->readers, 1) - 1;
  if (left <= 0) {
    atomic_store(&h->readers, 0);
    pthread_mutex_lock(&h->build_lock);
  pthread_rwlock_wrlock(&h->lock);
    ConfigArenaFree(h->retired);
    h->retired = NULL;
    pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  }
}
