/**
 * @file ufconfig_validate.c
 * @brief The validators: what a field's value must satisfy beyond its type.
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
#include <string.h>

#include <regex.h>
UfConfigStatus ConfigValidateIp4(const char *s);
UfConfigStatus ConfigValidateIp6(const char *s);

UfConfigStatus ConfigValidateCidr(const char *s)
{
  if (!s) return UF_CONFIG_ERR_VALIDATION;
  const char *slash = strrchr(s, '/');
  if (!slash || slash == s || !slash[1]) return UF_CONFIG_ERR_VALIDATION;
  char   ip[64];
  size_t ipl = (size_t)(slash - s);
  if (ipl >= sizeof(ip)) return UF_CONFIG_ERR_VALIDATION;
  memcpy(ip, s, ipl);
  ip[ipl]  = 0;
  int bits = 0;
  for (const char *p = slash + 1; *p; p++) {
    if (!ConfigAsciiIsDigit((unsigned char)*p)) return UF_CONFIG_ERR_VALIDATION;
    bits = bits * 10 + (*p - '0');
  }
  if (strchr(ip, ':')) {
    if (bits > 128) return UF_CONFIG_ERR_VALIDATION;
    return ConfigValidateIp6(ip);
  }
  if (bits > 32) return UF_CONFIG_ERR_VALIDATION;
  return ConfigValidateIp4(ip);
}

static int sIsLeap(int y)
{
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

UfConfigStatus ConfigValidateDate(const char *s)
{
  if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return UF_CONFIG_ERR_VALIDATION;
  int y = 0, m = 0, d = 0;
  for (int i = 0; i < 4; i++) {
    if (!ConfigAsciiIsDigit((unsigned char)s[i])) return UF_CONFIG_ERR_VALIDATION;
    y = y * 10 + (s[i] - '0');
  }
  if (!ConfigAsciiIsDigit((unsigned char)s[5]) || !ConfigAsciiIsDigit((unsigned char)s[6]) || !
    ConfigAsciiIsDigit((unsigned char)s[8]) || !ConfigAsciiIsDigit((unsigned char)s[9]))
    return UF_CONFIG_ERR_VALIDATION;
  m = (s[5] - '0') * 10 + (s[6] - '0');
  d = (s[8] - '0') * 10 + (s[9] - '0');
  if (m < 1 || m > 12 || d < 1) return UF_CONFIG_ERR_VALIDATION;
  static const int md[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int              maxd = md[m];
  if (m == 2 && sIsLeap(y)) maxd = 29;
  if (d > maxd) return UF_CONFIG_ERR_VALIDATION;
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateIp4(const char *s)
{
  int         oct = 0, val = 0, digits = 0, lead0 = 0;
  const char *p   = s;
  if (!p || !*p) return UF_CONFIG_ERR_VALIDATION;
  while (*p) {
    if (ConfigAsciiIsDigit((unsigned char)*p)) {
      if (digits == 0 && *p == '0' && ConfigAsciiIsDigit((unsigned char)p[1])) return UF_CONFIG_ERR_VALIDATION;
      /* leading zero */
      val = val * 10 + (*p - '0');
      digits++;
      if (val > 255 || digits > 3) return UF_CONFIG_ERR_VALIDATION;
      (void)lead0;
    }
    else if (*p == '.') {
      if (digits == 0) return UF_CONFIG_ERR_VALIDATION;
      oct++;
      val    = 0;
      digits = 0;
      if (oct > 3) return UF_CONFIG_ERR_VALIDATION;
    }
    else return UF_CONFIG_ERR_VALIDATION;
    p++;
  }
  if (digits == 0 || oct != 3) return UF_CONFIG_ERR_VALIDATION;
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateIp6(const char *s)
{
  if (!s || !*s) return UF_CONFIG_ERR_VALIDATION;
  int         compact = 0, groups = 0, hex = 0;
  const char *p       = s;
  if (*p == ':' && p[1] != ':') return UF_CONFIG_ERR_VALIDATION;
  while (*p) {
    if (*p == ':') {
      if (p[1] == ':') {
        if (compact) return UF_CONFIG_ERR_VALIDATION;
        compact = 1;
        p       += 2;
        if (*p == ':') return UF_CONFIG_ERR_VALIDATION;
        if (hex) {
          groups++;
          hex = 0;
        }
        continue;
      }
      if (hex == 0 && !compact) return UF_CONFIG_ERR_VALIDATION;
      groups++;
      hex = 0;
      p++;
      continue;
    }
    int  hv = -1;
    char c  = *p;
    if (c >= '0' && c <= '9') hv = 1;
    else if (c >= 'a' && c <= 'f') hv = 1;
    else if (c >= 'A' && c <= 'F') hv = 1;
    if (hv < 0) return UF_CONFIG_ERR_VALIDATION;
    hex++;
    if (hex > 4) return UF_CONFIG_ERR_VALIDATION;
    p++;
  }
  if (hex) groups++;
  if (compact) {
    if (groups > 7) return UF_CONFIG_ERR_VALIDATION;
  }
  else if (groups != 8) return UF_CONFIG_ERR_VALIDATION;
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateEmail(const char *s)
{
  if (!s || !*s) return UF_CONFIG_ERR_VALIDATION;
  const char *at = strchr(s, '@');
  if (!at || at == s || !at[1] || strchr(at + 1, '@')) return UF_CONFIG_ERR_VALIDATION;
  if (at[1] == '@') return UF_CONFIG_ERR_VALIDATION;
  for (const char *p = s; *p; p++) if (*p == ' ' || *p == '\t') return UF_CONFIG_ERR_VALIDATION;
  const char *dot = strrchr(at, '.');
  if (!dot || dot < at + 2 || !dot[1]) return UF_CONFIG_ERR_VALIDATION;
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateUrl(const char *s)
{
  if (!s) return UF_CONFIG_ERR_VALIDATION;
  const char *sep = strstr(s, "://");
  if (!sep || sep == s) return UF_CONFIG_ERR_VALIDATION;
  const char *host = sep + 3;
  if (*host == '/' || *host == '\0') return UF_CONFIG_ERR_VALIDATION; /* relative / scheme-relative */
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateFqdn(const char *s)
{
  if (!s || !*s) return UF_CONFIG_ERR_VALIDATION;
  size_t n = strlen(s);
  if (n > 253) return UF_CONFIG_ERR_VALIDATION;
  /* RFC 1123: a label begins and ends with a letter or a digit, and may carry
     hyphens only between them.  The end-of-label test was missing entirely, so
     both "-leading.host" and "trailing-.host" were accepted; the start-of-label
     test was missing too, and the entry for this defect claimed the leading
     case was already refused.  It was not. */
  size_t lab  = 0;
  char   prev = 0; /* last character of the label being read */
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '_') return UF_CONFIG_ERR_VALIDATION;
    if (c == '.') {
      if (lab == 0) return UF_CONFIG_ERR_VALIDATION;
      if (!ConfigAsciiIsAlpha((unsigned char)prev) && !ConfigAsciiIsDigit((unsigned char)prev)) {
        return UF_CONFIG_ERR_VALIDATION;
      }
      lab  = 0;
      prev = 0;
      continue;
    }
    int alpha = ConfigAsciiIsAlpha((unsigned char)c);
    int digit = ConfigAsciiIsDigit((unsigned char)c);
    if (!alpha && !digit && c != '-') return UF_CONFIG_ERR_VALIDATION;
    if (lab == 0 && !alpha && !digit) return UF_CONFIG_ERR_VALIDATION;
    lab++;
    if (lab > 63) return UF_CONFIG_ERR_VALIDATION;
    prev = c;
  }
  /* The last label ends at the end of the string rather than at a dot, so the
     same test has to run once more after the loop. */
  if (lab == 0) return UF_CONFIG_ERR_VALIDATION;
  if (!ConfigAsciiIsAlpha((unsigned char)prev) && !ConfigAsciiIsDigit((unsigned char)prev)) {
    return UF_CONFIG_ERR_VALIDATION;
  }
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigValidateFileSize(const char *s)
{
  if (!s || !*s) return UF_CONFIG_ERR_VALIDATION;
  const char *p = s;
  if (!ConfigAsciiIsDigit((unsigned char)*p)) return UF_CONFIG_ERR_VALIDATION;
  while (ConfigAsciiIsDigit((unsigned char)*p)) p++;
  /* A unit is required.  This returned OK for a bare number, which the
     validator table in the design document says must be refused -- "no unit,
     unknown unit" -- so the code disagreed with its own contract, and a size of
     "10" would have been read as whatever the consumer's unit happened to be. */
  if (!*p) return UF_CONFIG_ERR_VALIDATION;
  char   u[8];
  size_t i = 0;
  while (*p && i + 1 < sizeof(u)) {
    char c = *p++;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    u[i++] = c;
  }
  u[i] = 0;
  /* A unit too long for the buffer leaves input behind; say so rather than
     falling through to the comparison with a truncated unit. */
  if (*p) return UF_CONFIG_ERR_VALIDATION;
  if (!strcmp(u, "b") || !strcmp(u, "kb") || !strcmp(u, "mb") || !strcmp(u, "gb") || !strcmp(u, "kib") || !
    strcmp(u, "mib") || !strcmp(u, "gib")) return UF_CONFIG_OK;
  return UF_CONFIG_ERR_VALIDATION;
}

UfConfigStatus ConfigValidateString(const struct UfConfigFieldDesc *d, const char *s, size_t n, int line, int col)
{
  /* Every message below names the value, so a caller diagnosing a rejected
     configuration does not have to re-read the document to find out what was
     refused.  Truncated: a value may be a megabyte, and the message lives in a
     512-byte buffer.

     Except when the field is a secret.  By the time a validator runs, `encrypted`
     fields have already been decrypted, so the bytes here are the plaintext —
     and this buffer is copied into the message that UfConfigLastError() hands to
     any caller and that every harness prints.  Naming the rule that rejected the
     value is the point; naming the value would put the secret in a log. */
  char vshow[65];
  if (d && d->is_encrypted) {
    snprintf(vshow, sizeof(vshow), "<redacted>");
  }
  else {
    size_t vcpy = n < 64 ? n : 64;
    memcpy(vshow, s, vcpy);
    vshow[vcpy] = 0;
    if (n > 64) memcpy(vshow + 61, "...", 4);
  }

  if (!d || !s) return UF_CONFIG_OK;
  if (d->one_of) {
    int         ok = 0;
    const char *p  = d->one_of;
    while (*p) {
      const char *c = strchr(p, ',');
      size_t      L = c ? (size_t)(c - p) : strlen(p);
      if (L == n && memcmp(p, s, n) == 0) {
        ok = 1;
        break;
      }
      p = c ? c + 1 : p + L;
      if (!c) break;
    }
    if (!ok) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "one_of", line, col,
                     "%s = '%s' not in one_of", d->path, vshow);
      return UF_CONFIG_ERR_VALIDATION;
    }
  }
  if (d->fmt) {
    UfConfigStatus st = UF_CONFIG_OK;
    if (!strcmp(d->fmt, "date")) st = ConfigValidateDate(s);
    else if (!strcmp(d->fmt, "url")) st = ConfigValidateUrl(s);
    else if (!strcmp(d->fmt, "email")) st = ConfigValidateEmail(s);
    else if (!strcmp(d->fmt, "fqdn") || !strcmp(d->fmt, "fqd")) st = ConfigValidateFqdn(s);
    else if (!strcmp(d->fmt, "ip4")) st = ConfigValidateIp4(s);
    else if (!strcmp(d->fmt, "ip6")) st = ConfigValidateIp6(s);
    else if (!strcmp(d->fmt, "file_size")) st = ConfigValidateFileSize(s);
    else if (!strcmp(d->fmt, "cidr") || !strcmp(d->fmt, "netmask.cidr")) st = ConfigValidateCidr(s);
    if (st) {
      ConfigSetError(st, d->path, d->fmt, line, col, "%s = '%s' failed %s", d->path,
                     vshow, d->fmt);
      return st;
    }
  }
  if (d->has_len_min && (long long)n < d->len_min) {
    ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "length", line, col,
                   "%s: %zu characters, shorter than length.min (%lld)", d->path, n,
                   d->len_min);
    return UF_CONFIG_ERR_VALIDATION;
  }
  if (d->has_len_max && (long long)n > d->len_max) {
    ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "length", line, col,
                   "%s: %zu characters, longer than length.max (%lld)", d->path, n,
                   d->len_max);
    return UF_CONFIG_ERR_VALIDATION;
  }
  if (d->regex && d->regex[0]) {
    regex_t re;
    if (regcomp(&re, d->regex, REG_EXTENDED | REG_NOSUB) != 0) return UF_CONFIG_ERR_VALIDATION;
    char tmp[n + 1];
    memcpy(tmp, s, n);
    tmp[n] = 0;
    int rc = regexec(&re, tmp, 0, NULL, 0);
    regfree(&re);
    if (rc != 0) {
      ConfigSetError(UF_CONFIG_ERR_VALIDATION, d->path, "regex", line, col,
                     "%s = '%s' failed regex '%s'", d->path, vshow, d->regex);
      return UF_CONFIG_ERR_VALIDATION;
    }
  }
  return UF_CONFIG_OK;
}
