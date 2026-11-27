/**
 * @file id_64bit_test.c
 * @brief The identifier map contract: id zero, replace, remove, limits.
 *
 * The values are opaque addresses the map must never dereference, so the test
 * hands it addresses of unrelated storage and checks only identity.
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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <uflib/adt/robinhood_hashtable/id_64bit.h>

int
main(void)
{
  static int first_object, second_object;
  void *first = &first_object;
  void *second = &second_object;

  Id64Map *map = Id64MapCreate(8, 1);
  assert(Id64MapPut(map, 0, first) == ID64_INSERTED);
  assert(Id64MapGet(map, 0) == first);
  assert(Id64MapPut(map, 0, second) == ID64_UPDATED);
  assert(Id64MapGet(map, 0) == second);
  assert(Id64MapRemove(map, 0) == second);
  assert(Id64MapGet(map, 0) == NULL);
  assert(Id64MapCount(map) == 0);

  for (uint64_t identifier = 1; identifier <= 8; identifier++) {
    assert(Id64MapPut(map, identifier, first) == ID64_INSERTED);
  }
  assert(Id64MapCount(map) == 8);
  assert(Id64MapPut(map, 99, first) == ID64_FULL); /* bounded: at the limit */
  assert(Id64MapRemove(map, 99) == NULL);

  /* iteration visits every live id exactly once, and tolerates NULL out-params */
  size_t cursor = 0, visited = 0;
  uint64_t identifier = 0;
  void *   value = NULL;
  while (Id64MapNext(map, &cursor, &identifier, &value)) {
    assert(identifier >= 1 && identifier <= 8);
    assert(value == first);
    visited++;
  }
  assert(visited == 8);
  assert(!Id64MapNext(map, &cursor, &identifier, &value));
  assert(!Id64MapNext(NULL, &cursor, &identifier, &value));
  assert(!Id64MapNext(map, NULL, &identifier, &value));

  /* a NULL value is the empty-slot marker, never an entry */
  assert(Id64MapPut(map, 42, NULL) == ID64_ERROR);

  /* a NULL handle is tolerated on the read paths, rejected on the write path */
  assert(Id64MapGet(NULL, 1) == NULL);
  assert(Id64MapCount(NULL) == 0);
  assert(Id64MapRemove(NULL, 1) == NULL);
  assert(Id64MapPut(NULL, 1, first) == ID64_ERROR);
  Id64MapDestroy(map);

  map = Id64MapCreate(0, 1); /* unbounded */
  assert(map != NULL);
  Id64MapDestroy(map);
  puts("ok");
  return 0;
}
