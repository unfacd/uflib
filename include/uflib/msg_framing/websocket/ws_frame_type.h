/**
 * @file ws_frame_type.h
 * @brief Public types for the RFC 6455 frame codec.
 *
 * Opcodes, decode outcomes, masking policy and the descriptor a decoded frame
 * is reported through. The implementation keeps its limits and helpers in
 * src/msg_framing/websocket/ws_frame_priv.h.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_TYPE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_TYPE_H

#include <sys/types.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Worst-case bytes a frame adds on top of its payload: a 10-byte header plus a
 * 4-byte masking key. A destination of payload_len + WS_FRAME_MAX_OVERHEAD always
 * fits; consumers need no other sizing helper.
 */
#define WS_FRAME_MAX_OVERHEAD 14u

/** Frame opcodes (RFC 6455 §5.2). */
typedef enum WsOpcode
{
  WS_OPCODE_CONTINUATION = 0x0,  ///< continuation of a fragmented message
  WS_OPCODE_TEXT         = 0x1,  ///< UTF-8 text message
  WS_OPCODE_BINARY       = 0x2,  ///< binary message
  WS_OPCODE_CLOSE        = 0x8,  ///< connection close
  WS_OPCODE_PING         = 0x9,  ///< ping
  WS_OPCODE_PONG         = 0xA,  ///< pong
} WsOpcode;

/** A frame was read. */
#define WS_FRAME_DECODED      1
/** Not enough bytes yet: the caller reads more and calls again. */
#define WS_FRAME_NEED_MORE    0
/** Wire violation: reserved bits, bad opcode, illegal control frame, bad length. */
#define WS_FRAME_ERR_PROTOCOL (-1)
/** The frame is complete but payload_cap cannot hold the payload. */
#define WS_FRAME_ERR_CAPACITY (-2)
/** Invalid argument: NULL buffer, bad flags, NULL descriptor. */
#define WS_FRAME_ERR_ARGUMENT (-3)
/** RSV1 is set: legal only with a negotiated extension, which this codec has none of. */
#define WS_FRAME_ERR_RESERVED (-5)

/** Require that the incoming frame is masked (server side). */
#define WS_FRAME_REQUIRE_MASKED   0x01u
/** Require that the incoming frame is unmasked (client side). */
#define WS_FRAME_REQUIRE_UNMASKED 0x02u
/** Accept either masking direction. */
#define WS_FRAME_ACCEPT_ANY       0x00u

/** What one decoded frame carries. */
typedef struct WsFrameInfo
{
  WsOpcode opcode;       ///< opcode of this frame
  int      fin;          ///< non-zero when this frame ends the message
  int      masked;       ///< the frame arrived masked
  int      rsv1;         ///< RSV1 bit as received (always 0: the codec refuses it)
  size_t   header_len;   ///< 2, 4 or 10
  size_t   payload_len;  ///< payload bytes delivered to payload_out
} WsFrameInfo;

#ifdef __cplusplus
}
#endif

#endif
