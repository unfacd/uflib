/**
 * @file ws_utf8_type.h
 * @brief Incremental UTF-8 validator state (RFC 3629, as required by RFC 6455 §8.1).
 *
 * A text message is validated as it is assembled: a multi-byte sequence may be
 * split across continuation frames, so the validator carries its partial state
 * from one frame to the next.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_UTF8_TYPE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_UTF8_TYPE_H

#include <stddef.h>

#include <uflib/uflib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Validator state. Zero-initialise, or call the reset helper. */
typedef struct WsUtf8Validator
{
  unsigned need;       ///< continuation bytes still expected
  unsigned codepoint;  ///< code point accumulated so far
  unsigned minimum;    ///< shortest legal encoding for the current sequence
  int      valid;      ///< cleared as soon as a violation is seen
} WsUtf8Validator;

#ifdef __cplusplus
}
#endif

#endif
