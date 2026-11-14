/**
 * @file ws_handshake.c
 * @brief RFC 6455 opening-handshake primitives.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <uflib/msg_framing/websocket/ws_handshake.h>

#include <stdlib.h>
#include <string.h>

#include <uflib/utils_base64.h>
#include <uflib/utils_crypto.h>

#define WS_HANDSHAKE_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_STATUS_LINE_MAX 64u

static int
sEqualsIgnoreCase(const char *a, const char *b, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++) {
    char ca = a[i];
    char cb = b[i];

    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return 1;
}

/* Find a header by case-insensitive name and copy its trimmed value. */
static int
sFindHeader(const char *response, size_t len, const char *name, char *value, size_t value_cap)
{
  size_t name_len = strlen(name);
  size_t pos      = 0;
  size_t end;

  /* skip the status line */
  while (pos + 1 < len && !(response[pos] == '\r' && response[pos + 1] == '\n')) pos++;
  if (pos + 1 >= len) return -1;
  pos += 2;

  while (pos < len) {
    size_t line_start = pos;
    size_t line_end;
    size_t value_start;
    size_t value_end;

    while (pos + 1 < len && !(response[pos] == '\r' && response[pos + 1] == '\n')) pos++;
    line_end = pos;
    if (line_end == line_start) return -1; /* empty line: end of headers */

    if (line_end - line_start > name_len && response[line_start + name_len] == ':' &&
        sEqualsIgnoreCase(response + line_start, name, name_len)) {
      value_start = line_start + name_len + 1;
      while (value_start < line_end && (response[value_start] == ' ' || response[value_start] == '\t')) {
        value_start++;
      }
      value_end = line_end;
      while (value_end > value_start &&
             (response[value_end - 1] == ' ' || response[value_end - 1] == '\t')) {
        value_end--;
      }
      if (value_end - value_start >= value_cap) return -1;
      memcpy(value, response + value_start, value_end - value_start);
      value[value_end - value_start] = '\0';
      return 0;
    }

    pos = line_end + 2;
  }

  return -1;
}

/* Is `token` one of the comma-separated tokens in `value`, case-insensitively? */
static int
sHasToken(const char *value, const char *token)
{
  size_t token_len = strlen(token);
  const char *p    = value;
  size_t i;

  while (*p) {
    const char *start;
    const char *end;

    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    start = p;
    while (*p && *p != ',') p++;
    end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    if ((size_t)(end - start) == token_len && sEqualsIgnoreCase(start, token, token_len)) return 1;
    for (i = 0; i < 1; i++) {} /* keep the loop shape uniform */
  }

  return 0;
}

static int
sParseStatus(const char *response, size_t len, int *status)
{
  size_t i = 0;
  char   code[4];

  if (len < 12) return -1; /* "HTTP/1.1 101" is the shortest useful line */
  if (strncmp(response, "HTTP/1.", 7) != 0) return -1;
  if (response[8] != ' ') return -1;

  i = 9;
  if (i + 3 > len) return -1;
  code[0] = response[i];
  code[1] = response[i + 1];
  code[2] = response[i + 2];
  code[3] = '\0';

  if (code[0] < '0' || code[0] > '9' || code[1] < '0' || code[1] > '9' || code[2] < '0' ||
      code[2] > '9') {
    return -1;
  }
  if (i + 3 < len && response[i + 3] != ' ' && response[i + 3] != '\r') return -1;

  *status = atoi(code);
  return 0;
}

int
WsHandshakeKey(char out_b64[25])
{
  unsigned char raw[16];
  unsigned char encoded[25];

  if (!out_b64) return -1;
  if (GenerateSecureRandom(raw, sizeof raw) < 0) return -1;
  if (!base64_encode(raw, (int)sizeof raw, encoded)) return -1;

  memcpy(out_b64, encoded, 24);
  out_b64[24] = '\0';
  return 0;
}

int
WsHandshakeAcceptKey(const char *key, char out_b64[29])
{
  char   buffer[128];
  size_t key_len;

  if (!key || !out_b64) return -1;

  key_len = strlen(key);
  if (key_len + sizeof WS_HANDSHAKE_GUID - 1u >= sizeof buffer) return -1;

  memcpy(buffer, key, key_len);
  memcpy(buffer + key_len, WS_HANDSHAKE_GUID, sizeof WS_HANDSHAKE_GUID - 1u);
  buffer[key_len + sizeof WS_HANDSHAKE_GUID - 1u] = '\0';

  if (ComputeSHA1((const unsigned char *)buffer, key_len + sizeof WS_HANDSHAKE_GUID - 1u, out_b64,
                  29, 1) < 0) {
    return -1;
  }

  return 0;
}

int
WsHandshakeValidateReply(const char *response, size_t len, const char *key,
                         const char *const *offered_protocols, WsHandshakeReply *reply)
{
  char   accept[64];
  char   expected[29];
  char   upgrade[64];
  char   connection[128];
  char   protocols[64];
  char   extensions[64];
  int    status = 0;
  size_t i;

  if (!response || !key || len == 0) return WS_HANDSHAKE_ERR_ARGUMENT;

  if (sParseStatus(response, len, &status) < 0) return WS_HANDSHAKE_ERR_ARGUMENT;

  if (status == 426) return WS_HANDSHAKE_ERR_VERSION;
  if (status != 101) return WS_HANDSHAKE_ERR_STATUS;

  if (sFindHeader(response, len, "Upgrade", upgrade, sizeof upgrade) < 0) {
    return WS_HANDSHAKE_ERR_UPGRADE;
  }
  if (!sHasToken(upgrade, "websocket")) return WS_HANDSHAKE_ERR_UPGRADE;

  if (sFindHeader(response, len, "Connection", connection, sizeof connection) < 0) {
    return WS_HANDSHAKE_ERR_UPGRADE;
  }
  if (!sHasToken(connection, "upgrade")) return WS_HANDSHAKE_ERR_UPGRADE;

  if (sFindHeader(response, len, "Sec-WebSocket-Accept", accept, sizeof accept) < 0) {
    return WS_HANDSHAKE_ERR_ACCEPT;
  }
  if (WsHandshakeAcceptKey(key, expected) < 0) return WS_HANDSHAKE_ERR_ARGUMENT;
  if (strcmp(accept, expected) != 0) return WS_HANDSHAKE_ERR_ACCEPT;

  if (sFindHeader(response, len, "Sec-WebSocket-Extensions", extensions, sizeof extensions) == 0 &&
      extensions[0] != '\0') {
    return WS_HANDSHAKE_ERR_EXTENSION;
  }

  if (sFindHeader(response, len, "Sec-WebSocket-Protocol", protocols, sizeof protocols) == 0 &&
      protocols[0] != '\0') {
    int offered = 0;

    for (i = 0; offered_protocols && offered_protocols[i]; i++) {
      if (strcmp(protocols, offered_protocols[i]) == 0) {
        offered = 1;
        break;
      }
    }
    if (!offered) return WS_HANDSHAKE_ERR_PROTOCOL;
  } else {
    protocols[0] = '\0';
  }

  if (reply) {
    memset(reply, 0, sizeof *reply);
    reply->status = status;
    memcpy(reply->accept, accept, sizeof reply->accept - 1u);
    reply->accept[sizeof reply->accept - 1u] = '\0';
    memcpy(reply->protocol, protocols, sizeof reply->protocol - 1u);
    reply->protocol[sizeof reply->protocol - 1u] = '\0';
  }

  return WS_HANDSHAKE_OK;
}
