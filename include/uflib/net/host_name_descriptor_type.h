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

//
// Created by devops on 7/22/21.
//

#ifndef UFLIB_NET_HOST_NAME_DESCRIPTOR_TYPE_H
#define UFLIB_NET_HOST_NAME_DESCRIPTOR_TYPE_H

#define HOSTNAME_MAXSIZE (_POSIX_HOST_NAME_MAX + 1)

typedef struct HostNameDescriptor {
  uint16_t port;
  char *name;
} HostNameDescriptor;

#endif //UFSRV_HOST_NAME_DESCRIPTOR_TYPE_H
