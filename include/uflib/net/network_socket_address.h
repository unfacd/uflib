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

/*
 * network_socket_address.h
 *
 *  Created on: 5 Feb 2017
 *      Author: ayman
 */

#ifndef UFLIB_NET_NETWORK_SOCKET_ADDRESS_H
#define UFLIB_NET_NETWORK_SOCKET_ADDRESS_H

#include <uflib/uflib_defs.h>

#define HAVE_INET_PTON	1
#define HAVE_INET_NTOP	1

#include <stdint.h>
#include <uflib/net/network_socket_address_type.h>

PUBLIC_API void InitNetworkSocketAddress(NetworkSocketAddress *sa, int af);
PUBLIC_API int NetworkSocketAddressInstantiate(NetworkSocketAddress *socket_address_ptr, const char *addr, uint16_t port);

PUBLIC_API int ConvertSocketAddressToNetworkFormat(const char *addr, NetworkSocketAddress *sa);
PUBLIC_API int ConvertSocketAddressToReadableFormat(const NetworkSocketAddress *sa, char *buf, int size);
PUBLIC_API void NetworkSocketAddressSetInet4FromHost(NetworkSocketAddress *sa, uint32_t addr, uint16_t port);
PUBLIC_API void NetworkSocketAddressSetInet6FromHost(NetworkSocketAddress *sa, const uint8_t *addr, uint16_t port);
PUBLIC_API int NetworkSocketAddressSetLocalFromFd(int sock_fd, NetworkSocketAddress *local);
PUBLIC_API int NetworkSocketAddressSetPeerFromFd(int sock_fd, NetworkSocketAddress *local);
PUBLIC_API int NetworkSocketAddressSetInet6FromSockaddr(NetworkSocketAddress *sa, const struct sockaddr *s);
PUBLIC_API void NetworkSocketAddressSetPort(NetworkSocketAddress *sa, uint16_t port);
PUBLIC_API int NetworkSocketAddressGetAddressFamily(const NetworkSocketAddress *sa);
PUBLIC_API uint32_t NetworkSocketAddressGetInet4Address(const NetworkSocketAddress *sa);
PUBLIC_API void NetworkSocketAddressGetInet6Address(const NetworkSocketAddress *sa, uint8_t *addr);
PUBLIC_API int NetworSocketAddresssaToReadable(const NetworkSocketAddress *sa, char *buf, int size);
PUBLIC_API uint16_t NetworkSocketAddressGetPort(const NetworkSocketAddress *sa);
PUBLIC_API bool SocketAddressIsAttributeSet(const NetworkSocketAddress *sa, int flag);
PUBLIC_API uint32_t NetworkSocketAddressGetHashValue(const NetworkSocketAddress *sa, int flag);
PUBLIC_API void NetworkSocketAddressCopy(NetworkSocketAddress *dst, const NetworkSocketAddress *src);
PUBLIC_API bool NetworkSocketAddressCompare(const NetworkSocketAddress *l, const NetworkSocketAddress *r, int flag);
PUBLIC_API bool NetworkSocketAddressIsLinkLocal(const NetworkSocketAddress *sa);
PUBLIC_API bool NetworkSocketAddressIsLoopback(const NetworkSocketAddress *sa);
PUBLIC_API bool NetworkSocketAddressIsAddressUnspecified(const NetworkSocketAddress *sa);

#endif /* UFLIB_NET_NETWORK_SOCKET_ADDRESS_H */
