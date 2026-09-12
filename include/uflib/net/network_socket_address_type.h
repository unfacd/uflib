/*

 Copyright (c) 2015-20266 unfacd works

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

#ifndef UFLIB_NET_NETWORK_SOCKET_ADDRESS_TYPE_H
#define UFLIB_NET_NETWORK_SOCKET_ADDRESS_TYPE_H

#include <netinet/in.h>
#include <sys/socket.h>

/** Socket Address flags */
enum NetworkSocketAddressFlags {
	NSA_ADDR      = 1<<0,
	NSA_PORT      = 1<<1,
	NSA_ALL       = NSA_ADDR | NSA_PORT
};

//generic holder of network address
typedef struct NetworkSocketAddress {
	union {
		struct sockaddr sa;
		struct sockaddr_in in;
		struct sockaddr_in6 in6;
		uint8_t padding[28];
	} u;

	socklen_t len;
}	NetworkSocketAddress;


#endif /* UFLIB_NET_NETWORK_SOCKET_ADDRESS_TYPE_H */
