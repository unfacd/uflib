/**
 * @file ufconfig_transform.c
 * @brief The transform catalogue: what a declared transform does to a value, and how it
 *        is reversed.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

/* --- hex --- */
static int sHexval(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static UfConfigStatus sHexEnc(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  static const char *H = "0123456789abcdef";
  char *             o = (char*)ConfigArenaAlloc(a, n * 2 + 1);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  for (size_t i = 0; i < n; i++) {
    o[i * 2]     = H[(unsigned char)in[i] >> 4];
    o[i * 2 + 1] = H[(unsigned char)in[i] & 0xF];
  }
  o[n * 2] = 0;
  *out     = o;
  *olen    = n * 2;
  return UF_CONFIG_OK;
}

static UfConfigStatus sHexDec(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  if (n % 2) {
    ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "hex", 0, 0, "odd hex length");
    return UF_CONFIG_ERR_VALIDATION;
  }
  char *o = (char*)ConfigArenaAlloc(a, n / 2 + 1);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  for (size_t i = 0; i < n; i += 2) {
    int hi = sHexval((unsigned char)in[i]), lo = sHexval((unsigned char)in[i + 1]);
    if (hi < 0 || lo < 0) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "hex", 0, 0, "non-hex digit");
      return UF_CONFIG_ERR_VALIDATION;
    }
    o[i / 2] = (char)((hi << 4) | lo);
  }
  o[n / 2] = 0;
  *out     = o;
  *olen    = n / 2;
  return UF_CONFIG_OK;
}

static const char B64[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64U[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char B32[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int sB64val(char c, int url)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (!url && c == '+') return 62;
  if (!url && c == '/') return 63;
  if (url && c == '-') return 62;
  if (url && c == '_') return 63;
  return -1;
}

static UfConfigStatus sB64Enc(UfArena **a, const char *in, size_t n, char **out, size_t *olen, int url)
{
  const char *A      = url ? B64U : B64;
  size_t      groups = (n + 2) / 3;
  size_t      cap    = groups * 4 + 1;
  char *      o      = (char*)ConfigArenaAlloc(a, cap);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  size_t j = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = ((unsigned char)in[i]) << 16;
    if (i + 1 < n) v |= ((unsigned char)in[i + 1]) << 8;
    if (i + 2 < n) v |= (unsigned char)in[i + 2];
    o[j++] = A[(v >> 18) & 63];
    o[j++] = A[(v >> 12) & 63];
    if (i + 1 < n) o[j++] = A[(v >> 6) & 63];
    else if (!url) o[j++] = '=';
    if (i + 2 < n) o[j++] = A[v & 63];
    else if (!url) o[j++] = '=';
  }
  o[j]  = 0;
  *out  = o;
  *olen = j;
  return UF_CONFIG_OK;
}

static UfConfigStatus sB64Dec(UfArena **a, const char *in, size_t n, char **out, size_t *olen, int url)
{
  /* strip padding */
  while (n && in[n - 1] == '=') n--;
  if (!url) {
    /* reject non-canonical: remainder 1 is illegal */
  }
  for (size_t i = 0; i < n; i++) {
    if (sB64val(in[i], url) < 0) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "base64", 0, 0, "bad alphabet");
      return UF_CONFIG_ERR_VALIDATION;
    }
  }
  if (n % 4 == 1) {
    ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "base64", 0, 0, "trailing garbage");
    return UF_CONFIG_ERR_VALIDATION;
  }
  size_t outn = n * 3 / 4;
  char * o    = (char*)ConfigArenaAlloc(a, outn + 1);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  size_t j = 0;
  for (size_t i = 0; i < n; i += 4) {
    int      a0 = sB64val(in[i], url);
    int      a1 = i + 1 < n ? sB64val(in[i + 1], url) : 0;
    int      a2 = i + 2 < n ? sB64val(in[i + 2], url) : 0;
    int      a3 = i + 3 < n ? sB64val(in[i + 3], url) : 0;
    unsigned v  = ((unsigned)a0 << 18) | ((unsigned)a1 << 12) | ((unsigned)a2 << 6) | (unsigned)a3;
    if (j < outn) o[j++] = (char)((v >> 16) & 0xFF);
    if (i + 2 < n && j < outn) o[j++] = (char)((v >> 8) & 0xFF);
    if (i + 3 < n && j < outn) o[j++] = (char)(v & 0xFF);
  }
  o[j]  = 0;
  *out  = o;
  *olen = j;
  return UF_CONFIG_OK;
}

static int sB32val(char c)
{
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= '2' && c <= '7') return c - '2' + 26;
  return -1;
}

static UfConfigStatus sB32Enc(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  size_t cap = ((n + 4) / 5) * 8 + 1;
  char * o   = (char*)ConfigArenaAlloc(a, cap);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  size_t j = 0, i = 0;
  while (i < n) {
    unsigned char buf[5] = {0};
    size_t        k      = n - i > 5 ? 5 : n - i;
    memcpy(buf, in + i, k);
    o[j++] = B32[buf[0] >> 3];
    o[j++] = B32[((buf[0] & 7) << 2) | (buf[1] >> 6)];
    o[j++] = (k > 1) ? B32[(buf[1] >> 1) & 31] : '=';
    o[j++] = (k > 1) ? B32[((buf[1] & 1) << 4) | (buf[2] >> 4)] : '=';
    o[j++] = (k > 2) ? B32[((buf[2] & 15) << 1) | (buf[3] >> 7)] : '=';
    o[j++] = (k > 3) ? B32[(buf[3] >> 2) & 31] : '=';
    o[j++] = (k > 3) ? B32[((buf[3] & 3) << 3) | (buf[4] >> 5)] : '=';
    o[j++] = (k > 4) ? B32[buf[4] & 31] : '=';
    i      += 5;
  }
  o[j]  = 0;
  *out  = o;
  *olen = j;
  return UF_CONFIG_OK;
}

static UfConfigStatus sB32Dec(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  while (n && in[n - 1] == '=') n--;
  for (size_t i = 0; i < n; i++) {
    if (sB32val(in[i]) < 0) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "base32", 0, 0, "bad alphabet");
      return UF_CONFIG_ERR_VALIDATION;
    }
  }
  size_t outn = n * 5 / 8;
  char * o    = (char*)ConfigArenaAlloc(a, outn + 1);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  size_t   j    = 0;
  unsigned acc  = 0;
  int      bits = 0;
  for (size_t i = 0; i < n; i++) {
    acc  = (acc << 5) | (unsigned)sB32val(in[i]);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (j < outn) o[j++] = (char)((acc >> bits) & 0xFF);
    }
  }
  o[j]  = 0;
  *out  = o;
  *olen = j;
  return UF_CONFIG_OK;
}

/* LZF with back-references for runs / repeats (still valid LZF). */
static UfConfigStatus sLzfEnc(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  size_t         cap  = n + n / 8 + 64;
  unsigned char *outb = (unsigned char*)ConfigArenaAlloc(a, cap ? cap : 1);
  if (!outb) return UF_CONFIG_ERR_NO_MEMORY;
  const unsigned char *ip     = (const unsigned char*)in;
  const unsigned char *in_end = ip + n;
  unsigned char *      op     = outb;
  const unsigned char *anchor = ip;
  unsigned short       htab[1 << 14];
  memset(htab, 0, sizeof(htab));
  while (ip + 2 < in_end && (size_t)(op - outb) + 8 < cap) {
    unsigned             h   = (((unsigned)ip[0] << 8) ^ ((unsigned)ip[1] << 4) ^ ip[2]) & ((1u << 14) - 1);
    const unsigned char *ref = (const unsigned char*)in + htab[h];
    htab[h]                  = (unsigned short)(ip - (const unsigned char*)in);
    if (ref >= (const unsigned char*)in && ref + 2 < ip && (ip - ref) <= 8192 && ref[0] == ip[0] && ref[1] == ip[1] &&
      ref[2] == ip[2]) {
      size_t lit = (size_t)(ip - anchor);
      while (lit) {
        size_t chunk = lit > 32 ? 32 : lit;
        *op++        = (unsigned char)(chunk - 1);
        memcpy(op, anchor, chunk);
        op     += chunk;
        anchor += chunk;
        lit    -= chunk;
      }
      size_t len = 3;
      while (ip + len < in_end && ref[len] == ip[len] && len < 264) len++;
      unsigned off   = (unsigned)(ip - ref - 1);
      size_t   lcode = len - 2;
      if (lcode < 7) *op++ = (unsigned char)((off >> 8) + (lcode << 5));
      else {
        *op++ = (unsigned char)((off >> 8) + (7 << 5));
        *op++ = (unsigned char)(lcode - 7);
      }
      *op++  = (unsigned char)off;
      ip     += len;
      anchor = ip;
      continue;
    }
    ip++;
  }
  {
    size_t lit = (size_t)(in_end - anchor);
    while (lit) {
      size_t chunk = lit > 32 ? 32 : lit;
      *op++        = (unsigned char)(chunk - 1);
      memcpy(op, anchor, chunk);
      op     += chunk;
      anchor += chunk;
      lit    -= chunk;
    }
  }
  *out  = (char*)outb;
  *olen = (size_t)(op - outb);
  return UF_CONFIG_OK;
}

static UfConfigStatus sLzfDec(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  if (n == 0) {
    char *o = (char*)ConfigArenaAlloc(a, 1);
    if (!o) return UF_CONFIG_ERR_NO_MEMORY;
    o[0]  = 0;
    *out  = o;
    *olen = 0;
    return UF_CONFIG_OK;
  }
  size_t         cap  = n * 4 + 64;
  unsigned char *outb = (unsigned char*)ConfigArenaAlloc(a, cap);
  if (!outb) return UF_CONFIG_ERR_NO_MEMORY;
  const unsigned char *ip     = (const unsigned char*)in;
  const unsigned char *ip_end = ip + n;
  unsigned char *      op     = outb;
  unsigned char *      op_end = outb + cap;
  while (ip < ip_end) {
    unsigned ctrl = *ip++;
    if (ctrl < 32) {
      size_t lit = ctrl + 1;
      if (ip + lit > ip_end || op + lit > op_end) {
        ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "lzf", 0, 0, "bad lzf literal");
        return UF_CONFIG_ERR_VALIDATION;
      }
      memcpy(op, ip, lit);
      op += lit;
      ip += lit;
    }
    else {
      size_t   len = ctrl >> 5;
      unsigned off = (ctrl & 31) << 8;
      if (len == 7) {
        if (ip >= ip_end) return UF_CONFIG_ERR_VALIDATION;
        len += *ip++;
      }
      if (ip >= ip_end) return UF_CONFIG_ERR_VALIDATION;
      off                += *ip++;
      len                += 2;
      unsigned char *ref = op - off - 1;
      if (ref < outb || op + len > op_end) {
        ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, "lzf", 0, 0, "bad lzf backref");
        return UF_CONFIG_ERR_VALIDATION;
      }
      while (len--) *op++ = *ref++;
    }
  }
  *out  = (char*)outb;
  *olen = (size_t)(op - outb);
  return UF_CONFIG_OK;
}

static UfConfigStatus sAsciiCase(UfArena **a, const char *in, size_t n, char **out, size_t *olen, int up)
{
  char *o = (char*)ConfigArenaAlloc(a, n + 1);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  for (size_t i = 0; i < n; i++)
    o[i] = (char)(up ? ConfigAsciiToUpper((unsigned char)in[i]) : ConfigAsciiToLower((unsigned char)in[i]));
  o[n]  = 0;
  *out  = o;
  *olen = n;
  return UF_CONFIG_OK;
}

static UfConfigStatus sVarsub(UfArena **a, const char *in, size_t n, char **out, size_t *olen)
{
  char *o = (char*)ConfigArenaAlloc(a, n * 4 + 64);
  if (!o) return UF_CONFIG_ERR_NO_MEMORY;
  size_t j = 0;
  for (size_t i = 0; i < n;) {
    if (in[i] == '\\' && i + 1 < n && in[i + 1] == '$' && i + 2 < n && in[i + 2] == '$') {
      o[j++] = '$';
      o[j++] = '$';
      i      += 3;
      continue;
    }
    if (in[i] == '\\' && i + 1 == n) {
      ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, "varsub", 0, 0, "trailing backslash");
      return UF_CONFIG_ERR_PARSE;
    }
    if (in[i] == '$' && i + 1 < n && in[i + 1] == '$') {
      i += 2;
      char   name[64];
      size_t nl = 0;
      if (i < n && in[i] == '{') {
        i++;
        while (i < n && in[i] != '}' && nl + 1 < sizeof(name)) name[nl++] = in[i++];
        if (i < n && in[i] == '}') i++;
      }
      else {
        while (i < n && (ConfigAsciiIsAlpha((unsigned char)in[i]) || ConfigAsciiIsDigit((unsigned char)in[i]) || in[i]
          == '_') && nl + 1 < sizeof(name)) name[nl++] = in[i++];
      }
      name[nl]        = 0;
      const char *val = getenv(name);
      if (!val) {
        ConfigSetError(UF_CONFIG_ERR_SUBST_UNRESOLVED, name, "varsub", 0, 0, "unresolved $$%s", name);
        return UF_CONFIG_ERR_SUBST_UNRESOLVED;
      }
      size_t vl = strlen(val);
      memcpy(o + j, val, vl);
      j += vl;
      continue;
    }
    o[j++] = in[i++];
  }
  o[j]  = 0;
  *out  = o;
  *olen = j;
  return UF_CONFIG_OK;
}

static UfConfigStatus sRegexSub(UfArena **a, const char *in, size_t n, char **out, size_t *olen, const char *spec)
{
  /* spec: regex(pattern,repl) or regex/pattern/repl/ */
  const char *pat = NULL, *repl = "";
  char        pbuf[256],   rbuf[256];
  pbuf[0] = rbuf[0] = 0;
  if (spec && !strncmp(spec, "regex", 5)) {
    const char *s = spec + 5;
    if (*s == '(') {
      s++;
      const char *comma = strchr(s, ',');
      const char *end   = strrchr(s, ')');
      if (comma && end && comma < end) {
        size_t pl = (size_t)(comma - s);
        if (pl > 255) pl = 255;
        memcpy(pbuf, s, pl);
        pbuf[pl]  = 0;
        size_t rl = (size_t)(end - comma - 1);
        if (rl > 255) rl = 255;
        memcpy(rbuf, comma + 1, rl);
        rbuf[rl] = 0;
        pat      = pbuf;
        repl     = rbuf;
      }
    }
    else if (*s == '/' || *s == ',') {
      char        sep = *s++;
      const char *mid = strchr(s, sep);
      if (mid) {
        size_t pl = (size_t)(mid - s);
        if (pl > 255) pl = 255;
        memcpy(pbuf, s, pl);
        pbuf[pl] = 0;
        snprintf(rbuf, sizeof(rbuf), "%s", mid + 1);
        size_t L = strlen(rbuf);
        if (L && rbuf[L - 1] == sep) rbuf[L - 1] = 0;
        pat  = pbuf;
        repl = rbuf;
      }
    }
  }
  if (!pat || !pat[0]) {
    char *o = ConfigArenaStrndup(a, in, n);
    if (!o) return UF_CONFIG_ERR_NO_MEMORY;
    *out  = o;
    *olen = n;
    return UF_CONFIG_OK;
  }
  regex_t re;
  if (regcomp(&re, pat, REG_EXTENDED) != 0) return UF_CONFIG_ERR_VALIDATION;
  char tmp[n + 1];
  memcpy(tmp, in, n);
  tmp[n] = 0;
  regmatch_t m;
  if (regexec(&re, tmp, 1, &m, 0) != 0) {
    regfree(&re);
    char *o = ConfigArenaStrndup(a, in, n);
    *out    = o;
    *olen   = n;
    return UF_CONFIG_OK;
  }
  size_t pre  = (size_t)m.rm_so;
  size_t post = n - (size_t)m.rm_eo;
  size_t rl   = strlen(repl);
  size_t outn = pre + rl + post;
  char * o    = (char*)ConfigArenaAlloc(a, outn + 1);
  memcpy(o, in, pre);
  memcpy(o + pre, repl, rl);
  memcpy(o + pre + rl, in + m.rm_eo, post);
  o[outn] = 0;
  *out    = o;
  *olen   = outn;
  regfree(&re);
  return UF_CONFIG_OK;
}

static const char *sBaseOp(const char *op)
{
  if (!op) return "";
  if (!strncmp(op, "compress", 8)) return "compress";
  if (!strncmp(op, "regex", 5)) return "regex";
  return op;
}

UfConfigStatus ConfigTransformApplyNamed(UfArena **arena, const char *op, const char *in, size_t inlen, char **out,
                                         size_t *  outlen)
{
  const char *b = sBaseOp(op);
  if (!strcmp(b, "hex")) return sHexEnc(arena, in, inlen, out, outlen);
  if (!strcmp(b, "base64")) return sB64Enc(arena, in, inlen, out, outlen, 0);
  if (!strcmp(b, "base64url")) return sB64Enc(arena, in, inlen, out, outlen, 1);
  if (!strcmp(b, "base32")) return sB32Enc(arena, in, inlen, out, outlen);
  if (!strcmp(b, "toupper")) return sAsciiCase(arena, in, inlen, out, outlen, 1);
  if (!strcmp(b, "tolower")) return sAsciiCase(arena, in, inlen, out, outlen, 0);
  if (!strcmp(b, "varsub")) return sVarsub(arena, in, inlen, out, outlen);
  if (!strcmp(b, "compress")) return sLzfEnc(arena, in, inlen, out, outlen);
  if (!strcmp(b, "regex")) return sRegexSub(arena, in, inlen, out, outlen, op);
  ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, op, 0, 0, "unknown transform %s", op);
  return UF_CONFIG_ERR_VALIDATION;
}

UfConfigStatus ConfigTransformRevertNamed(UfArena **arena, const char *op, const char *in, size_t inlen, char **out,
                                          size_t *  outlen)
{
  const char *b = sBaseOp(op);
  if (!strcmp(b, "hex")) return sHexDec(arena, in, inlen, out, outlen);
  if (!strcmp(b, "base64")) return sB64Dec(arena, in, inlen, out, outlen, 0);
  if (!strcmp(b, "base64url")) return sB64Dec(arena, in, inlen, out, outlen, 1);
  if (!strcmp(b, "base32")) return sB32Dec(arena, in, inlen, out, outlen);
  if (!strcmp(b, "compress")) return sLzfDec(arena, in, inlen, out, outlen);
  /* lossy / identity revert */
  if (!strcmp(b, "toupper") || !strcmp(b, "tolower") || !strcmp(b, "varsub") || !strcmp(b, "regex")) {
    char *o = ConfigArenaStrndup(arena, in, inlen);
    if (!o) return UF_CONFIG_ERR_NO_MEMORY;
    *out    = o;
    *outlen = inlen;
    return UF_CONFIG_OK;
  }
  ConfigSetError(UF_CONFIG_ERR_VALIDATION, NULL, op, 0, 0, "unknown transform %s", op);
  return UF_CONFIG_ERR_VALIDATION;
}
