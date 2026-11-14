/**
 * @file ws_frame_priv.h
 * @brief Private constants for the RFC 6455 frame codec.
 */
#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_PRIV_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_FRAME_PRIV_H

#include <uflib/msg_framing/websocket/ws_frame.h>

/* RFC 6455 §5.5: control frames carry at most 125 payload bytes. */
#define WS_CONTROL_PAYLOAD_MAX 125u

/* Leaves room for the two-byte status code inside a control frame. */
#define WS_CLOSE_REASON_MAX    (WS_CONTROL_PAYLOAD_MAX - 2u)

/* Header forms: 7-bit, 16-bit and 64-bit payload lengths. */
#define WS_HEADER_LEN_SMALL    2u
#define WS_HEADER_LEN_MEDIUM   4u
#define WS_HEADER_LEN_LARGE    10u
#define WS_LENGTH_16BIT        126u
#define WS_LENGTH_64BIT        127u
#define WS_MASK_KEY_LEN        4u

#endif
