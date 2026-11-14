/**
 * @file ws_frame.h
 * @brief RFC 6455 frame codec: self-contained, allocation-free, direction-agnostic.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_H

#include <sys/types.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/msg_framing/websocket/ws_frame_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode one frame.
 *
 * @param out      destination buffer.
 * @param cap      capacity of out.
 * @param payload  payload bytes; may be NULL only when len is 0.
 * @param len      payload length, up to SIZE_MAX.
 * @param opcode   frame opcode.
 * @param fin      non-zero for the final (or only) frame.
 * @param mask     NULL to emit a server frame (unmasked); otherwise a 4-byte
 *                 masking key to emit a client frame.
 *
 * @return the frame length in bytes, or -1 for invalid arguments, an illegal
 *         control frame (fragmented, or payload above 125), an unknown opcode,
 *         or a destination that is too small.
 */
PUBLIC_API ssize_t WsFrameEncode(unsigned char *out, size_t cap,
                                 const unsigned char *payload, size_t len,
                                 WsOpcode opcode, int fin, const unsigned char mask[4]);

/**
 * @brief Decode one frame from the front of in[0..in_len).
 *
 * The payload is unmasked into payload_out; a zero-length payload does not
 * touch payload_out and may be NULL.
 *
 * @param in          input buffer.
 * @param in_len      bytes available.
 * @param flags       WS_FRAME_REQUIRE_MASKED / _UNMASKED / _ACCEPT_ANY.
 * @param payload_out payload destination (unmasked).
 * @param payload_cap capacity of payload_out.
 * @param info        receives the frame's opcode, FIN, masking direction,
 *                    header length and payload length.
 * @param consumed    receives the bytes of in consumed by this frame.
 *
 * @return WS_FRAME_DECODED, WS_FRAME_NEED_MORE, or one of the error codes.
 */
PUBLIC_API int WsFrameDecode(const unsigned char *in, size_t in_len, unsigned int flags,
                             unsigned char *payload_out, size_t payload_cap,
                             WsFrameInfo *info, size_t *consumed);

/**
 * @brief Build the response a received control frame mandates.
 *
 * - PING  → PONG carrying the same application data.
 * - CLOSE → CLOSE echoing the received status code and reason. A malformed
 *           close payload is answered with 1002 (protocol error), as RFC 6455
 *           §7.1.7 requires.
 * - PONG, and every data frame → no response.
 *
 * The caller must send the result, then (for CLOSE) close the transport, and
 * must not answer the same CLOSE twice; that close-once bookkeeping is
 * application state, not codec state.
 *
 * @param info     a frame previously returned by WsFrameDecode.
 * @param payload  that frame's payload, as decoded.
 * @param out      destination for the reply frame.
 * @param cap      capacity of out.
 * @param mask     NULL for a server reply (unmasked), otherwise the 4-byte
 *                 masking key for a client reply.
 *
 * @return the reply frame length, 0 when no response is mandated, or -1 on
 *         invalid arguments or a destination that is too small.
 */
PUBLIC_API ssize_t WsFrameControlReply(const WsFrameInfo *info,
                                       const unsigned char *payload,
                                       unsigned char *out, size_t cap,
                                       const unsigned char mask[4]);

/**
 * @brief Encode a close payload.
 *
 * Two-byte status code followed by an optional UTF-8 reason (at most 123 bytes,
 * so the payload never exceeds the control-frame limit of 125).
 *
 * @return the payload length, or -1 for an invalid code, an over-long reason,
 *         or too small a destination.
 */
PUBLIC_API ssize_t WsClosePayloadEncode(unsigned char *out, size_t cap,
                                        unsigned short code, const char *reason);

/**
 * @brief Decode a close payload.
 *
 * @param payload     close frame payload (may be NULL when len is 0).
 * @param len         payload length.
 * @param code        receives the status code, or 1005 when the payload carried
 *                    no code (RFC 6455 §7.1.5).
 * @param reason_out  optional buffer for the reason text; NUL-terminated when
 *                    it fits.
 * @param reason_cap  capacity of reason_out.
 * @param reason_len  receives the reason length in bytes.
 *
 * @return 0 on success, -1 when the payload is malformed (a single byte, or an
 *         illegal status code).
 */
PUBLIC_API int WsClosePayloadDecode(const unsigned char *payload, size_t len,
                                    unsigned short *code,
                                    char *reason_out, size_t reason_cap, size_t *reason_len);

#ifdef __cplusplus
}
#endif

#endif
