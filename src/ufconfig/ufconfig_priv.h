/**
 * @file ufconfig_priv.h
 * @brief Internal entry points of the ufconfig module.
 *
 * Not installed, and not reachable from a consumer — @c src/ is on the
 * library's PRIVATE include path only, and the install rule copies @c include/
 * and nothing else.
 *
 * The internal types live in @ref ufconfig_type_priv.h and the module's
 * compile-time bounds in @ref ufconfig_defs_priv.h; this header carries the
 * functions that move data between them.  Every one of them is internal: a
 * consumer reaches this module only through @ref ufconfig.h.
 *
 * Naming follows linkage, and the two are not interchangeable:
 *
 *   @c UfConfig*   the public contract.  Installed, annotated @c PUBLIC_API,
 *                  stable for consumers.
 *   @c Config*     internal, external linkage.  Reached from more than one
 *                  translation unit, or from a test — so it is declared here
 *                  rather than made @c static.  The @c Config prefix keeps it
 *                  out of the module's public namespace.
 *   @c s*          internal, @c static, one translation unit.  Declared and
 *                  defined in the same @c .c file and never here.
 *
 * Nothing in this header is @c s-prefixed: a name in a header has external
 * linkage by definition, so the @c s prefix would be a false statement about
 * it.  A function that is only ever used in the file that defines it belongs
 * @c static in that file, with the prefix, and does not appear below.
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

#ifndef UFLIB_UFCONFIG_UFCONFIG_PRIV_H
#define UFLIB_UFCONFIG_UFCONFIG_PRIV_H

#include "ufconfig_type_priv.h"

/* Lifecycle.  ConfigCreate is the raw allocator: it produces a handle with no
   schema, which can parse and serialise but cannot validate.  A consumer never
   reaches it -- UfConfigCreate is the only constructor they see, and it always
   binds a schema.  This exists for the internal paths that are schema-free by
   design, such as UfConfigPairsFromLua. */
UfConfigStatus ConfigCreate(UfConfig **out);

/* Arena. */
void *ConfigArenaAlloc(UfArena **a, size_t n);
char *ConfigArenaStrndup(UfArena **a, const char *s, size_t n);
void  ConfigArenaFree(UfArena *a);

/* Error reporting, per thread. */
void ConfigSetError(UfConfigStatus st, const char *path, const char *validator, int line, int col, const char *fmt,
                    ...);
void ConfigClearError(void);

/* Pipeline stages, in the order a load runs them. */
UfConfigStatus ConfigParseLuaStyle(UfConfig *h, const char *src, size_t len, UfNode **out_root);
UfConfigStatus ConfigResolveAliases(UfConfig *h, UfNode *root);
UfConfigStatus ConfigApplySchema(UfConfig *h, UfNode *root, UfConfigLoadMode mode, UfConfigLoadReport *report);
UfConfigStatus ConfigCanonAndDigest(UfNode *n, unsigned char out[32]);

/* Emitters. */
UfConfigStatus ConfigEmitJson(const UfNode *n, char *buf, size_t cap, size_t *needed, bool pretty, bool expand);
UfConfigStatus ConfigEmitYaml(const UfNode *n, char *buf, size_t cap, size_t *needed, bool pretty);
UfConfigStatus ConfigEmitIni(const UfNode *n, char *buf, size_t cap, size_t *needed);
UfConfigStatus ConfigEmitLua(const UfNode *n, char *buf, size_t cap, size_t *needed);

/* Tree access. */
UfNode *       ConfigFindPath(UfNode *root, const char *path);
UfNode *       ConfigResolvePath(const UfConfig *h, const char *path);
void           ConfigRebuildIndexes(UfConfig *h);
UfConfigStatus ConfigNodeToValue(const UfNode *n, UfConfigValue *out);

/* Character classification, locale-independent. */
int ConfigAsciiIsDigit(int c);
int ConfigAsciiIsAlpha(int c);
int ConfigAsciiToUpper(int c);
int ConfigAsciiToLower(int c);

/* Hashing and file reads. */
UfConfigStatus ConfigSha256(const void *data, size_t len, unsigned char out[32]);
UfConfigStatus ConfigFileReadAll(const char *path, char **out, size_t *len);

/* Transforms and their inverses. */
UfConfigStatus ConfigTransformApplyNamed(UfArena **arena, const char *op, const char *in, size_t inlen, char **out,
                                         size_t *  outlen);
UfConfigStatus ConfigTransformRevertNamed(UfArena **arena, const char *op, const char *in, size_t inlen, char **out,
                                          size_t *  outlen);

/* Validators. */
UfConfigStatus ConfigValidateString(const struct UfConfigFieldDesc *d, const char *s, size_t n, int line, int col);
UfConfigStatus ConfigValidateCidr(const char *s);
UfConfigStatus ConfigValidateFileSize(const char *s);
UfConfigStatus ConfigValidateIp4(const char *s);
UfConfigStatus ConfigValidateIp6(const char *s);
UfConfigStatus ConfigValidateEmail(const char *s);
UfConfigStatus ConfigValidateUrl(const char *s);
UfConfigStatus ConfigValidateFqdn(const char *s);
UfConfigStatus ConfigValidateDate(const char *s);

/* The superseded write-through backend defaults, still wired at create time. */
UfConfigStatus ConfigFileReadAllDefault(void *ctx, const char *path, char **out, size_t *len);
UfConfigStatus ConfigFileWriteAllDefault(void *ctx, const char *path, const char *buf, size_t len);

#endif /* UFLIB_UFCONFIG_UFCONFIG_PRIV_H */
