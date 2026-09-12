/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef UFLIB_UUID4_TYPE_H
#define UFLIB_UUID4_TYPE_H

#include <uflib/uuid4/uuid4.h>
#include <uflib/uuid_type.h>


typedef struct Uuid4 {
    struct {
        UUID4_T *by_ref;
        UUID4_T by_value;
    } raw;

    struct {
        char *by_ref;
        char by_value[UUID4_STR_BUFFER_SIZE + 1];

        struct {
            const char *(*serialised_value_getter)(struct Uuid4 *);
        } accessor;
    } serialised;
} Uuid4;

#endif //UFSRV_UUID4_TYPE_H
