/**
 * @file ws_close.c
 * @brief The RFC 6455 closing handshake, as explicit state.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <uflib/msg_framing/websocket/ws_close.h>

void
WsCloseHandshakeInit(WsCloseHandshake *handshake)
{
  if (!handshake) return;

  handshake->sent     = 0;
  handshake->received = 0;
  handshake->code     = 1000;
}

int
WsCloseHandshakeOnSent(WsCloseHandshake *handshake, unsigned short code)
{
  if (!handshake) return WS_CLOSE_ACTION_INVALID;

  if (handshake->sent) return WS_CLOSE_ACTION_CLOSED;

  handshake->sent = 1;
  handshake->code = code;
  return WS_CLOSE_ACTION_NONE;
}

int
WsCloseHandshakeOnReceive(WsCloseHandshake *handshake, const WsFrameInfo *frame)
{
  if (!handshake || !frame) return WS_CLOSE_ACTION_INVALID;

  if (frame->opcode == WS_OPCODE_CLOSE) {
    if (handshake->received) return WS_CLOSE_ACTION_CLOSED;
    handshake->received = 1;
    return handshake->sent ? WS_CLOSE_ACTION_CLOSED : WS_CLOSE_ACTION_REPLY;
  }

  if (frame->opcode == WS_OPCODE_PING || frame->opcode == WS_OPCODE_PONG) {
    return WS_CLOSE_ACTION_NONE;
  }

  /* data frames after either side closed are a violation */
  if (handshake->sent || handshake->received) return WS_CLOSE_ACTION_INVALID;

  return WS_CLOSE_ACTION_NONE;
}

int
WsCloseHandshakeCanSendData(const WsCloseHandshake *handshake)
{
  if (!handshake) return 0;
  return !handshake->sent && !handshake->received;
}

unsigned short
WsCloseCodeForError(int result)
{
  switch (result) {
  case -1: return 1002; /* protocol error */
  case -2: return 1009; /* message too big */
  case -3: return 1011; /* internal error */
  case -4: return 1007; /* invalid payload data */
  case -5: return 1002; /* reserved bit set: protocol error */
  default: return 1000; /* normal closure */
  }
}
