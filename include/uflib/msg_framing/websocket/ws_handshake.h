/**
 * @file ws_handshake.h
 * @brief RFC 6455 opening-handshake primitives: key generation, accept key and
 * reply validation.
 *
 * Both ends of a connection need the same computations: a client generates the
 * key and checks the accept value, a server computes the accept value from the
 * client's key. This module implements those without owning a socket, an HTTP
 * parser or a policy about cookies and forwarding headers.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_HANDSHAKE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_HANDSHAKE_H

#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/msg_framing/websocket/ws_handshake_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a client Sec-WebSocket-Key.
 *
 * 16 random bytes from the platform CSPRNG, base64-encoded into 24 characters.
 *
 * @param out_b64 destination of 25 bytes (24 characters plus NUL).
 * @return 0 on success, -1 when the CSPRNG fails or out_b64 is NULL.
 */
PUBLIC_API int WsHandshakeKey(char out_b64[25]);

/**
 * @brief Compute the Sec-WebSocket-Accept value for a key.
 *
 * base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
 *
 * @param key      the Sec-WebSocket-Key as sent on the wire.
 * @param out_b64  destination of 29 bytes (28 characters plus NUL).
 * @return 0 on success, -1 for a NULL or over-long key, or a hashing failure.
 */
PUBLIC_API int WsHandshakeAcceptKey(const char *key, char out_b64[29]);

/**
 * @brief Validate a server's opening-handshake reply.
 *
 * Checks, in this order: the reply parses, the status is 101 (426 reports
 * WS_HANDSHAKE_ERR_VERSION), `Upgrade: websocket`, `Connection: Upgrade`,
 * `Sec-WebSocket-Accept` against @p key, that no extension was negotiated, and
 * that a selected subprotocol was one of @p offered_protocols.
 *
 * Header names are matched case-insensitively and values tolerate surrounding
 * whitespace, as RFC 7230 requires.
 *
 * @param response             reply bytes, not necessarily NUL-terminated.
 * @param len                  reply length.
 * @param key                  the key the client sent.
 * @param offered_protocols    NULL-terminated list of offered protocols, or
 *                             NULL when none were offered. Matching is
 *                             case-sensitive (RFC 6455 §4.1).
 * @param reply                optional; filled on success.
 *
 * @return WS_HANDSHAKE_OK, or one of the WS_HANDSHAKE_ERR_* codes.
 */
PUBLIC_API int WsHandshakeValidateReply(const char *response, size_t len, const char *key,
                                        const char *const *offered_protocols,
                                        WsHandshakeReply *reply);

#ifdef __cplusplus
}
#endif

#endif
