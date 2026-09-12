/**
 * @file utils_collection_str.h
 * @brief String-specialised helpers for CollectionDescriptor
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 12/09/2026.
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

#ifndef UFLIB_UTILS_COLLECTION_STR_H
#define UFLIB_UTILS_COLLECTION_STR_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/collection_descriptor_type.h>

/* ── String iterator ──────────────────────────────────────────────────────── */

/*! Iterator over the string elements of a CollectionDescriptor.
 *
 *  Specialises CollectionDescriptor for the common case where every element
 *  is a NUL-terminated string referenced by pointer (i.e. `collection_base_offset`
 *  is 0 and `collection[i]` is a `const char *`).  NULL slots are skipped.
 */
typedef struct CollectionStringIterator
{
  const CollectionDescriptor *descriptor_ptr;  ///< collection being iterated
  size_t                      index;           ///< next slot to yield
} CollectionStringIterator;

/**
 * @brief Create a string iterator over a CollectionDescriptor.
 *
 * @param descriptor_ptr Collection of strings to iterate (may be NULL — the
 *                       iterator is then immediately exhausted).
 * @return An iterator positioned at the first element.
 *
 * @code{.c}
 * CollectionStringIterator it = CollectionStringIteratorMake(types);
 * const char *name;
 * while ((name = CollectionStringIteratorNext(&it))) {
 *     printf("%s\n", name);
 * }
 * @endcode
 */
PUBLIC_API CollectionStringIterator
CollectionStringIteratorMake(const CollectionDescriptor *descriptor_ptr);

/**
 * @brief Yield the next non-NULL string from the iterator.
 *
 * @param it_ptr  Iterator (returned by CollectionStringIteratorMake()).
 * @return The next string, or NULL when exhausted (or the iterator is invalid).
 *         NULL slots are skipped, so a non-NULL return is always a string.
 */
PUBLIC_API const char *
CollectionStringIteratorNext(CollectionStringIterator *it_ptr);

/**
 * @brief Reset the iterator to the beginning of the collection.
 *
 * @param it_ptr  Iterator to rewind (NULL is a no-op).
 */
PUBLIC_API void
CollectionStringIteratorReset(CollectionStringIterator *it_ptr);

/* ── Callback iteration ───────────────────────────────────────────────────── */

/*! Per-element callback for CollectionStringIteratorWithOperator(). */
typedef void (*CollectionStringOperator)(size_t index, const char *node);

/**
 * @brief Invoke @p op for each string in the collection, with its slot index.
 *
 * Walks the collection in order and calls @p op(index, node) for every
 * non-NULL string, where @p index is the element's slot index (NULL slots are
 * skipped).  This is the callback counterpart to the explicit
 * CollectionStringIterator.
 *
 * @param collection  Collection of strings to iterate (NULL is a no-op).
 * @param op          Callback invoked once per non-NULL string (NULL is a no-op).
 *
 * @code{.c}
 * static void print_node(size_t index, const char *node) {
 *     printf("[%zu] %s\n", index, node);
 * }
 * CollectionStringIteratorWithOperator(cd, print_node);
 * @endcode
 */
PUBLIC_API void
CollectionStringIteratorWithOperator(const CollectionDescriptor *collection,
                                     CollectionStringOperator op);

/* ── Convenience accessors ────────────────────────────────────────────────── */

/**
 * @brief Number of string slots in a CollectionDescriptor.
 *
 * @param descriptor_ptr  Collection (NULL returns 0).
 * @return The `collection_sz` element count.
 */
PUBLIC_API size_t
CollectionDescriptorStringCount(const CollectionDescriptor *descriptor_ptr);

/**
 * @brief Fetch the string at a slot index, with bounds checking.
 *
 * @param descriptor_ptr  Collection of strings (NULL returns NULL).
 * @param index           Slot index.
 * @return The string at @p index, or NULL if out of range / the slot is NULL.
 */
PUBLIC_API const char *
CollectionDescriptorStringAt(const CollectionDescriptor *descriptor_ptr,
                             size_t index);

/* ── String tokeniser + construction ─────────────────────────────────────── */

/*! Single-char delimiter selector for the convenience front ends. */
typedef enum StringTokeniser
{
  STR_TOKEN_SPACE = 0,  ///< split on ' '
  STR_TOKEN_COLON = 1,  ///< split on ':'
  STR_TOKEN_COMMA = 2   ///< split on ','
} StringTokeniser;

/**
 * @brief Generic single-character tokeniser (strtok-like, per-thread).
 *
 * Splits @p str in-place on @p token: the first call passes the string, the
 * next token is read by passing NULL.  The returned pointer is a NUL-terminated
 * substring of the (mutable) input; it is not newly allocated.  Consecutive
 * delimiters collapse (no empty tokens are produced).  The save state is
 * thread-local, so the tokeniser is safe to use concurrently from distinct
 * threads.
 *
 * @param str    String to tokenise, or NULL to fetch the next token.
 * @param token  Single delimiter character.
 * @return The next token, or NULL when exhausted.
 *
 * @code{.c}
 * char buf[] = "one,two,three";
 * char *t = CollectionTokenise(buf, ',');
 * while (t) {
 *     printf("%s\n", t);
 *     t = CollectionTokenise(NULL, ',');
 * }
 * @endcode
 */
PUBLIC_API char *
CollectionTokenise(char *str, char token);

/**
 * @brief Split a string into a one-slab CollectionDescriptor of tokens.
 *
 * Tokenises @p string on @p token using @p tokeniser and returns a
 * CollectionDescriptor whose elements are the NUL-terminated tokens, all
 * carved from a single malloc block (struct + pointer array + token strings),
 * so the caller releases everything with one free().
 *
 * @param string    NUL-terminated string to split (NULL returns NULL).
 * @param token     Single delimiter character, passed to @p tokeniser on every
 *                  call.
 * @param tokeniser strtok-style single-character tokeniser (see contract below).
 * @return Newly allocated CollectionDescriptor (collection_base_offset == 0),
 *         or NULL on allocation failure.
 *
 * @par Tokeniser contract
 * @p tokeniser is a strtok-style, single-character-delimiter state machine
 * with signature `char *(char *str, char token)`:
 *
 * - First call: pass the mutable string; it returns the first token, having
 *   overwritten the delimiter in place with a NUL byte.  Returned tokens point
 *   into the caller's buffer, so the tokeniser must not allocate.
 * - Continuation: pass NULL; it returns the next token, or NULL when exhausted.
 * - Re-arm: a non-NULL str resets the state, so the tokeniser can be run over
 *   a fresh buffer after a prior session (CollectionFromString runs it twice —
 *   once to count, once to copy — over two different buffers).
 * - Deterministic: the same (input, delimiter) must yield the same token
 *   sequence, so the two passes agree.
 * - Collapsing consecutive / leading / trailing delimiters is the tokeniser's
 *   choice; CollectionFromString records whatever tokens are returned.
 *
 * The reference implementation is CollectionTokenise(); any function with the
 * same signature and semantics may be substituted.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromString("a:b:c", ':', CollectionTokenise);
 * CollectionStringIterator it = CollectionStringIteratorMake(cd);
 * const char *name;
 * while ((name = CollectionStringIteratorNext(&it))) {
 *     printf("%s\n", name);
 * }
 * free(cd);   // one free releases the whole slab
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
CollectionFromString(const char *string, char token,
                     char *(*tokeniser)(char *, char));

/**
 * @brief Convenience: tokenise @p string on spaces.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromStringSpaceTokenised("a b c");
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
CollectionFromStringSpaceTokenised(const char *string);

/**
 * @brief Convenience: tokenise @p string on colons.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromStringColonTokenised("a:b:c");
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
CollectionFromStringColonTokenised(const char *string);

/**
 * @brief Convenience: tokenise @p string on commas.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromStringCommaTokenised("a,b,c");
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
CollectionFromStringCommaTokenised(const char *string);

/**
 * @brief Convenience: tokenise @p string on the delimiter selected by @p type.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromStringTokenised("a b c", STR_TOKEN_SPACE);
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
CollectionFromStringTokenised(const char *string, StringTokeniser type);

/* ── Describe (JSON / YAML / INI) ─────────────────────────────────────────── */

/*! Output format for CollectionFromStringDescribe(). */
typedef enum DescribeFormat
{
  DESCRIBE_FORMAT_JSON = 0,  ///< JSON array, e.g. ["a","b"]
  DESCRIBE_FORMAT_YAML = 1,  ///< YAML block sequence, e.g. "- a\n- b\n"
  DESCRIBE_FORMAT_INI  = 2   ///< INI indexed keys, e.g. "0=a\n1=b\n"
} DescribeFormat;

/**
 * @brief Describe a string collection in JSON, YAML, or INI.
 *
 * Renders the collection's strings in the requested format and returns a
 * malloc'd, NUL-terminated string the caller must free().
 *
 * @param collection  Collection of strings to describe (NULL returns NULL).
 * @param format      Output format (DESCRIBE_FORMAT_*).
 * @return Heap-allocated description string (caller frees), or NULL on failure.
 *
 * @code{.c}
 * CollectionDescriptor *cd = CollectionFromStringColonTokenised("a:b:c");
 * char *json = CollectionFromStringDescribe(cd, DESCRIBE_FORMAT_JSON);
 * printf("%s\n", json);   // ["a","b","c"]
 * free(json);
 * free(cd);
 * @endcode
 */
PUBLIC_API char *
CollectionFromStringDescribe(const CollectionDescriptor *collection,
                             DescribeFormat format);

/**
 * @brief Convenience: describe @p collection as a JSON array.
 *
 * @code{.c}
 * char *json = CollectionFromStringDescribeAsJson(cd);
 * @endcode
 */
PUBLIC_API char *
CollectionFromStringDescribeAsJson(const CollectionDescriptor *collection);

/**
 * @brief Convenience: describe @p collection as a YAML block sequence.
 *
 * @code{.c}
 * char *yaml = CollectionFromStringDescribeAsYaml(cd);
 * @endcode
 */
PUBLIC_API char *
CollectionFromStringDescribeAsYaml(const CollectionDescriptor *collection);

/**
 * @brief Convenience: describe @p collection as INI indexed keys.
 *
 * @code{.c}
 * char *ini = CollectionFromStringDescribeAsIni(cd);
 * @endcode
 */
PUBLIC_API char *
CollectionFromStringDescribeAsIni(const CollectionDescriptor *collection);

#endif /* UFLIB_UTILS_COLLECTION_STR_H */
