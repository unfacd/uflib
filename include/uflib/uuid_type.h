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

#ifndef UFSRV_UUID_TYPE_H
#define UFSRV_UUID_TYPE_H

#include <uuid/uuid.h>

#ifndef UUID_SERIALISED_LENGTH
# define UUID_SERIALISED_LENGTH 36
#endif

typedef struct Uuid {
  struct {
    uuid_t *by_ref;
    uuid_t by_value;
  } raw;

  struct {
    char *by_ref;
    char by_value[UUID_SERIALISED_LENGTH + 1];

    struct {
      const char *(*serialised_value_getter)(struct Uuid *);
    } accessor;
  } serialised;
} Uuid;

#endif //UFSRV_UUID_TYPE_H
