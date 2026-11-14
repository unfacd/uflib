/**
 * @file ws_close_type.h
 * @brief Public types for the RFC 6455 closing handshake.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_CLOSE_TYPE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_CLOSE_TYPE_H

#include <uflib/uflib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The frame may be processed: the connection is open. */
#define WS_CLOSE_ACTION_NONE       0
/** Build and send a CLOSE reply (WsFrameControlReply), then close the transport. */
#define WS_CLOSE_ACTION_REPLY      1
/** No reply is owed: close the transport. */
#define WS_CLOSE_ACTION_CLOSED     2
/** Illegal state: data after a close, or a second close frame. Close the connection. */
#define WS_CLOSE_ACTION_INVALID (-1)

/** Closing-handshake state (RFC 6455 §5.5.1, §7). */
typedef struct WsCloseHandshake
{
  int            sent;      ///< we have sent a CLOSE frame
  int            received;  ///< the peer has sent a CLOSE frame
  unsigned short code;      ///< the status code we decided on
} WsCloseHandshake;

#ifdef __cplusplus
}
#endif

#endif
