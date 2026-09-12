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
** net.h Copyright (c) 1998 Ayman Akt
**
** See the COPYING file for terms of use and conditions.
**
MODULEID("$Id: net.h,v 1.1 1999/07/26 01:46:59 ayman Exp $")
**
*/

#ifndef NET_H
# define NET_H

#include <uflib/uflib_defs.h>
#include <uflib/net/network_socket_address_type.h>
#include <uflib/net/socket_type.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include "host_name_descriptor_type.h"

 struct ResolvedAddress {
        char dns[MAXHOSTLEN],
             dotted[MAXHOSTLEN];
        unsigned flags;
        struct in_addr inetaddr;
       };
 typedef struct ResolvedAddress ResolvedAddress;

PUBLIC_API int SetupListeningSocket (const char *, unsigned, unsigned, unsigned);
PUBLIC_API void FetchLocalhost (void);
PUBLIC_API int InitTelnet (void);
PUBLIC_API int isdottedquad (const char *);
PUBLIC_API int RequestTCPSocket (void);
PUBLIC_API int SetSocketFlags (int, int, int);
PUBLIC_API int ResolveAddress (const char *, ResolvedAddress *);
PUBLIC_API int ConnectToServer(const char *, unsigned long, Socket *);
//int ConnectToServerSecure(const char *server, unsigned long port, SSL_CTX *ctx, Session *sesn_ptr); //temporarily disabled until Session dependency is resolved
PUBLIC_API int Connect (struct in_addr *, unsigned long, bool);
PUBLIC_API void nslookup (char *);
char *RawIPToDotted (unsigned long);
char *HostToDotted (char *);
char *DottedToHost (char *);
struct in_addr *NetworkToAddress (const char *);
char *AddressToNetwork (struct in_addr *);
PUBLIC_API int ServiceToPort (const char *, unsigned short);
char *PortToService (int, unsigned short);
char *ProtocolToName (unsigned short);
struct in_addr NetworkPrefixToAdress (unsigned int);
PUBLIC_API int IsLocalIP (const char *);
PUBLIC_API int IsSocketAlive (int socket);
PUBLIC_API int GenericDnsResolve (const char *host, char *ipbuf, size_t ipbuf_len);
//int TcpSocketOptionSetLinger (int fd);
PUBLIC_API int ConfigureSocketKeepAliveOptions(int nsocket, int interval);
PUBLIC_API int SocketOptionSetLINGER (int);
PUBLIC_API int SocketOptionSetLargeRCVBUF (int sock_fd);
PUBLIC_API int SocketOptionSetLargeSNDBUF (int sock_fd);
PUBLIC_API int SocketOptionSetREUSEPORT (int sock_fd, int reuse);
PUBLIC_API int  SocketOptionSetREUSEADDR(int sock_fd, int reuse);
PUBLIC_API int SocketOptionSetIP_PKTINFO(int sock_fd, int on);
PUBLIC_API unsigned long HashNetAddress(const struct sockaddr_in *address);
PUBLIC_API unsigned long HashTwoWayNetAddress(const struct sockaddr_in *address_src, const struct sockaddr_in *address_dst);
HostNameDescriptor *GetSocketAddress (int socket_fd, HostNameDescriptor *hostname_provided);

#define HAVE_HSTRERROR 	1
#if !(HAVE_HSTRERROR)
 char *h_strerror (int);
# define hstrerror h_strerror
#endif
 PUBLIC_API unsigned long atoul_ (char *) __attribute__ ((deprecated("use strtoul instead")));

 #endif
