/**
 * Copyright (C) 2015-2021 unfacd works
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

#ifndef UFLIB_UTILS_STR_H
#define UFLIB_UTILS_STR_H

#include <uflib/uflib_defs.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
  uint64_t tab[4];
  int sep, finished;
  const char *p; // end of the current token
} TokenAux;

PUBLIC_API bool is_unsigned_long_format(const char *unsigned_long_str);
PUBLIC_API int ftoa(char *outbuf, float f);
char *ultoa(unsigned long value, char *ptr, int base);
PUBLIC_API int itoa(char *ptr, uint32_t number);
PUBLIC_API unsigned digits_count (uint64_t number, unsigned base);
const char *traverse_quoted(const char *ptr);
char *TokeniseString(const char *str, const char *sep_in, TokenAux *aux);

PUBLIC_API size_t mstrlcpy(char *, const char *, size_t);

#define IGNORE_BYTES_READ_PARAM NULL
#define IGNORE_IS_VALID_PARAM   NULL

PUBLIC_API size_t DefensiveStrlenUtf8(const char *str, size_t max_bytes, size_t *bytes_read, bool *is_valid);
PUBLIC_API size_t DefensiveStrlenUtf8Binary(const uint8_t *buf, size_t buf_size, size_t max_chars);
PUBLIC_API bool IsUtf8Valid(const char *str, size_t max_bytes);
PUBLIC_API bool DefensiveStrlen(const char * _Nonnull str, size_t max_sz, size_t * _Nullable out_len);
PUBLIC_API bool DefensiveStrlenWithMinMax(const char * _Nonnull str, size_t min_sz, size_t max_sz, size_t * _Nullable out_len);

#if defined(__GNUC__)
//call vardic function without using classic c style va variables INVOKE_FUNCTION(MyFunctionaName, {(intptr)0, (intptr)1, (intptr_t)"hello"})
//invoked function last arg will be the number of vardic args passed into it
# define INVOKE_FUNCTION(x, ...) ({ intptr_t *values = { __VA_ARGS__ }; x(__VA_ARGS__, sizeof(values) / sizeof(*values)); })
#endif

//A cannot be a regular C language expression and no macro expansion. However, if invoked from a macro, macro arguments are expanded
#define STRINGISE_NX(A) #A

//A macro expanded
#define STRINGISE(A) STRINGISE_NX(A)

//A permitted to be an expression
#define STRINGISE_EXPR(A) ((A),STRINGISE_NX(A))

//Concatenate preprocessor tokens A and B without expanding macro definitions. If invoked from a macro, macro arguments are expanded
#define CONCATENATE_NX(A, B) A ## B

//  Concatenate preprocessor tokens A and B after macro-expanding them.
#define CONCATENATE(A, B) CONCATENATE_NX(A, B)

//_convenient_ macro to stringify on the fly without having to worry about dynamic allocation.
#define get_buffer_size(...) (snprintf(NULL, 0, __VA_ARGS__) + 1)
#define STRINGIFY_PARAMETER(...) sprintf_provided_buffer(alloca(get_buffer_size(__VA_ARGS__)), __VA_ARGS__)

char *strbufdup(const char *s, size_t n);
char *strbufdup_nullable(const char *s, size_t n, char *(^on_null)(void));
char *sprintf_provided_buffer (char *user_allocated_buffer, char *format, ...);

PUBLIC_API char * mystrdup(const char *);

#endif /* UFLIB_UTILS_STR_H */
