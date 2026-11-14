/**
 * @file ws_handshake_type.h
 * @brief Public types for the RFC 6455 opening handshake.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_HANDSHAKE_TYPE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_HANDSHAKE_TYPE_H

#include <uflib/uflib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The reply is a valid WebSocket opening handshake. */
#define WS_HANDSHAKE_OK              0
/** Invalid argument, or a reply that cannot be parsed at all. */
#define WS_HANDSHAKE_ERR_ARGUMENT  (-1)
/** The reply is not 101. */
#define WS_HANDSHAKE_ERR_STATUS    (-2)
/** `Upgrade: websocket` or `Connection: Upgrade` is missing. */
#define WS_HANDSHAKE_ERR_UPGRADE   (-3)
/** `Sec-WebSocket-Accept` is absent or does not match the key. */
#define WS_HANDSHAKE_ERR_ACCEPT    (-4)
/** The server negotiated an extension, which this endpoint does not offer. */
#define WS_HANDSHAKE_ERR_EXTENSION (-5)
/** The server selected a subprotocol that was not offered. */
#define WS_HANDSHAKE_ERR_PROTOCOL  (-6)
/** The server rejected the protocol version (426). */
#define WS_HANDSHAKE_ERR_VERSION   (-7)

/** What a validated reply carried. */
typedef struct WsHandshakeReply
{
  int  status;              ///< HTTP status code as received
  char accept[29];          ///< Sec-WebSocket-Accept, NUL-terminated
  char protocol[64];        ///< Sec-WebSocket-Protocol, "" when absent
  int  extension_present;   ///< non-zero when Sec-WebSocket-Extensions was sent
} WsHandshakeReply;

#ifdef __cplusplus
}
#endif

#endif
