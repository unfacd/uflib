/**
 * @file ws_message_type.h
 * @brief Public types for RFC 6455 message reassembly.
 *
 * A WebSocket message is one or more frames: the first carries TEXT or BINARY
 * and FIN=0, each middle frame is CONTINUATION with FIN=0, and the last is
 * CONTINUATION with FIN=1. Control frames may appear between them.
 *
 * The assembler tracks that progression without owning memory: the caller
 * supplies the message buffer and its capacity.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_MESSAGE_TYPE_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_MESSAGE_TYPE_H

#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/msg_framing/websocket/ws_frame_type.h>
#include <uflib/msg_framing/websocket/ws_utf8_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The message continues: feed the next frame. */
#define WS_MESSAGE_INCOMPLETE   0
/** The message is complete: read opcode/length, then the caller dispatches it. */
#define WS_MESSAGE_COMPLETE     1
/** Framing violation (continuation rules). The connection must be closed. */
#define WS_MESSAGE_ERR_PROTOCOL (-1)
/** The message exceeds the configured limit. Close with 1009. */
#define WS_MESSAGE_ERR_CAPACITY (-2)
/** Invalid argument, or a message buffer smaller than the limit allows. */
#define WS_MESSAGE_ERR_ARGUMENT (-3)
/** A TEXT message (or a close reason) is not valid UTF-8. Close with 1007. */
#define WS_MESSAGE_ERR_ENCODING (-4)

/** Reassembly state. */
typedef enum WsMessageState
{
  WS_MSG_STATE_IDLE = 0,      ///< no message in progress
  WS_MSG_STATE_IN_PROGRESS,   ///< first fragment seen, awaiting CONTINUATION
  WS_MSG_STATE_COMPLETE,      ///< final fragment consumed; readable until the next call
  WS_MSG_STATE_FAILED,        ///< protocol violation; sticky until re-initialised
} WsMessageState;

/** Reassembly state. Zero-initialise, then call WsMessageAssemblerInit(). */
typedef struct WsMessageAssembler
{
  WsMessageState state;      ///< current state
  WsOpcode       opcode;     ///< TEXT or BINARY, fixed by the first frame
  size_t         length;     ///< bytes assembled so far
  size_t         limit;      ///< maximum message size; 0 means "message_cap"
  size_t         fragments;  ///< frames consumed for this message
  WsUtf8Validator utf8;      ///< incremental validation, TEXT messages only
  unsigned       flags;      ///< reserved
} WsMessageAssembler;

#ifdef __cplusplus
}
#endif

#endif
