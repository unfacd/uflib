/**
 * Copyright (C) 2015-2026 unfacd works
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/net/utils_net.h>
#include <uflib/net/network_socket_address.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <sys/un.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/tcp.h>

//to use with setsockopt SO_RCVBUF and SO_SNDBUF
#ifdef __gnu_linux__
#define _CONFIG_LARGE_SOCK_SIZE 33554431
#else
#define _CONFIG_LARGE_SOCK_SIZE 4096
#endif

static inline int _SetupListeningSocketForTcp(NetworkSocketAddress *socket_address_ptr, const char *address, unsigned port,unsigned sock_opts);
static inline int _SetupListeningSocketForUdp(NetworkSocketAddress *socket_address_ptr, const char *address, unsigned port,unsigned sock_opts);
static inline int _InvokeListenForTcp(int sock_fd, int backlog);
static int _GetSocketFd(unsigned sock_type, unsigned sock_opts);

#define mstrncpy(s, p, n) \
   strncpy (s, p, strlen(p)>n-1 ? n : strlen(p) + 1); \
   s[n - 1] = '\0'

/**
 * 	@brief: Acquire network socket based on provided attributes
 */
__unused static int
_GetSocketFd(unsigned sock_type, unsigned sock_opts)
{
	int socket_fd	=	0;

	switch (sock_type)
	{
		case SOCK_TCP:
			if (sock_opts&SOCKOPT_IP4) {
				if ((socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
					syslog (LOG_ERR, "%s (errno:'%d'): ERROR: COULD NOT GET IP4 TCP SOCKET", __func__, errno);
					socket_fd = 0;
				}
			} else	if (sock_opts&SOCKOPT_IP6) {
				if ((socket_fd = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP)) < 0) {
					syslog (LOG_ERR, "%s (errno:'%d'): ERROR: COULD NOT GET IP6 TCP SOCKET", __func__, errno);
					socket_fd = 0;
				}
			}
			break;

		case SOCK_UDP:
			if (sock_opts&SOCKOPT_IP4) {
				if ((socket_fd = socket(PF_INET, SOCK_DGRAM,  IPPROTO_UDP)) < 0) {
					syslog (LOG_ERR, "%s (errno:'%d'): ERROR: COULD NOT GET IP4 UDP SOCKET", __func__, errno);
					socket_fd = 0;
				}
			} else	if (sock_opts&SOCKOPT_IP6) {
				if ((socket_fd = socket(PF_INET6, SOCK_DGRAM,  IPPROTO_UDP)) < 0) {
					syslog (LOG_ERR, "%s (errno:'%d'): ERROR: COULD NOT GET IP6 UDP SOCKET", __func__, errno);
					socket_fd = 0;
				}
			}
			break;
	}

	return socket_fd;
}

/**
 * 	@brief: return opened socket
 * 	@return -1 on error
 */
static inline int
_SetupListeningSocketForTcp(NetworkSocketAddress *socket_address_ptr, const char *address, unsigned port,unsigned sock_opts)
{
	char 		service[6] 							= "0";
	struct 	addrinfo 	addrinfo_hints,
										*addrinfo_res = NULL;

	addrinfo_hints.ai_family   = PF_UNSPEC;
	addrinfo_hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	addrinfo_hints.ai_socktype = SOCK_STREAM;
	addrinfo_hints.ai_protocol = IPPROTO_TCP;

	snprintf(service, sizeof(service), "%u", port);

	int error;
	if ((error = getaddrinfo((IS_PRESENT(address)? address : NULL), service, &addrinfo_hints, &addrinfo_res)) != 0) {
		syslog(LOG_WARNING, "%s (errno:'%d', error:'%s'): ERROR: COULD NOT INVOKE getaddrinfo for '%s'...", __func__, errno, address, gai_strerror(error));
		return -1;
	}

	int 						sock_fd = -1;
	struct addrinfo *r;

	for (r=addrinfo_res; IS_PRESENT(r); r=r->ai_next) {
		if ((sock_fd = socket(r->ai_family, SOCK_STREAM, IPPROTO_TCP)) == -1) {
			syslog(LOG_WARNING, "%s (errno:'%d'): ERROR COULD NOT GET listening socket...", __func__, errno);
			continue;
		}

		if ((error = bind(sock_fd, r->ai_addr, r->ai_addrlen)) == -1) {
			syslog(LOG_WARNING, "%s (fd:'%d', errno:'%d'): ERROR: COULD NOT BIND SOCKET for '%s': trying next (if any)...", __func__, sock_fd, errno, address);
			close(sock_fd);
			continue;
		}

		if (sock_opts&SOCKOPT_REUSEADDRE) {
      SocketOptionSetREUSEADDR(sock_fd, 1);
      SocketOptionSetREUSEPORT(sock_fd, 1);
      //fallthrough below on failure; this not fatal
		}

		if (sock_opts&SOCKOPT_LINGER)	(void)SocketOptionSetLINGER(sock_fd);

		break;
	}

	freeaddrinfo(addrinfo_res);

	if (IS_EMPTY(r))	return -1;

	return sock_fd;

}

/**
 * 	@return on error: -1
 * 	@return on success: live socket number
 */
static inline int
_InvokeListenForTcp(int sock_fd, int backlog)
{
	int error;

	if ((error=listen(sock_fd, backlog)) < 0)
	{
		syslog(LOG_WARNING, "%s (fd:'%d', errno:'%d'): ERROR: COULD NOT LISTEN FOR SOCKET...", __func__, sock_fd, errno);
		return error;
	}

	return sock_fd;
}

static inline int
_SetupListeningSocketForUdp(NetworkSocketAddress *socket_address_ptr, const char *address, unsigned port, unsigned sock_opts)
{
	int			address_family;
	char 		service[6] 							= "0";
	struct 	addrinfo 	addrinfo_hints,
										*addrinfo_res = NULL;


	addrinfo_hints.ai_family   = NetworkSocketAddressGetAddressFamily(socket_address_ptr);
	addrinfo_hints.ai_flags    = AI_PASSIVE | AI_NUMERICHOST;
	addrinfo_hints.ai_socktype = SOCK_DGRAM;
	addrinfo_hints.ai_protocol = IPPROTO_UDP;

	snprintf(service, sizeof(service), "%u", port);

	int error;
	if ((error = getaddrinfo((IS_PRESENT(address)?address:NULL), service, &addrinfo_hints, &addrinfo_res)) != 0) {
		syslog(LOG_WARNING, "%s (errno:'%d', error:'%s'): ERROR: COULD NOT INVOKE getaddrinfo for '%s'...", __func__, errno, address, gai_strerror(error));
		return -1;
	}

	int 						sock_fd = -1;
	struct addrinfo *r;

	for (r=addrinfo_res; IS_PRESENT(r); r=r->ai_next) {

		if ((sock_fd = socket(r->ai_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
			syslog(LOG_WARNING, "%s (errno:'%d'): ERROR COULD NOT GET listening socket...", __func__, errno);
			continue;
		}

		if (!(sock_opts&SOCKOPT_BLOCKING))	SetSocketFlags(sock_fd, 1, O_NONBLOCK);

		//this must happen before bind https://lwn.net/Articles/542629/
		if (sock_opts&SOCKOPT_REUSEPORT)	SocketOptionSetREUSEPORT(sock_fd, 1);
		if (sock_opts&SOCKOPT_REUSEADDRE)	SocketOptionSetREUSEADDR(sock_fd, 1);
		SocketOptionSetLargeRCVBUF(sock_fd);

		if ((error = bind(sock_fd, r->ai_addr, r->ai_addrlen)) == -1) {
			syslog(LOG_WARNING, "%s (fd:'%d', errno:'%d'): ERROR: COULD NOT BIND SOCKET for '%s': trying next (if any)...", __func__, sock_fd, errno, address);
			close(sock_fd);
			continue;
		}

		/* Can we do both IPv4 and IPv6 on same socket? */
		if (AF_INET6 == r->ai_family) {
			NetworkSocketAddress socket_address	=	{0};
			int on = 1;  // assuming v6only

#if defined (IPPROTO_IPV6) && defined (IPV6_V6ONLY)
			socklen_t on_len = sizeof(on);
			if (0 != getsockopt(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY,	(char *)&on, &on_len))	on =1;
#endif

			// Extra check for unspec addr - MAC OS X/Solaris
			if ((0 == NetworkSocketAddressSetInet6FromSockaddr(&socket_address, r->ai_addr)) &&
					NetworkSocketAddressIsAddressUnspecified(&socket_address))	on = 1;

			syslog (LOG_INFO, "%s (sock_fd:'%d'):  IPV6_V6ONLY is %d\n", __func__, sock_fd, on);

			//TODO we need to have two sockets
//			if (on) {
//				us->fd6 = fd;
//				continue;
//			}
		}

		break;
	}

	freeaddrinfo(addrinfo_res);

	if (IS_EMPTY(r))	return -1;

	return sock_fd;

}

/**
 * 	@return: -1 on error
 */
int
SetupListeningSocket(const char *ip, unsigned port, unsigned sock_type, unsigned sock_opts)
{
	int 									sock_fd			=	-1;
	NetworkSocketAddress	socket_address	=	{0};
	struct sockaddr_in 		telnet					=	{0};
	struct in_addr 				ina;

	struct addrinfo hints, *res = NULL, *r;

	//TODO: check ip for ip4 vs ip6 and set fa attribute in SocketAddress
	NetworkSocketAddressInstantiate(&socket_address, ip, port);

	switch (sock_type) {
		case SOCK_TCP:
			if ((sock_fd = _SetupListeningSocketForTcp(&socket_address, ip, port, sock_opts)) > 0) {
				if ((_InvokeListenForTcp(sock_fd, 1/*backlog*/)) > 0) goto exit;
			}

			close(sock_fd);
			sock_fd	= -1;
			break;

		case SOCK_UDP:
			sock_fd = _SetupListeningSocketForUdp(&socket_address, ip, port, sock_opts);
			break;
	}

	exit:
	return sock_fd;

}  /**/

#if 0
/**
 * Get the IP address of the host
 *
 * @param af  Address Family
 * @param ip  Returned IP address
 *
 * @return 0 if success, otherwise errorcode
 */
int GetMyHostName (int af, NetworkSocketAddress *socket_address_ptr)
{
	char hostname[256];
	struct in_addr in;
	struct hostent *he;

	if (-1 == gethostname(hostname, sizeof(hostname)))
		return errno;

	he = gethostbyname(hostname);
	if (!he)
		return ENOENT;

	if (af != he->h_addrtype)
		return EAFNOSUPPORT;

	/* Get the first entry */
	memcpy(&in, he->h_addr_list[0], sizeof(in));
	NetworkSocketAddressSetInet4FromHost(socket_address_ptr, ntohl(in.s_addr), 0);

	return 0;
}
#endif

 /*
 ** If socket returned is < 0, then we know the connection is still
 ** in progress, i.e., it hasn't been accepted straight away...
 ** We need to persue that in our select function.
 **
 ** dsptr->haddress: dest_host
 ** dsptr->hport: dest_port
 ** dsptr->address: localhost
 ** dsptr->port: our port to that connection
 **
 ** ssptr->haddress: connecting host
 ** ssptr->hport: hist port
 ** ssptr->address: localhost
 ** ssptr->port: our port on which he's connected
 */
 int
 ConnectToServer(const char *server, unsigned long port, Socket *sptr)
 {
  unsigned int len;
  int    nsocket;
  ResolvedAddress raddr;
  //struct sockaddr_in me;

  struct sockaddr_storage address_peer;
	socklen_t               address_len_peer = sizeof(address_peer);

#if 0
	//or alternatively using NetworkSocketAddress
	NetworkSocketAddress socket_address_peer;
	InitNetworkSocketAddress (&socket_address_peer, AF_UNSPEC);
	getsockname(abs(nsocket), &socket_address_peer.u.sa, &socket_address_peer.len);
#endif

   memset(&raddr, 0, sizeof(ResolvedAddress));

//   say ("*** Resolving server %s...\n", server);

    if (!(ResolveAddress(server, &raddr)))  return 0;

//   say ("*** Negotiating connection to %s (%s) on port %lu...\n", raddr.dns, raddr.dotted, port);

    if (!(nsocket = Connect(&raddr.inetaddr, port, false)))  return 0;

    if ((getsockname(abs(nsocket), (struct sockaddr *)&address_peer, (socklen_t *)&address_len_peer)) < 0)     {
      syslog (LOG_INFO, "'getsockname' failed on socket %d", nsocket);

      close(abs(nsocket));

      return 0;
     }

    if (sptr) {
    	char port_numeric[6] = {0};
			getnameinfo((struct sockaddr *)&address_peer, address_len_peer,	NULL, 0, port_numeric, sizeof(port_numeric),	NI_NUMERICHOST | NI_NUMERICSERV);

      //sptr->when=time(NULL);
      sptr->hport = port;
      sptr->port = atoi(port_numeric);//ntohs(me.sin_port);
      sptr->sock = abs(nsocket);
      strcpy(sptr->haddress, server);
      strcpy(sptr->address, "localhost");

       //if (nsocket<0)  sptr->flag|=BLOCKING;  /* in fact non-blocking...*/
     }

   return nsocket;

 }  /**/

 /**
  * 	IP4 only de to dns resolution method
  */
  //temporarily disabled until Session dependency is resolved
 /*int
 ConnectToServerSecure(const char *server, unsigned long port, SSL_CTX *ctx, Session *sesn_ptr)
{
	unsigned int len;
	int   nsocket;
	ResolvedAddress raddr;
	Socket *sptr;
	struct sockaddr_storage address_peer;
	socklen_t               address_len_peer = sizeof(address_peer);

	memset (&raddr, 0, sizeof(ResolvedAddress));

//	say ("*** Resolving server %s...\n", server);

	if (!(ResolveAddress(server, &raddr)))  return 0;

//	say ("*** Negotiating connection to %s (%s) on port %lu...\n",raddr.dns, raddr.dotted, port);

    if (!(nsocket = Connect(&raddr.inetaddr, port, false*//*nonblocking_flag*//*)))  return 0;

	//attach  SSL session to the now connected socket
    sesn_ptr->session_crypto.ssl=SSL_new(ctx);
	int rc = SSL_set_fd(sesn_ptr->session_crypto.ssl, abs(nsocket));
	if (rc <= 0) {
		fprintf(stderr, "Error: SSL set_fd failed..\n");
		SSL_free(sesn_ptr->session_crypto.ssl);
	}

	if (SSL_connect(sesn_ptr->session_crypto.ssl) != 1) {
	  fprintf(stderr, "Error: Could not build a SSL session to server\n");

	  return 0;
	}

	printf("*** Connected with %s encryption\n", SSL_get_cipher(sesn_ptr->session_crypto.ssl));

	if ((getsockname(abs(nsocket), (struct sockaddr *)&address_peer, (socklen_t *)&address_len_peer)) < 0) {
		syslog (LOG_INFO, "'getsockname' failed on socket %d", nsocket);

		close(abs(nsocket));

		return 0;
	}

	sptr = sesn_ptr->ssptr;
	{
		char port_numeric[6] = {0};
		getnameinfo((struct sockaddr *)&address_peer, address_len_peer,	NULL, 0, port_numeric, sizeof(port_numeric),	NI_NUMERICHOST | NI_NUMERICSERV);

		//sptr->when=time(NULL);
		sptr->hport = port;
		sptr->port = atoi(port_numeric);//ntohs(me.sin_port);
		sptr->sock = abs(nsocket);
		strcpy(sptr->haddress, server);
		strcpy(sptr->address, "localhost");

	//if (nsocket<0)  sptr->flag|=BLOCKING;  //in fact non-blocking...
	}

#if 0
	{
		//Get the remote certificate into the X509 structure         *
		X509 *cert = SSL_get_peer_certificate(sesn_ptr->session_crypto.ssl);
		if (cert == NULL)
			fprintf(stderr, "Error: Could not get a certificate\n");

		//extract various certificate information                    *
		X509_NAME *certname = X509_NAME_new();
		certname = X509_get_subject_name(cert);

		X509_free(cert);
	}
#endif

	SESNSTATUS_SET(sesn_ptr->stat, SESNSTATUS_SECURE);

    return nsocket;

  }

  */

 /*
 ** Caller should allocate radrr 
 ** NOTE: THIS IS IP4 only
 */ 
 int
 ResolveAddress(const char *address, ResolvedAddress *raddr)
 {
  struct hostent *hp;
  struct in_addr saddr;

    if (IS_EMPTY((hp = gethostbyname(address)))) {
      syslog (LOG_INFO, "Unable to resolve %s - %s.\n", address, hstrerror(h_errno));

      return 0;
     }

   memcpy(&saddr, hp->h_addr, sizeof(saddr));
   mstrncpy(raddr->dns, hp->h_name, MAXHOSTLEN);
   mstrncpy(raddr->dotted, inet_ntoa(saddr), MAXHOSTLEN);
   memcpy(&raddr->inetaddr, (void *)hp->h_addr, sizeof(saddr));

   return 1;
 }  /**/

 /* Resolves the  "host" and set the string representation of the IP address into the buffer pointed by "ipbuf".
  *
  * If flags is set to ANET_IP_ONLY the function only resolves hostnames
  * that are actually already IPv4 or IPv6 addresses. This turns the function
  * into a validating / normalizing function. */
 int
 GenericDnsResolve(const char *host, char *ipbuf, size_t ipbuf_len)
 {
     struct addrinfo hints, *info;
     int rv;

     memset(&hints,0, sizeof(hints));

     if (isdottedquad(host)) hints.ai_flags = AI_NUMERICHOST;
     hints.ai_family = AF_UNSPEC;
     hints.ai_socktype = SOCK_STREAM;  /* specify socktype to avoid dups */

     if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
    	 syslog (LOG_NOTICE, "%s: ERROR: UNABLE TO RESOLVE: '%s' '%s'", __func__, host, gai_strerror(rv));

         return -1;
     }

     if (info->ai_family == AF_INET) {
         struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
         inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
     } else {
         struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
         inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
     }

     freeaddrinfo(info);

     return 0;
 }

 /*
  *  @return 0: on success
  *  @return >0: system errors
  */
 int
 IsSocketAlive(int socket)
 {
	 int s_error = 0;
	 socklen_t len = sizeof(s_error);

	 int retval = getsockopt(socket, SOL_SOCKET, SO_ERROR, &s_error, &len);

	 if (retval != 0) {
	     /* there was a problem getting the error code */
	     //fprintf(stderr, "error getting socket error code: %s\n", strerror(retval));
	     return retval;
	 }

	 if (s_error != 0) {
	     /* socket has a non zero error status */
	     //fprintf(stderr, "socket error: %s\n", strerror(error));
	     return s_error;
	 }

	 return 0;//we are good
 }

 /**
  * 	@brief: standard tcp/ip connection. By default perform a synchronous connect, unless
  * 	nonblock_flag is set.
  * 	NOTE: THIS IS IP$ AWARE ONLY
  */
 int
 Connect(struct in_addr *addr, unsigned long port, bool nonblock_flag)
 {
  int sock, 
      connected;
  struct sockaddr_in address;

   memset((char *)&address, 0, sizeof(address));

   address.sin_family = AF_INET;
   address.sin_port   = htons(port);
   address.sin_addr.s_addr = addr->s_addr;
   
    if (!(sock = RequestTCPSocket())) {
      syslog (LOG_INFO, "*** Unable to obtain socket -  %s.\n", strerror(errno));

      return 0;
     }

    if (nonblock_flag)	if (!(SetSocketFlags(sock, 1, O_NONBLOCK)))  return 0;
       
    connected = connect(sock, (struct sockaddr *)&address, sizeof(address));

	switch (connected)
	{
		case 0:
		//SetSocketFlags (sock, 0, O_NONBLOCK);
		return sock;//success

		default:
			if (nonblock_flag && errno == EINPROGRESS) {
//				say ("*** ASYNC CONNECT MODE ON...");
				return (-sock);  /* blocking in progress */
			}

			if (errno == EALREADY) {
//				say ("*** THIS CONNECTION IS IN PROGRESS");
				return 0;
			}

	}

	return 0;

 }  /**/

 /**
  * 	IP4 only
  */
 int
 RequestTCPSocket(void)
 {
  int nsocket;
  
    if ((nsocket = socket(AF_INET, SOCK_STREAM, 0))<0)  return 0;

   return nsocket;

 }  /**/

 int
 SocketOptionSetLINGER(int sock_fd)
 {
 	const struct linger dl = {0, 0};

 	//err = setsockopt(fd, SOL_SOCKET, SO_LINGER,  &dl, sizeof(dl));
 	if (setsockopt(sock_fd, SOL_SOCKET, SO_LINGER, &dl, sizeof(dl)) == -1) {
 			syslog (LOG_ERR, "%s (sock_fd:'%d'): Failed to to set", __func__, sock_fd);
 			return -1;
	}

 	return 0;
 }

 int
 SetSocketFlags(int socket, int ON_OFF, int flags)
 {
  int cur;

     if ((cur = fcntl(socket, F_GETFL, 0)) == -1) {
       syslog (LOG_INFO, "*** Unable to F_GETFL socket - %s.\n", strerror(errno));

       return 0;
      }

   switch (ON_OFF) {
      case 1:
       cur |= flags;
       break;

      case 0:
       cur &= ~flags;
       break;
      }
    
    if ((fcntl(socket, F_SETFL, cur)) == -1) {
      syslog (LOG_INFO, "*** Unable to set socket flags - %s.\n", strerror(errno));

      return 0;
     }

   return 1;

 }  /**/

 /**
  * @brief Provide finer control over socket keepalive semantics
  * @param nsocket Socket to be configured
  * @param interval seconds
  * @return 0 on error
  */
int ConfigureSocketKeepAliveOptions(int nsocket, int interval)
{
  int val = interval;//seconds
  if (setsockopt(nsocket, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) { // Start keeplives after this period
    syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set TCP_KEEPIDLE", __func__, nsocket, strerror(errno));

    return 0;
  }

  val = interval / 3;
  if (val == 0) val = 1;
  if (setsockopt(nsocket, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) { //Interval between keepalives
    syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set TCP_KEEPINTVL", __func__, nsocket, strerror(errno));
    return 0;
  }

  val = 3;
  if (setsockopt(nsocket, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) { //Number of keepalives before death
    syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set TCP_KEEPCNT", __func__, nsocket, strerror(errno));
    return 0;
  }

  return 1;
}

int
SocketOptionSetLargeRCVBUF(int sock_fd)
{
	int block_sz = _CONFIG_LARGE_SOCK_SIZE;

	if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &block_sz, (socklen_t)sizeof(block_sz)) == -1) {
    syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set SO_RCVBUF", __func__, sock_fd, strerror(errno));
		return -1;
	}

	return 0;
}

int
SocketOptionSetLargeSNDBUF(int sock_fd)
{
	int block_sz = _CONFIG_LARGE_SOCK_SIZE;

	if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &block_sz, (socklen_t)sizeof(block_sz)) == -1) {
    syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set SO_SNDBUF", __func__, sock_fd, strerror(errno));
		return -1;
	}

	return 0;

}

int
SocketOptionSetREUSEPORT(int sock_fd, int reuse)
{
#ifdef SO_REUSEPORT
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) == -1) {
      syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set SO_REUSEPORT", __func__, sock_fd, strerror(errno));
			return -1;
		}
#endif

    return 0;
}

int
SocketOptionSetREUSEADDR(int sock_fd, int reuse)
{
	if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
		syslog (LOG_ERR, "%s (sock_fd:'%d', error:'%s'): Failed to to set SO_REUSEADDR", __func__, sock_fd, strerror(errno));
		return -1;
	}

	return 0;
}

unsigned long
HashNetAddress(const struct sockaddr_in *address)
{
#if UF_DEBUG_BUILD
  char  peer_addr_str[NI_MAXHOST] = {0},
        peer_service[NI_MAXSERV] = {0};
  int err = getnameinfo((struct sockaddr *)address, sizeof(*address), peer_addr_str, sizeof(peer_addr_str), peer_service, sizeof(peer_service), 0);
  if (err != 0) {
    fprintf(stderr, "%s: ERROR using getnameinfo: %s\n", __func__, gai_strerror(err));
  }
  syslog(LOG_DEBUG, "%s: Hashing: '%s' (service:'%s')", __func__, peer_addr_str, peer_service);
#endif
  return ((size_t)(address->sin_addr.s_addr) * 59) ^
         ((size_t)(address->sin_port) << 16) ^
         ((size_t)(address->sin_family));
}

HostNameDescriptor *
GetSocketAddress(int socket_fd, HostNameDescriptor *hostname_provided)
{
  struct sockaddr_in local_sin;
  HostNameDescriptor *hostname;

  if (IS_EMPTY(hostname_provided)) {
    hostname = calloc(1, sizeof(HostNameDescriptor) + HOSTNAME_MAXSIZE + 1);
    hostname->name = (char *)hostname + sizeof(HostNameDescriptor);
  }
  else hostname = hostname_provided;

  socklen_t local_sinlen = sizeof(local_sin);
  if (getsockname(socket_fd, (struct sockaddr *)&local_sin, &local_sinlen) == 0) {
    char service[NI_MAXSERV] = {0};
    int err = getnameinfo((struct sockaddr *)&local_sin, sizeof(struct sockaddr), hostname->name, HOSTNAME_MAXSIZE, service, sizeof(service), 0);
    if (err != 0) {
      fprintf(stderr, "%s: ERROR using getnameinfo (socket:'%d'): %s\n", __func__, socket_fd, gai_strerror(err));
      goto exit_error;
    }

    hostname->port = atoi(service);
    return hostname;
  } else {
    syslog(LOG_ERR, "%s: (socket:'%d', errno:'%d'): ERROR GETTING SOCKET ADDRESS: '%s'", __func__, socket_fd, errno, strerror(errno));
  }

  exit_error:
  if (IS_EMPTY(hostname_provided))  free(hostname);
  return NULL;
}

unsigned long
HashTwoWayNetAddress(const struct sockaddr_in *address_src, const struct sockaddr_in *address_dst)
{
  return ((size_t)(address_src->sin_addr.s_addr) * 59) ^
         ((size_t)(address_dst->sin_addr.s_addr)) ^
         ((size_t)(address_src->sin_port) << 16) ^
         ((size_t)(address_dst->sin_port)) ^
         ((size_t)(address_src->sin_family));
}

//http://stackoverflow.com/questions/3062205/setting-the-source-ip-for-a-udp-socket
int
SocketOptionSetIP_PKTINFO(int sock_fd, int on)
{
	int one = setsockopt(sock_fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on));
	int two = setsockopt(sock_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));

	if (one == -1 || two == -1) {
		syslog (LOG_ERR, "%s (sock_fd:'%d', one:'%d', two:'%d'): Failed to to set", __func__, sock_fd, one , two);
		return -1;
	}

	return 0;
}

 /*AddressToDotted*/
 char *
 RawIPToDotted(unsigned long raw)
 {
  static char rv[16];
  unsigned long ip;
 
   ip=htonl (raw);
 
   sprintf (rv, "%d.%d.%d.%d",
            (int)((ip>>0)&0xFF),
            (int)((ip>>8)&0xFF),
            (int)((ip>>16)&0xFF),
            (int)((ip>>24)&0xFF));

   return rv;

 }  /**/

 /**
  * 	IP4 only
  */
 char *
 HostToDotted(char *host)
 {
  unsigned long ip;
  struct hostent *hp;
  struct in_addr addr;
   
    if (isdottedquad(host))  return host;

   hp=gethostbyname(host);

    if (hp==(struct hostent *)NULL)  return ((char *)NULL);

  memcpy ((void *)&addr, (void *)hp->h_addr, sizeof(addr));

  ip=ntohl(addr.s_addr);

   if (!ip)  return ((char *)NULL);

  return (RawIPToDotted(ip));

 }  /**/

 /**
  * 	IP4 only
  */
 char *
 DottedToHost(char *dip)
 {
  struct sockaddr_in addr;
  struct hostent *hp;

   addr.sin_addr.s_addr=inet_addr(dip);

   hp=gethostbyaddr((char *)&addr.sin_addr, sizeof(addr.sin_addr), AF_INET);
    if (hp==(struct hostent *)NULL)  return ((char *)dip);

   return ((char *)hp->h_name);

 }  /**/

 /**
  * 	IP4 only. NON-REENTRANT
  */
 struct in_addr *
 NetworkToAddress(const char *name)
 {
  struct netent *net;
  static struct in_addr addr;

    if ((net = getnetbyname(name)) != NULL) {
       if (net->n_addrtype != AF_INET)  return (struct in_addr *)NULL;

      addr.s_addr = htonl((unsigned long)net->n_net);

      return &addr;
     } else {
      return (struct in_addr *)NULL;
     }
 
 }  /**/

 /**
  * 	IP4 only and NON-REENTRANT. Can use getnetbyaddr_r() for reentrant
  */
 char *
 AddressToNetwork(struct in_addr *addr)
 {
  struct netent *net;

    if ((net = getnetbyaddr((long)ntohl(addr->s_addr), AF_INET)) != NULL)
      return (char *)net->n_name;
    else
     return (char *)NULL;

 }  /**/

 /* ("irc", IPPROTO_TCP) --> 6667 */
 int
 ServiceToPort(const char *name, unsigned short proto)
 {
  struct servent *service;

    if ((proto==IPPROTO_TCP)&&
	((service=getservbyname(name, "tcp"))!=NULL))
     return ntohs((unsigned short) service->s_port);
    else 
    if ((proto==IPPROTO_UDP)&&
	((service=getservbyname(name, "udp"))!=NULL))
     return (ntohs((unsigned short)service->s_port));
    else
     return -1;

 }  /**/

 char *
 PortToService(int port, unsigned short proto)
 {
  struct servent *service;

    if ((proto==IPPROTO_TCP)&&
        ((service=getservbyport(htons(port), "tcp"))!=NULL))
     return service->s_name;
    else 
    if ((proto==IPPROTO_UDP)&&
        ((service=getservbyport(htons(port), "udp"))!=NULL))
     return service->s_name;
    else /*add more here*/
     return (char *)NULL;

 }  /**/

 /* (6) --> TCP */
 char *
 ProtocolToName(unsigned short proto)
 {
  /*const struct pprot *pp;*/

    if (proto) 
     {
      struct protoent *pent=getprotobynumber(proto);

   	    if (pent)  return pent->p_name;
     }

   return NULL;

 }  /**/

 int
 isdottedquad(const char *address)
 {
  register int n,
           numbered=1;

   n=strlen(address)-1;

    while ((address[n]!='.')&&(n))
    {
      if ((address[n]<'0')||(address[n]>'9'))
	   {
		numbered=0;
		break;
	   }

     n--;
    }

   return numbered;

 }  /**/

 /**
  * 	IP4 only
  */
 struct in_addr
	NetworkPrefixToAdress(unsigned int bits)
 {
  struct in_addr addr;
  unsigned long mask;

    memset (&addr, 0, sizeof(struct in_addr));

   if (bits==0)  mask=0;
   else 
    {
     /* set the 'mask_bits' most significant bits */
     mask=0xffffffffU;
     mask>>=(32-bits);
     mask<<=(32-bits);
    }

   mask=ntohl(mask);
   addr.s_addr=mask;

   return addr;

 }  /**/

 int
 IsLocalIP(const char *ip)
 {
  struct ifconf ifc;
  struct ifreq ifreq;
  struct in_addr ina,
                 tmpina;
  struct sockaddr sa __attribute__((unused));
  char *buf,
       *cp,
       *cplim;
  int bufsiz=4095,
      s, n, cpsize;

     if ((!ip)||(!*ip))  return -1;

     if (!inet_aton(ip, &tmpina))  return -2; /*illformed addr*/
  

    if ((s=socket(AF_INET, SOCK_DGRAM, 0))<0)  return -1;

    while (1!=2)
     {
      buf=malloc(bufsiz);
      ifc.ifc_len=bufsiz;
      ifc.ifc_buf=buf;

       if ((n=ioctl(s, SIOCGIFCONF, (char *)&ifc))!=-1)
        {
          if (ifc.ifc_len+2*sizeof(ifreq)<bufsiz)  break;  /*got it*/
        }

       if ((n==-1)&&(errno!=EINVAL))  return -1;

      free (buf);
      bufsiz+=4096;  /* try again*/
     }  /*while*/

   cplim=buf+ifc.ifc_len;
    for (cp=buf; cp<cplim; cp+=cpsize)
     {
      memcpy (&ifreq, cp, sizeof(ifreq));

#if HAVE_SA_LEN
      cpsize = sizeof ifreq;
       if (ifreq.ifr_addr.sa_len>sizeof(struct sockaddr))
        cpsize+=(int)ifreq.ifr_addr.sa_len-(int)(sizeof(struct sockaddr));
#else
      cpsize=sizeof(ifreq);
#endif

       if (ifreq.ifr_addr.sa_family!=AF_INET)
        {
         /*printf ("ERR: getnetconf: %s AF %d != INET\n",
                 ifreq.ifr_name, ifreq.ifr_addr.sa_family);*/
         continue;
        }

      sa=ifreq.ifr_addr;
      ina=((struct sockaddr_in *)&ifreq.ifr_addr)->sin_addr;

      /*printf ("*** getnetconf: considering %s [%s]\n",
              ifreq.ifr_name, inet_ntoa(ina));*/

       if (ina.s_addr==tmpina.s_addr)  return 1;
     } /*for*/

   close (s);

   free (buf);

   return 0;

 }  /**/


 unsigned long
 atoul_(char *addr)
 {
  unsigned long ip=0L;

   if (!addr)  return 0L;

    while ((*addr>='0')&&(*addr<='9'))
     {
      ip=(ip*10)+(*addr++)-'0';
     }

   return ip;

 }  /**/

 /* Return true is STRING (case-insensitively) matches PATTERN, false
    otherwise.  The recognized wildcard character is "*", which matches
    any character in STRING except ".".  Any number of the "*" wildcard
    may be present in the pattern.
    This is used to match of hosts as indicated in rfc2818: "Names may
    contain the wildcard character * which is considered to match any
    single domain name component or component fragment. E.g., *.a.com
    matches foo.a.com but not bar.foo.a.com. f*.com matches foo.com but
    not bar.com [or foo.bar.com]."
    If the pattern contain no wildcards, pattern_match(a, b) is
    equivalent to !strcasecmp(a, b).  */
 static bool pattern_match(const char *pattern, const char *string)__attribute__((unused));
 static bool
 pattern_match(const char *pattern, const char *string)
 {
#if 0
	 //sourced from wget https://github.com/mirror/wget/blob/master/src/openssl.c
   const char *p = pattern, *n = string;
   char c;
   for (; (c = c_tolower (*p++)) != '\0'; n++)
     if (c == '*')
       {
         for (c = c_tolower (*p); c == '*'; c = c_tolower (*++p))
           ;
         for (; *n != '\0'; n++)
           if (c_tolower (*n) == c && pattern_match (p, n))
             return true;
 #ifdef ASTERISK_EXCLUDES_DOT
           else if (*n == '.')
             return false;
 #endif
         return c == '\0';
       }
     else
       {
         if (c != c_tolower (*n))
           return false;
       }
   return *n == '\0';
#endif

   return false;
 }

#if 0
 void url_to_inaddr2(struct sockaddr_in *addr, const char *url, int port)
 {
 	memset(addr, 0x0, sizeof(struct sockaddr_in));

 	if (url) {
 		struct addrinfo hints;
 		struct addrinfo *result, *rp;

 		memset(&hints, 0, sizeof(struct addrinfo));
 		hints.ai_family = AF_INET;

 		if (getaddrinfo(url, NULL, &hints, &result) != 0)
 			die("failed to resolve address '%s'", url);

 		/* Look for the first IPv4 address we can find */
 		for (rp = result; rp; rp = rp->ai_next) {
 			if (result->ai_family == AF_INET &&
 				result->ai_addrlen == sizeof(struct sockaddr_in))
 				break;
 		}

 		if (!rp)
 			die("address format not supported");

 		memcpy(addr, rp->ai_addr, rp->ai_addrlen);
 		addr->sin_port = htons(port);

 		freeaddrinfo(result);
 	} else {
 		addr->sin_family = AF_INET;
 		addr->sin_port = htons(port);
 		addr->sin_addr.s_addr = htonl(INADDR_ANY);
 	}
 }
#endif

#if !(HAVE_HSTRERROR) 
 static char *_h_errlist[]={
    "You should be fine",
    "Host not found",
    "Host name lookup failure",
    "Unknown server error",
    "No address associated with name",
    "Service unavailable",
};

static int _h_nerr = sizeof(_h_errlist)/sizeof(_h_errlist[0]);

 char *h_strerror (int error)

 {
  static char aux[35];

    if ((error<0)||(error>_h_nerr)) 
       {
        sprintf (aux, "Unknown resolver error");

        return (char *)aux;
       }

   return (char *)_h_errlist[error];

 }  /**/
#endif
