/**
 * @file ws_frame.c
 * @brief RFC 6455 frame codec: self-contained, allocation-free, direction-agnostic.
 */
#include "ws_frame_priv.h"
#include "ws_utf8_priv.h"

#include <string.h>

static int
sIsControlOpcode(WsOpcode opcode)
{
  return opcode == WS_OPCODE_CLOSE || opcode == WS_OPCODE_PING || opcode == WS_OPCODE_PONG;
}

static int
sIsKnownOpcode(unsigned opcode)
{
  switch (opcode) {
  case WS_OPCODE_CONTINUATION:
  case WS_OPCODE_TEXT:
  case WS_OPCODE_BINARY:
  case WS_OPCODE_CLOSE:
  case WS_OPCODE_PING:
  case WS_OPCODE_PONG:
    return 1;
  default:
    return 0;
  }
}

static int
sIsValidCloseCode(unsigned code)
{
  if (code >= 3000u && code <= 4999u) return 1;

  switch (code) {
  case 1000: /* normal closure */
  case 1001: /* going away */
  case 1002: /* protocol error */
  case 1003: /* unsupported data */
  case 1007: /* invalid payload data */
  case 1008: /* policy violation */
  case 1009: /* message too big */
  case 1010: /* mandatory extension missing */
  case 1011: /* internal error */
  case 1012: /* service restart */
  case 1013: /* try again later */
  case 1014: /* bad gateway */
    return 1;
  default:
    return 0;
  }
}

static size_t
sHeaderSize(size_t payload_len)
{
  if (payload_len < WS_LENGTH_16BIT) return WS_HEADER_LEN_SMALL;
  if (payload_len <= 0xffffu) return WS_HEADER_LEN_MEDIUM;
  return WS_HEADER_LEN_LARGE;
}

ssize_t
WsFrameEncode(unsigned char *out, size_t cap, const unsigned char *payload, size_t len,
              WsOpcode opcode, int fin, const unsigned char mask[4])
{
  size_t header = sHeaderSize(len);
  size_t mask_len = mask ? WS_MASK_KEY_LEN : 0u;
  size_t total;
  size_t i;

  if (!out) return -1;
  if (len && !payload) return -1;
  if (!sIsKnownOpcode((unsigned)opcode)) return -1;

  if (sIsControlOpcode(opcode)) {
    if (!fin) return -1;
    if (len > WS_CONTROL_PAYLOAD_MAX) return -1;
  }

  if (len > SIZE_MAX - header - mask_len) return -1;
  total = header + mask_len + len;
  if (cap < total) return -1;

  out[0] = (unsigned char)((fin ? 0x80u : 0x00u) | ((unsigned)opcode & 0x0fu));
  if (header == WS_HEADER_LEN_SMALL) {
    out[1] = (unsigned char)((mask ? 0x80u : 0x00u) | (unsigned char)len);
  }
  else if (header == WS_HEADER_LEN_MEDIUM) {
    out[1] = (unsigned char)((mask ? 0x80u : 0x00u) | WS_LENGTH_16BIT);
    out[2] = (unsigned char)((len >> 8) & 0xffu);
    out[3] = (unsigned char)(len & 0xffu);
  }
  else {
    out[1] = (unsigned char)((mask ? 0x80u : 0x00u) | WS_LENGTH_64BIT);
    for (i = 0; i < 8u; i++) out[2u + i] = (unsigned char)((len >> (8u * (7u - i))) & 0xffu);
  }

  if (mask) memcpy(out + header, mask, WS_MASK_KEY_LEN);
  for (i = 0; i < len; i++) {
    out[header + mask_len + i] = mask ? (unsigned char)(payload[i] ^ mask[i % 4u]) : payload[i];
  }

  return (ssize_t)total;
}

int
WsFrameDecode(const unsigned char *in, size_t in_len, unsigned int flags,
              unsigned char *payload_out, size_t payload_cap,
              WsFrameInfo *info, size_t *consumed)
{
  size_t   header_len;
  size_t   len7;
  uint64_t payload_len;
  size_t   mask_len = 0u;
  size_t   total;
  size_t   i;
  int      fin;
  int      masked;
  unsigned opcode;

  if (!in || !info || !consumed) return WS_FRAME_ERR_ARGUMENT;
  if (flags & ~(WS_FRAME_REQUIRE_MASKED | WS_FRAME_REQUIRE_UNMASKED)) return WS_FRAME_ERR_ARGUMENT;
  if ((flags & WS_FRAME_REQUIRE_MASKED) && (flags & WS_FRAME_REQUIRE_UNMASKED)) return WS_FRAME_ERR_ARGUMENT;

  if (in_len < 2u) return WS_FRAME_NEED_MORE;

  fin    = (in[0] & 0x80u) != 0;
  masked = (in[1] & 0x80u) != 0;
  opcode = in[0] & 0x0fu;

  /* RSV1 is the bit a compression extension would claim; it is refused with a
   * code of its own so a future extension can change only this branch. */
  if (in[0] & 0x40u) return WS_FRAME_ERR_RESERVED;
  if (in[0] & 0x30u) return WS_FRAME_ERR_PROTOCOL; /* RSV2/RSV3 stay protocol errors */
  if (!sIsKnownOpcode(opcode)) return WS_FRAME_ERR_PROTOCOL;

  len7 = in[1] & 0x7fu;
  if (len7 < WS_LENGTH_16BIT) {
    header_len  = WS_HEADER_LEN_SMALL;
    payload_len = len7;
  }
  else if (len7 == WS_LENGTH_16BIT) {
    if (in_len < 4u) return WS_FRAME_NEED_MORE;
    header_len  = WS_HEADER_LEN_MEDIUM;
    payload_len = ((uint64_t)in[2] << 8) | (uint64_t)in[3];
  }
  else {
    if (in_len < 10u) return WS_FRAME_NEED_MORE;
    header_len  = WS_HEADER_LEN_LARGE;
    payload_len = 0;
    for (i = 0; i < 8u; i++) payload_len = (payload_len << 8) | (uint64_t)in[2u + i];
    if (in[2] & 0x80u) return WS_FRAME_ERR_PROTOCOL; /* RFC 6455 §5.2: MSB must be 0 */
    if (payload_len > (uint64_t)SIZE_MAX) return WS_FRAME_ERR_PROTOCOL;
  }

  if (sIsControlOpcode((WsOpcode)opcode)) {
    if (!fin) return WS_FRAME_ERR_PROTOCOL;
    if (payload_len > WS_CONTROL_PAYLOAD_MAX) return WS_FRAME_ERR_PROTOCOL;
  }

  if (masked && (flags & WS_FRAME_REQUIRE_UNMASKED)) return WS_FRAME_ERR_PROTOCOL;
  if (!masked && (flags & WS_FRAME_REQUIRE_MASKED)) return WS_FRAME_ERR_PROTOCOL;

  if (masked) {
    mask_len = WS_MASK_KEY_LEN;
    if (in_len < header_len + mask_len) return WS_FRAME_NEED_MORE;
  }

  if (payload_len > (uint64_t)SIZE_MAX - header_len - mask_len) return WS_FRAME_ERR_PROTOCOL;
  /* Refuse an oversized frame before asking for more bytes: a peer that
   * declares a huge length must not make the caller buffer unboundedly. The
   * descriptor is filled so the caller can size a buffer and retry. */
  if (payload_len > (uint64_t)payload_cap) {
    info->opcode      = (WsOpcode)opcode;
    info->fin         = fin;
    info->masked      = masked;
    info->rsv1        = 0;
    info->header_len  = header_len;
    info->payload_len = (size_t)payload_len;
    return WS_FRAME_ERR_CAPACITY;
  }
  if (payload_len && !payload_out) return WS_FRAME_ERR_ARGUMENT;

  total = header_len + mask_len + (size_t)payload_len;
  if (in_len < total) return WS_FRAME_NEED_MORE;

  if (masked) {
    const unsigned char *mask = in + header_len;

    for (i = 0; i < (size_t)payload_len; i++) {
      payload_out[i] = (unsigned char)(in[header_len + mask_len + i] ^ mask[i % 4u]);
    }
  }
  else if (payload_len) {
    memcpy(payload_out, in + header_len, (size_t)payload_len);
  }

  info->opcode      = (WsOpcode)opcode;
  info->fin         = fin;
  info->masked      = masked;
  info->rsv1        = 0;
  info->header_len  = header_len;
  info->payload_len = (size_t)payload_len;
  *consumed         = total;

  return WS_FRAME_DECODED;
}

ssize_t
WsFrameControlReply(const WsFrameInfo *info, const unsigned char *payload,
                    unsigned char *out, size_t cap, const unsigned char mask[4])
{
  unsigned char close_payload[WS_CONTROL_PAYLOAD_MAX];
  unsigned short code       = 0;
  size_t         reason_len = 0;
  ssize_t        reply_len;

  if (!info || !out) return -1;
  if (info->payload_len && !payload) return -1;

  switch (info->opcode) {
  case WS_OPCODE_PING:
    return WsFrameEncode(out, cap, payload, info->payload_len, WS_OPCODE_PONG, 1, mask);

  case WS_OPCODE_CLOSE:
    if (WsClosePayloadDecode(payload, info->payload_len, &code, NULL, 0, &reason_len) < 0) {
      /* malformed close: answer with a protocol-error close */
      reply_len = WsClosePayloadEncode(close_payload, sizeof close_payload, 1002, NULL);
      if (reply_len < 0) return -1;
      return WsFrameEncode(out, cap, close_payload, (size_t)reply_len, WS_OPCODE_CLOSE, 1, mask);
    }

    /* echo the received status code and reason verbatim */
    if (info->payload_len > sizeof close_payload) return -1;
    if (info->payload_len) memcpy(close_payload, payload, info->payload_len);
    return WsFrameEncode(out, cap, info->payload_len ? close_payload : NULL,
                         info->payload_len, WS_OPCODE_CLOSE, 1, mask);

  case WS_OPCODE_PONG:
  case WS_OPCODE_TEXT:
  case WS_OPCODE_BINARY:
  case WS_OPCODE_CONTINUATION:
  default:
    return 0;
  }
}

ssize_t
WsClosePayloadEncode(unsigned char *out, size_t cap, unsigned short code, const char *reason)
{
  size_t reason_len = reason ? strlen(reason) : 0u;

  if (!out) return -1;
  if (!sIsValidCloseCode(code)) return -1;
  if (reason_len > WS_CLOSE_REASON_MAX) return -1;
  if (cap < 2u + reason_len) return -1;

  out[0] = (unsigned char)((code >> 8) & 0xffu);
  out[1] = (unsigned char)(code & 0xffu);
  if (reason_len) memcpy(out + 2u, reason, reason_len);

  return (ssize_t)(2u + reason_len);
}

int
WsClosePayloadDecode(const unsigned char *payload, size_t len, unsigned short *code,
                     char *reason_out, size_t reason_cap, size_t *reason_len)
{
  size_t text_len;

  if (!code || !reason_len) return -1;
  if (len && !payload) return -1;
  if (len == 1u) return -1;

  if (len == 0u) {
    *code       = 1005;
    *reason_len = 0;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return 0;
  }

  *code = (unsigned short)(((unsigned)payload[0] << 8) | (unsigned)payload[1]);
  if (!sIsValidCloseCode(*code)) return -1;

  text_len    = len - 2u;
  {
    WsUtf8Validator validator;

    ws_utf8_reset(&validator);
    if (!ws_utf8_validate(&validator, payload + 2u, text_len, 1)) return -1;
  }
  *reason_len = text_len;
  if (reason_out && reason_cap) {
    size_t copy = text_len < reason_cap - 1u ? text_len : reason_cap - 1u;

    memcpy(reason_out, payload + 2u, copy);
    reason_out[copy] = '\0';
  }

  return 0;
}
