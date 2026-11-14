/**
 * @file ws_message.h
 * @brief RFC 6455 message reassembly: frames in, one message out.
 *
 * The frame codec (ws_frame.h) reports frames; this module applies the
 * message-level rules of RFC 6455 §5.4 — which frames may follow which, how
 * control frames interleave, and when a message is complete.
 *
 * ## Ownership
 *
 * No allocation: the caller passes the message buffer and its capacity to every
 * call. The assembler records position and legality only.
 *
 * ## Failure
 *
 * A protocol violation puts the assembler in WS_MESSAGE_FAILED, and it stays
 * there until WsMessageAssemblerInit() is called again: an endpoint that has
 * broken the framing rules must be closed, not resynchronised.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_MESSAGE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_MESSAGE_H

#include <uflib/uflib_defs.h>
#include <uflib/msg_framing/websocket/ws_message_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise (or reset) an assembler.
 *
 * @param assembler  assembler to initialise; NULL is a no-op.
 * @param limit      maximum message size in bytes. 0 means "whatever the
 *                   message buffer passed to Consume can hold". A non-zero
 *                   limit is the wire policy; exceeding it reports
 *                   WS_MESSAGE_ERR_CAPACITY, which an endpoint answers with
 *                   close code 1009.
 */
PUBLIC_API void WsMessageAssemblerInit(WsMessageAssembler *assembler, size_t limit);

/**
 * @brief Feed one decoded frame to the assembler.
 *
 * Control frames (PING/PONG/CLOSE) may arrive between fragments; they are not
 * part of the message, are not copied, and leave the state untouched. The
 * caller answers them with WsFrameControlReply().
 *
 * On WS_MESSAGE_COMPLETE the assembler keeps `opcode` and `length` readable,
 * and resets itself when the next frame is consumed, so a streaming caller
 * never has to re-initialise between messages.
 *
 * @param assembler   assembler state.
 * @param frame       frame reported by WsFrameDecode().
 * @param payload     that frame's payload, as decoded.
 * @param message     destination for the assembled message.
 * @param message_cap capacity of message.
 *
 * @return WS_MESSAGE_INCOMPLETE, WS_MESSAGE_COMPLETE, or an error code.
 */
PUBLIC_API int WsMessageAssemblerConsume(WsMessageAssembler *assembler,
                                         const WsFrameInfo *frame,
                                         const unsigned char *payload,
                                         unsigned char *message, size_t message_cap);

#ifdef __cplusplus
}
#endif

#endif
