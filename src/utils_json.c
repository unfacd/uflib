/**
 * @file utils_json.c
 * @brief utils_json
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 15/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include <uflib/standard_defs.h>
#include <uflib/utils_json.h>

void
JsonEscapeString(BufferDescriptor *bd, const char *str)
{
  if (bd == NULL) return;

  if (str == NULL) {
    BufferDescriptorAppendFormatted(bd, "null");
    return;
  }

  BufferDescriptorAppendFormatted(bd, "\"");

  for (const char *p = str; *p; p++) {
    unsigned char c = (unsigned char)*p;

    switch (c) {
    case '"':  BufferDescriptorAppendFormatted(bd, "\\\""); break;
    case '\\': BufferDescriptorAppendFormatted(bd, "\\\\"); break;
    case '\b': BufferDescriptorAppendFormatted(bd, "\\b");  break;
    case '\f': BufferDescriptorAppendFormatted(bd, "\\f");  break;
    case '\n': BufferDescriptorAppendFormatted(bd, "\\n");  break;
    case '\r': BufferDescriptorAppendFormatted(bd, "\\r");  break;
    case '\t': BufferDescriptorAppendFormatted(bd, "\\t");  break;
    default:
      /* Remaining control characters have no short form; JSON requires the \u00XX form. */
      if (c < 0x20) BufferDescriptorAppendFormatted(bd, "\\u%04x", c);
      else          BufferDescriptorAppendFormatted(bd, "%c", c);
      break;
    }
  }

  BufferDescriptorAppendFormatted(bd, "\"");
}
