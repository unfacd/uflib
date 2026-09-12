/**
 * @file utils_collection_str.c
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

#include <uflib/utils_collection_str.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PUBLIC_API CollectionStringIterator
CollectionStringIteratorMake(const CollectionDescriptor *descriptor_ptr)
{
  CollectionStringIterator it;

  it.descriptor_ptr = descriptor_ptr;
  it.index          = 0;

  return it;
}

PUBLIC_API const char *
CollectionStringIteratorNext(CollectionStringIterator *it_ptr)
{
  const CollectionDescriptor *cd;

  if (!it_ptr) return NULL;

  cd = it_ptr->descriptor_ptr;
  if (!cd || !cd->collection) return NULL;

  while (it_ptr->index < cd->collection_sz) {
    const char *s = (const char *)cd->collection[it_ptr->index];
    it_ptr->index++;
    if (s) return s;
  }

  return NULL;
}

PUBLIC_API void
CollectionStringIteratorReset(CollectionStringIterator *it_ptr)
{
  if (!it_ptr) return;

  it_ptr->index = 0;
}

PUBLIC_API void
CollectionStringIteratorWithOperator(const CollectionDescriptor *collection,
                                     CollectionStringOperator op)
{
  size_t i;

  if (!collection || !op || !collection->collection) return;

  for (i = 0; i < collection->collection_sz; i++) {
    const char *node = (const char *)collection->collection[i];
    if (node) {
      op(i, node);
    }
  }
}

PUBLIC_API size_t
CollectionDescriptorStringCount(const CollectionDescriptor *descriptor_ptr)
{
  return descriptor_ptr ? descriptor_ptr->collection_sz : 0;
}

PUBLIC_API const char *
CollectionDescriptorStringAt(const CollectionDescriptor *descriptor_ptr,
                             size_t index)
{
  if (!descriptor_ptr || !descriptor_ptr->collection) return NULL;
  if (index >= descriptor_ptr->collection_sz) return NULL;

  return (const char *)descriptor_ptr->collection[index];
}

static char *
sCollectionStrDup(const char *str)
{
  size_t len;
  char *dup;

  if (!str) return NULL;

  len = strlen(str) + 1;
  dup = malloc(len);
  if (dup) memcpy(dup, str, len);

  return dup;
}

static void
sCleanupFree(void *p_ptr)
{
  void **p = (void **)p_ptr;
  free(*p);
}

PUBLIC_API char *
CollectionTokenise(char *str, char token)
{
  static _Thread_local char *s_save = NULL;
  char *start;

  if (str) s_save = str;
  if (!s_save) return NULL;

  while (*s_save == token) s_save++;
  if (*s_save == '\0') {
    s_save = NULL;
    return NULL;
  }

  start = s_save;
  while (*s_save != '\0' && *s_save != token) s_save++;
  if (*s_save == token) {
    *s_save = '\0';
    s_save++;
  } else {
    s_save = NULL;
  }

  return start;
}

PUBLIC_API CollectionDescriptor *
CollectionFromString(const char *string, char token, char *(*tokeniser)(char *, char))
{
  size_t n = 0, total = 0;
  char *block;
  CollectionDescriptor *cd;
  void **slots;
  char *str_area;
  size_t i;

  if (!string || !tokeniser) return NULL;

  /* Pass 1 — count tokens and their total byte length. */
  {
    __attribute__((cleanup(sCleanupFree))) char *dup = sCollectionStrDup(string);
    char *t;

    if (!dup) return NULL;

    t = tokeniser(dup, token);
    while (t) {
      n++;
      total += strlen(t) + 1;
      t = tokeniser(NULL, token);
    }
  }  /* dup freed on scope exit */

  /* One-slab allocation: struct + pointer array + token strings. */
  block = malloc(sizeof(CollectionDescriptor) + n * sizeof(void *) + total);
  if (!block) return NULL;

  cd = (CollectionDescriptor *)block;
  cd->collection = (n > 0) ? (collection_t **)(block + sizeof(CollectionDescriptor)) : NULL;
  cd->collection_sz = n;
  cd->collection_base_offset = 0;
  cd->on_destroy_collection = NULL;

  if (n == 0) return cd;

  slots = (void **)(block + sizeof(CollectionDescriptor));
  str_area = block + sizeof(CollectionDescriptor) + n * sizeof(void *);

  /* Pass 2 — tokenise a fresh copy and store each token into the slab. */
  {
    __attribute__((cleanup(sCleanupFree))) char *dup = sCollectionStrDup(string);
    char *t;

    if (!dup) {
      free(block);
      return NULL;
    }

    i = 0;
    t = tokeniser(dup, token);
    while (t && i < n) {
      size_t len = strlen(t) + 1;
      memcpy(str_area, t, len);
      slots[i++] = str_area;
      str_area += len;
      t = tokeniser(NULL, token);
    }
  }  /* dup freed on scope exit */

  return cd;
}

PUBLIC_API CollectionDescriptor *
CollectionFromStringSpaceTokenised(const char *string)
{
  return CollectionFromString(string, ' ', CollectionTokenise);
}

PUBLIC_API CollectionDescriptor *
CollectionFromStringColonTokenised(const char *string)
{
  return CollectionFromString(string, ':', CollectionTokenise);
}

PUBLIC_API CollectionDescriptor *
CollectionFromStringCommaTokenised(const char *string)
{
  return CollectionFromString(string, ',', CollectionTokenise);
}

PUBLIC_API CollectionDescriptor *
CollectionFromStringTokenised(const char *string, StringTokeniser type)
{
  char token;

  switch (type) {
  case STR_TOKEN_COLON: token = ':'; break;
  case STR_TOKEN_COMMA: token = ','; break;
  case STR_TOKEN_SPACE:
  default:              token = ' '; break;
  }

  return CollectionFromString(string, token, CollectionTokenise);
}

static void
sCollectionJsonString(FILE *s, const char *str)
{
  if (!str) {
    fputs("null", s);
    return;
  }

  fputc('"', s);
  for (const char *p = str; *p; p++) {
    switch (*p) {
    case '"':  fputs("\\\"", s); break;
    case '\\': fputs("\\\\", s); break;
    case '\n': fputs("\\n",  s); break;
    case '\t': fputs("\\t",  s); break;
    case '\r': fputs("\\r",  s); break;
    default:   fputc(*p, s);     break;
    }
  }
  fputc('"', s);
}

PUBLIC_API char *
CollectionFromStringDescribe(const CollectionDescriptor *collection,
                             DescribeFormat format)
{
  char *buf = NULL;
  size_t sz = 0;
  FILE *s;
  size_t i;

  if (!collection) return NULL;

  s = open_memstream(&buf, &sz);
  if (!s) return NULL;

  switch (format) {
  case DESCRIBE_FORMAT_YAML:
    for (i = 0; i < collection->collection_sz; i++) {
      const char *str = CollectionDescriptorStringAt(collection, i);
      fprintf(s, "- %s\n", str ? str : "");
    }
    break;

  case DESCRIBE_FORMAT_INI:
    for (i = 0; i < collection->collection_sz; i++) {
      const char *str = CollectionDescriptorStringAt(collection, i);
      fprintf(s, "%zu=%s\n", i, str ? str : "");
    }
    break;

  case DESCRIBE_FORMAT_JSON:
  default:
    fputc('[', s);
    for (i = 0; i < collection->collection_sz; i++) {
      if (i > 0) fputc(',', s);
      sCollectionJsonString(s, CollectionDescriptorStringAt(collection, i));
    }
    fputc(']', s);
    break;
  }

  fclose(s);  /* flushes and NUL-terminates buf */
  return buf;
}

PUBLIC_API char *
CollectionFromStringDescribeAsJson(const CollectionDescriptor *collection)
{
  return CollectionFromStringDescribe(collection, DESCRIBE_FORMAT_JSON);
}

PUBLIC_API char *
CollectionFromStringDescribeAsYaml(const CollectionDescriptor *collection)
{
  return CollectionFromStringDescribe(collection, DESCRIBE_FORMAT_YAML);
}

PUBLIC_API char *
CollectionFromStringDescribeAsIni(const CollectionDescriptor *collection)
{
  return CollectionFromStringDescribe(collection, DESCRIBE_FORMAT_INI);
}
