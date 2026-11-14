/**
 * @file utils_json.h
 * @brief JSON serialisation primitives: JsonEscapeString().
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UFLIB_UTILS_JSON_H
#define UFLIB_UTILS_JSON_H

#include <uflib/uflib_defs.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>

/**
 * @brief Append a C string to a descriptor as a JSON string literal.
 *
 * Emits the opening quote, the escaped body and the closing quote, so the
 * caller can concatenate the result directly with the surrounding object
 * members.  The emitted text is a complete JSON value: `"..."`, or `null` when
 * @p str is NULL.
 *
 * Escaped: `"`, `\`, and every control character below U+0020 — the dedicated
 * short forms (`\n`, `\r`, `\t`, `\b`, `\f`) where JSON defines one, otherwise
 * `\u00XX`.  Bytes at or above 0x80 are copied verbatim, so the result is valid
 * JSON as long as @p str is valid UTF-8; a non-UTF-8 byte sequence is not
 * rewritten or rejected.
 *
 * @param[in,out] bd  Initialised descriptor to append to.  NULL is a no-op.
 * @param[in]     str NUL-terminated text to encode, or NULL to emit `null`.
 *
 * @note No allocation beyond the descriptor's own growth; nothing to free.
 * @note On failure to grow the descriptor the append is silently short, as
 *       BufferDescriptorAppendFormatted() returns its error rather than
 *       aborting.  Check bd->data only when the whole document must be trusted.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 64);
 * BufferDescriptorAppendFormatted(&bd, "{\"name\":");
 * JsonEscapeString(&bd, "a\"b\n");
 * BufferDescriptorAppendFormatted(&bd, "}");   // {"name":"a\"b\n"}
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API void JsonEscapeString(BufferDescriptor *bd, const char *str);

#endif //UFLIB_UTILS_JSON_H
