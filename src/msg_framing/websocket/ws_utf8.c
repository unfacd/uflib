/**
 * @file ws_utf8.c
 * @brief Incremental UTF-8 validation (RFC 3629).
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ws_utf8_priv.h"

#define WS_UTF8_MAX_CODEPOINT 0x10FFFFu
#define WS_UTF8_SURROGATE_MIN 0xD800u
#define WS_UTF8_SURROGATE_MAX 0xDFFFu

void
ws_utf8_reset(WsUtf8Validator *validator)
{
  validator->need      = 0;
  validator->codepoint = 0;
  validator->minimum   = 0;
  validator->valid     = 1;
}

int
ws_utf8_validate(WsUtf8Validator *validator, const unsigned char *bytes, size_t len, int final)
{
  size_t i;

  if (!validator->valid) return 0;

  for (i = 0; i < len; i++) {
    unsigned char b = bytes[i];

    if (validator->need) {
      if ((b & 0xC0u) != 0x80u) {
        validator->valid = 0;
        return 0;
      }
      validator->codepoint = (validator->codepoint << 6) | (b & 0x3Fu);
      if (--validator->need == 0) {
        if (validator->codepoint < validator->minimum ||
            validator->codepoint > WS_UTF8_MAX_CODEPOINT ||
            (validator->codepoint >= WS_UTF8_SURROGATE_MIN &&
             validator->codepoint <= WS_UTF8_SURROGATE_MAX)) {
          validator->valid = 0;
          return 0;
        }
      }
      continue;
    }

    if (b < 0x80u) continue; /* ASCII */

    if (b >= 0xC2u && b <= 0xDFu) {          /* 2-byte */
      validator->need      = 1;
      validator->codepoint = b & 0x1Fu;
      validator->minimum   = 0x80u;
    }
    else if (b >= 0xE0u && b <= 0xEFu) {     /* 3-byte */
      validator->need      = 2;
      validator->codepoint = b & 0x0Fu;
      validator->minimum   = 0x800u;
    }
    else if (b >= 0xF0u && b <= 0xF4u) {     /* 4-byte */
      validator->need      = 3;
      validator->codepoint = b & 0x07u;
      validator->minimum   = 0x10000u;
    }
    else {                                   /* 0x80-0xC1, 0xF5-0xFF */
      validator->valid = 0;
      return 0;
    }
  }

  if (final && validator->need) {
    validator->valid = 0;
    return 0;
  }

  return validator->valid;
}
