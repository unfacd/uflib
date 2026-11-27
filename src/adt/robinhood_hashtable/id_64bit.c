/**
 * @file id_64bit.c
 * @brief The 64-bit identifier map: an rh_template.h instantiation with a void * value.
 *
 * The template owns the algorithm; this file owns the five choices that make it
 * an identifier map — the scalar key, the opaque pointer value, the hash, the
 * 64-bit equality and the NULL value as the empty-slot marker. Those choices are
 * private to this translation unit.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/adt/robinhood_hashtable/id_64bit.h>

#include <uflib/adt/robinhood_hashtable/rh_common.h>

static inline uint64_t
sId64Hash(const uint64_t *identifier, uint64_t secret_a, uint64_t secret_b)
{
  return sRhtFoldMultiply(*identifier ^ secret_a, secret_b);
}

#define RhtTemplateName Id64Map
#define RhtTemplateKey uint64_t
#define RhtTemplateValue void *
#define RhtTemplateHash(identifier, secret_a, secret_b) sId64Hash((identifier), (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) (*(left) == *(right))
#define RhtTemplateSlotEmpty(slot) ((slot)->value == NULL)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

_Static_assert(sizeof(Id64MapSlot) == 16, "slot is the identifier plus the pointer");
_Static_assert(sizeof(Id64Map) > 0, "the map type is complete in this unit only");
_Static_assert((int)ID64_FULL == (int)RH_TEMPLATE_FULL && (int)ID64_ERROR == (int)RH_TEMPLATE_ERROR &&
                   (int)ID64_INSERTED == (int)RH_TEMPLATE_INSERTED &&
                   (int)ID64_UPDATED == (int)RH_TEMPLATE_UPDATED,
               "the public result codes mirror the template's");

PUBLIC_API Id64Map *
Id64MapCreate(size_t max_entries, uint64_t seed)
{
  return sId64MapCreate(max_entries, max_entries, seed);
}

PUBLIC_API void
Id64MapDestroy(Id64Map *map)
{
  sId64MapDestroy(map);
}

PUBLIC_API Id64Result
Id64MapPut(Id64Map *map, uint64_t identifier, void *value)
{
  if (!map || !value) return ID64_ERROR;
  return (Id64Result)sId64MapInsert(map, &identifier, value);
}

PUBLIC_API void *
Id64MapGet(const Id64Map *map, uint64_t identifier)
{
  void *value = NULL;
  if (!map) return NULL;
  return sId64MapFind(map, &identifier, &value) ? value : NULL;
}

PUBLIC_API void *
Id64MapRemove(Id64Map *map, uint64_t identifier)
{
  void *value = Id64MapGet(map, identifier);
  if (value) sId64MapErase(map, &identifier);
  return value;
}

PUBLIC_API bool
Id64MapNext(const Id64Map *map, size_t *cursor, uint64_t *identifier, void **value)
{
  if (!map || !cursor || !identifier || !value) return false;
  const uint64_t *key = NULL;
  if (!sId64MapNext(map, cursor, &key, value)) return false;
  *identifier = *key;
  return true;
}

PUBLIC_API size_t
Id64MapCount(const Id64Map *map)
{
  return map ? sId64MapSize(map) : 0;
}
