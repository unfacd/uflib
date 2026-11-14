/**
 * @file ws_close.h
 * @brief The RFC 6455 closing handshake, as explicit state.
 *
 * The codec builds frames; this module enforces the closing rules an endpoint
 * must not get wrong: one CLOSE each way, no data after a close, and the close
 * code an error maps to.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_CLOSE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_CLOSE_H

#include <uflib/uflib_defs.h>
#include <uflib/msg_framing/websocket/ws_close_type.h>
#include <uflib/msg_framing/websocket/ws_frame_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the handshake to "open". NULL is a no-op. */
PUBLIC_API void WsCloseHandshakeInit(WsCloseHandshake *handshake);

/**
 * @brief Record that this endpoint is sending (or has sent) a CLOSE.
 *
 * @return WS_CLOSE_ACTION_NONE on the first close, WS_CLOSE_ACTION_CLOSED when a
 *         close has already been sent — in which case no second frame must be
 *         emitted.
 */
PUBLIC_API int WsCloseHandshakeOnSent(WsCloseHandshake *handshake, unsigned short code);

/**
 * @brief Classify a frame in the light of the closing handshake.
 *
 * @return WS_CLOSE_ACTION_NONE    a data frame while open (or any control frame)
 *         WS_CLOSE_ACTION_REPLY   a first CLOSE from the peer: answer it, then close
 *         WS_CLOSE_ACTION_CLOSED  a duplicate CLOSE, or a CLOSE after we closed:
 *                                 close the transport without another frame
 *         WS_CLOSE_ACTION_INVALID data after either side closed
 */
PUBLIC_API int WsCloseHandshakeOnReceive(WsCloseHandshake *handshake, const WsFrameInfo *frame);

/** @brief May the application still send data frames? */
PUBLIC_API int WsCloseHandshakeCanSendData(const WsCloseHandshake *handshake);

/**
 * @brief Map a frame or message codec result to the close code an endpoint sends.
 *
 * -1 (protocol) → 1002, -2 (capacity/size) → 1009, -3 (internal misuse) → 1011,
 * -4 (encoding) → 1007, anything else → 1000 (normal).
 */
PUBLIC_API unsigned short WsCloseCodeForError(int result);

#ifdef __cplusplus
}
#endif

#endif
