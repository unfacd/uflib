/**
 * @file ws_message.c
 * @brief RFC 6455 message reassembly: frames in, one message out.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <uflib/msg_framing/websocket/ws_message.h>

#include <string.h>

#include "ws_utf8_priv.h"

static void
sReset(WsMessageAssembler *assembler)
{
  assembler->state     = WS_MSG_STATE_IDLE;
  assembler->opcode    = WS_OPCODE_CONTINUATION;
  assembler->length    = 0;
  assembler->fragments = 0;
  ws_utf8_reset(&assembler->utf8);
}

static int
sIsControl(WsOpcode opcode)
{
  return opcode == WS_OPCODE_CLOSE || opcode == WS_OPCODE_PING || opcode == WS_OPCODE_PONG;
}

void
WsMessageAssemblerInit(WsMessageAssembler *assembler, size_t limit)
{
  if (!assembler) return;

  memset(assembler, 0, sizeof *assembler);
  assembler->limit = limit;
  sReset(assembler);
}

int
WsMessageAssemblerConsume(WsMessageAssembler *assembler, const WsFrameInfo *frame,
                          const unsigned char *payload, unsigned char *message,
                          size_t message_cap)
{
  size_t limit;
  size_t needed;
  int    found_start = 0;

  if (!assembler || !frame) return WS_MESSAGE_ERR_ARGUMENT;
  if (frame->payload_len && (!payload || !message)) return WS_MESSAGE_ERR_ARGUMENT;
  if (assembler->state == WS_MSG_STATE_FAILED) return WS_MESSAGE_ERR_PROTOCOL;

  /* A completed message is readable until the next frame arrives. */
  if (assembler->state == WS_MSG_STATE_COMPLETE) sReset(assembler);

  /* Control frames interleave freely and are not part of the message. */
  if (sIsControl(frame->opcode)) return WS_MESSAGE_INCOMPLETE;

  switch (frame->opcode) {
  case WS_OPCODE_TEXT:
  case WS_OPCODE_BINARY:
    if (assembler->state == WS_MSG_STATE_IN_PROGRESS) {
      assembler->state = WS_MSG_STATE_FAILED;
      return WS_MESSAGE_ERR_PROTOCOL;
    }
    assembler->opcode = frame->opcode;
    found_start = 1;
    break;

  case WS_OPCODE_CONTINUATION:
    if (assembler->state != WS_MSG_STATE_IN_PROGRESS) {
      assembler->state = WS_MSG_STATE_FAILED;
      return WS_MESSAGE_ERR_PROTOCOL;
    }
    break;

  default:
    assembler->state = WS_MSG_STATE_FAILED;
    return WS_MESSAGE_ERR_PROTOCOL;
  }

  if (frame->payload_len > SIZE_MAX - assembler->length) {
    assembler->state = WS_MSG_STATE_FAILED;
    return WS_MESSAGE_ERR_PROTOCOL;
  }
  needed = assembler->length + frame->payload_len;

  limit = assembler->limit ? assembler->limit : message_cap;
  if (needed > limit) {
    assembler->state = WS_MSG_STATE_FAILED;
    return WS_MESSAGE_ERR_CAPACITY;
  }
  if (needed > message_cap) {
    assembler->state = WS_MSG_STATE_FAILED;
    return WS_MESSAGE_ERR_ARGUMENT;
  }

  /* TEXT messages carry UTF-8, and a sequence may straddle fragments. */
  if (found_start) ws_utf8_reset(&assembler->utf8);
  if (assembler->opcode == WS_OPCODE_TEXT &&
      !ws_utf8_validate(&assembler->utf8, payload, frame->payload_len, frame->fin)) {
    assembler->state = WS_MSG_STATE_FAILED;
    return WS_MESSAGE_ERR_ENCODING;
  }

  if (frame->payload_len) {
    memcpy(message + assembler->length, payload, frame->payload_len);
  }
  assembler->length = needed;
  assembler->fragments++;
  assembler->state = frame->fin ? WS_MSG_STATE_COMPLETE : WS_MSG_STATE_IN_PROGRESS;

  return frame->fin ? WS_MESSAGE_COMPLETE : WS_MESSAGE_INCOMPLETE;
}
