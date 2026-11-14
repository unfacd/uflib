/**
 * @file ws_utf8_priv.h
 * @brief Private interface of the incremental UTF-8 validator.
 *
 * Not part of the public contract: the validator is driven by the module's own
 * users (message assembler, close-payload decoding).
 */
#ifndef UFLIB_MSG_FRAMING_WEBSOCKET_WS_UTF8_PRIV_H
#define UFLIB_MSG_FRAMING_WEBSOCKET_WS_UTF8_PRIV_H

#include <stddef.h>

#include <uflib/msg_framing/websocket/ws_utf8_type.h>

/** Reset the validator: valid, no partial sequence. */
void ws_utf8_reset(WsUtf8Validator *validator);

/**
 * Feed bytes.
 *
 * @param final  non-zero when no more bytes belong to the same text, so a
 *               trailing partial sequence is a violation.
 * @return 1 while the input remains valid UTF-8, 0 as soon as it is not.
 */
int ws_utf8_validate(WsUtf8Validator *validator, const unsigned char *bytes, size_t len,
                     int final);

#endif
