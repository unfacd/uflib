/**
 * Copyright (C) 2015-2025 unfacd works
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

#ifndef UFLIB_VERSION_H
#define UFLIB_VERSION_H

#define UFLIB_MAJOR     4
#define UFLIB_MINOR     0
#define UFLIB_PATCH     0
#define UFLIB_INTERNAL  9

const char *UfsrvUfLibVersion()      __attribute__((const));
const char *UfsrvUfLibVersionMajor() __attribute__((const));
const char *UfsrvUfLibVersionMinor() __attribute__((const));
const char *UfsrvUfLibVersionPatch() __attribute__((const));
const char *UfsrvUfLibVersionInternal() __attribute__((const));

#endif
