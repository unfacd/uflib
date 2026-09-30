/**
 * @file ufconfig_util.c
 * @brief Shared foundations: the arena, the error state, the status strings, and the
 *        small helpers the rest of the module is written on.
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
#include <stdarg.h>

static _Thread_local UfConfigError g_err;

/*!
 * Where the last error's field path is copied to.
 *
 * The path handed to @ref ConfigSetError is normally a pointer into the parse
 * arena, and the failure path frees that arena before returning -- so storing
 * the pointer left the error record describing why a configuration was refused
 * by pointing at memory that had been released.  That is the first thing a
 * caller reads when it reports the refusal, and it is read after the free, so
 * it is the documented error path that was broken rather than an edge of it.
 * Thread-local for the same reason @c g_err is.
 */
static _Thread_local char g_err_path[CONFIG_DEFAULT_UFCONFIG_PATH_MAX];

void ConfigClearError(void) { memset(&g_err, 0, sizeof(g_err)); }

void ConfigSetError(UfConfigStatus st, const char *path, const char *validator, int line, int col, const char *fmt, ...)
{
  g_err.status = st;
  if (path) {
    snprintf(g_err_path, sizeof(g_err_path), "%s", path);
    g_err.field_path = g_err_path;
  }
  else {
    g_err.field_path = NULL;
  }
  /* @c validator is always a literal or a driver's static name, so it needs no
     copy of its own.  The message is already formatted into the record. */
  g_err.validator  = validator;
  g_err.line       = line;
  g_err.column     = col;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err.message, sizeof(g_err.message), fmt, ap);
  va_end(ap);
}

PUBLIC_API const UfConfigError *UfConfigLastError(void) { return &g_err; }

PUBLIC_API const char *UfConfigStatusString(UfConfigStatus s)
{
  switch (s) {
  case UF_CONFIG_OK: return "OK";
  case UF_CONFIG_NO_CHANGE: return "NO_CHANGE";
  case UF_CONFIG_ERR_NOFIELD: return "NOFIELD";
  case UF_CONFIG_ERR_TYPE_MISMATCH: return "TYPE_MISMATCH";
  case UF_CONFIG_ERR_REQUIRED: return "REQUIRED";
  case UF_CONFIG_ERR_VALIDATION: return "VALIDATION";
  case UF_CONFIG_ERR_IMMUTABLE: return "IMMUTABLE";
  case UF_CONFIG_ERR_UNKNOWN_FIELD: return "UNKNOWN_FIELD";
  case UF_CONFIG_ERR_PARSE: return "PARSE";
  case UF_CONFIG_ERR_BACKEND: return "BACKEND";
  case UF_CONFIG_ERR_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
  case UF_CONFIG_ERR_SUBST_UNRESOLVED: return "SUBST_UNRESOLVED";
  case UF_CONFIG_ERR_DIGEST_MISMATCH: return "DIGEST_MISMATCH";
  case UF_CONFIG_ERR_NO_MEMORY: return "NO_MEMORY";
  case UF_CONFIG_ERR_UNIMPLEMENTED: return "UNIMPLEMENTED";
  case UF_CONFIG_ERR_INVALID_ARG: return "INVALID_ARG";
  case UF_CONFIG_ERR_CYCLE: return "CYCLE";
  case UF_CONFIG_ERR_ALIAS_UNRESOLVED: return "ALIAS_UNRESOLVED";
  case UF_CONFIG_ERR_NOT_A_TABLE: return "NOT_A_TABLE";
  case UF_CONFIG_ERR_ALIAS_DEPTH_EXCEEDED: return "ALIAS_DEPTH_EXCEEDED";
  case UF_CONFIG_ERR_NOT_CONFIGURED: return "NOT_CONFIGURED";
  case UF_CONFIG_ERR_STORE_VERSION: return "STORE_VERSION";
  case UF_CONFIG_ERR_NOT_ATOMIC: return "NOT_ATOMIC";
  case UF_CONFIG_ERR_SECRET: return "SECRET";
  default: return "?";
  }
}

void *ConfigArenaAlloc(UfArena **ap, size_t n)
{
  if (n == 0) n = 1;
  n          = (n + 7u) & ~7u;
  UfArena *a = *ap;
  if (!a || a->used + n > a->cap) {
    size_t cap = 65536;
    while (cap < n + 64) cap *= 2;
    UfArena *b = (UfArena*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->buf = (char*)calloc(1, cap);
    if (!b->buf) {
      free(b);
      return NULL;
    }
    b->cap  = cap;
    b->next = a;
    *ap     = b;
    a       = b;
  }
  void *p = a->buf + a->used;
  a->used += n;
  return p;
}

char *ConfigArenaStrndup(UfArena **a, const char *s, size_t n)
{
  char *p = (char*)ConfigArenaAlloc(a, n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

void ConfigArenaFree(UfArena *a)
{
  while (a) {
    UfArena *n = a->next;
    free(a->buf);
    free(a);
    a = n;
  }
}

int ConfigAsciiIsDigit(int c) { return c >= '0' && c <= '9'; }
int ConfigAsciiIsAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int ConfigAsciiToUpper(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int ConfigAsciiToLower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static uint32_t sRotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sSha256Block(uint32_t h[8], const unsigned char block[64])
{
  static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2
  };
  uint32_t w[64];
  for (int t = 0; t < 16; t++)
    w[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16) | ((uint32_t)block[t * 4 + 2] << 8) |
      block[t * 4 + 3];
  for (int t = 16; t < 64; t++) {
    uint32_t s0 = sRotr(w[t - 15], 7) ^ sRotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    uint32_t s1 = sRotr(w[t - 2], 17) ^ sRotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t]        = w[t - 16] + s0 + w[t - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int t = 0; t < 64; t++) {
    uint32_t S1  = sRotr(e, 6) ^ sRotr(e, 11) ^ sRotr(e, 25);
    uint32_t ch  = (e & f) ^ ((~e) & g);
    uint32_t t1  = hh + S1 + ch + K[t] + w[t];
    uint32_t S0  = sRotr(a, 2) ^ sRotr(a, 13) ^ sRotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2  = S0 + maj;
    hh           = g;
    g            = f;
    f            = e;
    e            = d + t1;
    d            = c;
    c            = b;
    b            = a;
    a            = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

UfConfigStatus ConfigSha256(const void *data, size_t len, unsigned char out[32])
{
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const unsigned char *p = (const unsigned char*)data;
  unsigned char block[64];
  uint64_t bitlen = (uint64_t)len * 8ull;
  size_t i = 0;
  while (i + 64 <= len) {
    memcpy(block, p + i, 64);
    sSha256Block(h, block);
    i += 64;
  }
  size_t rem = len - i;
  memcpy(block, p + i, rem);
  block[rem] = 0x80;
  if (rem >= 56) {
    memset(block + rem + 1, 0, 63 - rem);
    sSha256Block(h, block);
    memset(block, 0, 64);
  }
  else {
    memset(block + rem + 1, 0, 63 - rem);
  }
  for (int t = 0; t < 8; t++) block[56 + t] = (unsigned char)(bitlen >> (56 - 8 * t));
  sSha256Block(h, block);
  for (int t = 0; t < 8; t++) {
    out[t * 4]     = (unsigned char)(h[t] >> 24);
    out[t * 4 + 1] = (unsigned char)(h[t] >> 16);
    out[t * 4 + 2] = (unsigned char)(h[t] >> 8);
    out[t * 4 + 3] = (unsigned char)(h[t]);
  }
  return UF_CONFIG_OK;
}
