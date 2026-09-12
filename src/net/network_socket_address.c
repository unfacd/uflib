/*
 * network_socket_address.c
 *
 *  Created on: 5 Feb 2017
 *      Author: ayman
 */

#include <uflib/standard_c_includes.h>
#include <uflib/standard_defs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <sys/un.h>
#include <uflib/net/network_socket_address.h>

/**
 * Initialize a Socket Address
 *
 * @param sa Socket Address
 * @param af Address Family
 */
void
InitNetworkSocketAddress(NetworkSocketAddress *socket_address_ptr, int af)
{
	memset(socket_address_ptr, 0, sizeof(*socket_address_ptr));
	socket_address_ptr->u.sa.sa_family = af;
	socket_address_ptr->len = sizeof(socket_address_ptr->u);

}

#if 0
/**
 * Set a Socket Address from a PL string
 *
 * @param sa   Socket Address
 * @param addr IP-address
 * @param port Port number
 *
 * @return 0 if success, otherwise errorcode
 */
int sa_set(struct sa *sa, const struct pl *addr, uint16_t port)
{
	char buf[64];

	(void)pl_strcpy(addr, buf, sizeof(buf));
	return sa_set_str(sa, buf, port);
}
#endif

/**
 * Set a Socket Address from a string
 *
 * @param sa   Socket Address
 * @param addr IP-address
 * @param port Port number
 *
 * @return 0 if success, otherwise errorcode
 */
//int sa_set_str(struct sa *sa, const char *addr, uint16_t port)
int
NetworkSocketAddressInstantiate(NetworkSocketAddress *socket_address_ptr, const char *addr, uint16_t port)
{
	int err;

	err = ConvertSocketAddressToNetworkFormat(addr, socket_address_ptr);

	if (err)	return err;

	switch (socket_address_ptr->u.sa.sa_family)
	{

		case AF_INET:
			socket_address_ptr->u.in.sin_port = htons(port);
			socket_address_ptr->len = sizeof(struct sockaddr_in);
			break;

		case AF_INET6:
			socket_address_ptr->u.in6.sin6_port = htons(port);
			socket_address_ptr->len = sizeof(struct sockaddr_in6);
			break;

		default:
			return EAFNOSUPPORT;
	}

	return 0;
}

/**
 * Set a Socket Address from an IPv4 address
 *
 * @param sa   Socket Address
 * @param addr IPv4 address in host order
 * @param port Port number
 *
 * @return 0 if success, otherwise errorcode
 */
//void sa_set_in(struct sa *sa, uint32_t addr, uint16_t port)
void NetworkSocketAddressSetInet4FromHost(NetworkSocketAddress *sa, uint32_t addr, uint16_t port)
{
	if (!sa)
		return;

	sa->u.in.sin_family = AF_INET;
	sa->u.in.sin_addr.s_addr = htonl(addr);
	sa->u.in.sin_port = htons(port);
	sa->len = sizeof(struct sockaddr_in);
}

/**
 * Set a Socket Address from an IPv6 address
 *
 * @param sa   Socket Address
 * @param addr IPv6 address
 * @param port Port number
 *
 * @return 0 if success, otherwise errorcode
 */
//void sa_set_in6(struct sa *sa, const uint8_t *addr, uint16_t port)
void NetworkSocketAddressSetInet6FromHost (NetworkSocketAddress *sa, const uint8_t *addr, uint16_t port)
{
	if (!sa)
		return;

#if 1
	sa->u.in6.sin6_family = AF_INET6;
	memcpy(&sa->u.in6.sin6_addr, addr, 16);
	sa->u.in6.sin6_port = htons(port);
	sa->len = sizeof(struct sockaddr_in6);
#else
	(void)addr;
	(void)port;
#endif
}

/**
 * @brief: retrieve the local end of the address for a connected socket
 * 	@return success: 0
 */
int
NetworkSocketAddressSetLocalFromFd (int sock_fd, NetworkSocketAddress *local)
{
	local->len = sizeof(local->u);

	if (0 == getsockname(sock_fd, &local->u.sa, &local->len))	return 0;

	return errno;
}

/**
 * 	@brief: Given a connected socket, retrive the reamote end address into pre alocated NetworkSocketAddress
 * 	@return error: >0
 */
int
NetworkSocketAddressSetPeerFromFd (int sock_fd, NetworkSocketAddress *socket_address_peer)
{
	InitNetworkSocketAddress(socket_address_peer, AF_UNSPEC);

	if (getpeername(sock_fd, &socket_address_peer->u.sa, &socket_address_peer->len) < 0) {
		syslog (LOG_ERR, "%s (errno:'%d'): ERROR: \n", __func__, errno);
		return errno;
	}

	return 0;
}

/**
 * Set a Socket Address from a sockaddr
 *
 * @param sa Socket Address
 * @param s  Sockaddr
 *
 * @return 0 if success, otherwise errorcode
 */
//int sa_set_sa(struct sa *sa, const struct sockaddr *s)
int NetworkSocketAddressSetInet6FromSockaddr(NetworkSocketAddress *sa, const struct sockaddr *s)
{
	if (!sa || !s)
		return EINVAL;

	switch (s->sa_family) {

	case AF_INET:
		memcpy(&sa->u.in, s, sizeof(struct sockaddr_in));
		sa->len = sizeof(struct sockaddr_in);
		break;

	case AF_INET6:
		memcpy(&sa->u.in6, s, sizeof(struct sockaddr_in6));
		sa->len = sizeof(struct sockaddr_in6);
		break;

	default:
		return EAFNOSUPPORT;
	}

	sa->u.sa.sa_family = s->sa_family;

	return 0;
}

/**
 * Set the port number on a Socket Address
 *
 * @param sa   Socket Address
 * @param port Port number
 */
//void sa_set_port(struct sa *sa, uint16_t port)
void NetworkSocketAddressSetPort(NetworkSocketAddress *sa, uint16_t port)
{
	if (!sa)
		return;

	switch (sa->u.sa.sa_family) {

	case AF_INET:
		sa->u.in.sin_port = htons(port);
		break;

	case AF_INET6:
		sa->u.in6.sin6_port = htons(port);
		break;

	default:
//		DEBUG_WARNING("sa_set_port: no af %d (port %u)\n",
	//		      sa->u.sa.sa_family, port);
		break;
	}
}

/**
 * Set a socket address from a string of type "address:port"
 * IPv6 addresses must be encapsulated in square brackets.
 *
 * @param sa   Socket Address
 * @param str  Address and port string
 * @param len  Length of string
 *
 * @return 0 if success, otherwise errorcode
 *
 * Example strings:
 *
 * <pre>
 *   1.2.3.4:1234
 *   [::1]:1234
 *   [::]:5060
 * </pre>
 */
int NetworkSocketAddressFromEncoded(NetworkSocketAddress *sa, const char *str, size_t len)
{
#if 0
	struct pl addr, port, pl;
	const char *c;

	if (!sa || !str || !len)
		return EINVAL;

	pl.p = str;
	pl.l = len;

	if ('[' == str[0] && (c = pl_strchr(&pl, ']'))) {
		addr.p = str + 1;
		addr.l = c - str - 1;
		++c;
	}
	else if (NULL != (c = pl_strchr(&pl, ':'))) {
		addr.p = str;
		addr.l = c - str;
	}
	else {
		return EINVAL;
	}

	if (len < (size_t)(c - str + 2))
		return EINVAL;

	if (':' != *c)
		return EINVAL;

	port.p = ++c;
	port.l = len + str - c;

	return sa_set(sa, &addr, pl_u32(&port));
#endif

	return 0;
}

/**
 * Get the Address Family of a Socket Address
 *
 * @param sa Socket Address
 *
 * @return Address Family
 */
//int sa_af(const struct sa *sa)
int NetworkSocketAddressGetAddressFamily (const NetworkSocketAddress *sa)
{
	return sa ? sa->u.sa.sa_family : AF_UNSPEC;
}

/**
 * Get the IPv4-address of a Socket Address
 *
 * @param sa Socket Address
 *
 * @return IPv4 address in host order
 */
//uint32_t sa_in(const struct sa *sa)
uint32_t NetworkSocketAddressGetInet4Address(const NetworkSocketAddress *sa)
{
	return sa ? ntohl(sa->u.in.sin_addr.s_addr) : 0;
}

/**
 * Get the IPv6-address of a Socket Address
 *
 * @param sa   Socket Address
 * @param addr On return, contains the IPv6-address
 */
//void sa_in6(const struct sa *sa, uint8_t *addr)
void NetworkSocketAddressGetInet6Address(const NetworkSocketAddress *sa, uint8_t *addr)
{
	if (!sa || !addr)
		return;

	memcpy(addr, &sa->u.in6.sin6_addr, 16);
}

/**
 * Convert a Socket Address to Presentation format
 *
 * @param sa   Socket Address
 * @param buf  Buffer to store presentation format
 * @param size Buffer size
 *
 * @return 0 if success, otherwise errorcode
 */
//int sa_ntop(const struct sa *sa, char *buf, int size)
int NetworSocketAddresssaToReadable (const NetworkSocketAddress *sa, char *buf, int size)
{
	//return net_inet_ntop(sa, buf, size);
	return (ConvertSocketAddressToReadableFormat (sa, buf, size));
}

/**
 * Get the port number from a Socket Address
 *
 * @param sa Socket Address
 *
 * @return Port number  in host order
 */
//uint16_t sa_port(const struct sa *sa)
uint16_t NetworkSocketAddressGetPort (const NetworkSocketAddress *sa)
{
	if (!sa)
		return 0;

	switch (sa->u.sa.sa_family)
	{
		case AF_INET:
			return ntohs(sa->u.in.sin_port);

		case AF_INET6:
			return ntohs(sa->u.in6.sin6_port);

		default:
			return 0;
	}
}

/**
 * Check if a Socket Address is set
 *
 * @param sa   Socket Address
 * @param flag Flags specifying which fields to check
 *
 * @return true if set, false if not set
 */
//bool sa_isset(const struct sa *sa, int flag)
bool SocketAddressIsAttributeSet (const NetworkSocketAddress *sa, int flag)
{
	if (!sa)
		return false;

	switch (sa->u.sa.sa_family) {

	case AF_INET:
		if (flag & NSA_ADDR)
			if (INADDR_ANY == sa->u.in.sin_addr.s_addr)
				return false;
		if (flag & NSA_PORT)
			if (0 == sa->u.in.sin_port)
				return false;
		break;

	case AF_INET6:
		if (flag & NSA_ADDR)
			if (IN6_IS_ADDR_UNSPECIFIED(&sa->u.in6.sin6_addr))
				return false;
		if (flag & NSA_PORT)
			if (0 == sa->u.in6.sin6_port)
				return false;
		break;

	default:
		return false;
	}

	return true;
}

/**
 * Calculate the hash value of a Socket Address
 *
 * @param sa   Socket Address
 * @param flag Flags specifying which fields to use
 *
 * @return Hash value
 */
//uint32_t sa_hash(const struct sa *sa, int flag)
uint32_t NetworkSocketAddressGetHashValue (const NetworkSocketAddress *sa, int flag)
{
	uint32_t v = 0;

	if (!sa)
		return 0;

	switch (sa->u.sa.sa_family) {

	case AF_INET:
		if (flag & NSA_ADDR)
			v += ntohl(sa->u.in.sin_addr.s_addr);
		if (flag & NSA_PORT)
			v += ntohs(sa->u.in.sin_port);
		break;

	case AF_INET6:
		if (flag & NSA_ADDR) {
			uint32_t *a = (uint32_t *)&sa->u.in6.sin6_addr;
			v += a[0] ^ a[1] ^ a[2] ^ a[3];
		}
		if (flag & NSA_PORT)
			v += ntohs(sa->u.in6.sin6_port);
		break;

	default:
		//DEBUG_WARNING("sa_hash: unknown af %d\n", sa->u.sa.sa_family);
		return 0;
	}

	return v;
}

/**
 * Copy a Socket Address
 *
 * @param dst Socket Address to be written
 * @param src Socket Address to be copied
 */
//void sa_cpy(struct sa *dst, const struct sa *src)
void NetworkSocketAddressCopy	(NetworkSocketAddress *dst, const NetworkSocketAddress *src)
{
	if (!dst || !src)
		return;

	memcpy(dst, src, sizeof(*dst));
}

/**
 * Compare two Socket Address objects
 *
 * @param l    Socket Address number one
 * @param r    Socket Address number two
 * @param flag Flags specifying which fields to use
 *
 * @return true if match, false if no match
 */
//bool sa_cmp(const struct sa *l, const struct sa *r, int flag)
bool NetworkSocketAddressCompare (const NetworkSocketAddress *l, const NetworkSocketAddress *r, int flag)
{
	if (!l || !r)
		return false;

	if (l == r)
		return true;

	if (l->u.sa.sa_family != r->u.sa.sa_family)
		return false;

	switch (l->u.sa.sa_family) {

	case AF_INET:
		if (flag & NSA_ADDR)
			if (l->u.in.sin_addr.s_addr != r->u.in.sin_addr.s_addr)
				return false;
		if (flag & NSA_PORT)
			if (l->u.in.sin_port != r->u.in.sin_port)
				return false;
		break;

	case AF_INET6:
		if (flag & NSA_ADDR)
			if (memcmp(&l->u.in6.sin6_addr,
				   &r->u.in6.sin6_addr, 16))
				return false;
		if (flag & NSA_PORT)
			if (l->u.in6.sin6_port != r->u.in6.sin6_port)
				return false;
		break;

	default:
		return false;
	}

	return true;
}

/** IPv4 Link-local test */
#define IN_IS_ADDR_LINKLOCAL(a)					\
	(((a) & htonl(0xffff0000)) == htonl (0xa9fe0000))


/**
 * Check if socket address is a link-local address
 *
 * @param sa Socket address
 *
 * @return true if link-local address, otherwise false
 */
//bool sa_is_linklocal(const struct sa *sa)
bool NetworkSocketAddressIsLinkLocal (const NetworkSocketAddress *sa)
{
	if (!sa)
		return false;

	switch (NetworkSocketAddressGetAddressFamily(sa)) {

	case AF_INET:
		return IN_IS_ADDR_LINKLOCAL(sa->u.in.sin_addr.s_addr);

	case AF_INET6:
		return IN6_IS_ADDR_LINKLOCAL(&sa->u.in6.sin6_addr);

	default:
		return false;
	}
}


/**
 * Check if socket address is a loopback address
 *
 * @param sa Socket address
 *
 * @return true if loopback address, otherwise false
 */
//bool sa_is_loopback(const struct sa *sa)
bool NetworkSocketAddressIsLoopback (const NetworkSocketAddress *sa)
{
	if (!sa)
		return false;

	switch (NetworkSocketAddressGetAddressFamily(sa)) {

	case AF_INET:
		return INADDR_LOOPBACK == ntohl(sa->u.in.sin_addr.s_addr);

	case AF_INET6:
		return IN6_IS_ADDR_LOOPBACK(&sa->u.in6.sin6_addr);

	default:
		return false;
	}
}


/**
 * Check if socket address is any/unspecified address
 *
 * @param sa Socket address
 *
 * @return true if any address, otherwise false
 */
//bool sa_is_any(const struct sa *sa)
bool NetworkSocketAddressIsAddressUnspecified(const NetworkSocketAddress *sa)
{
	if (!sa)
		return false;

	switch (NetworkSocketAddressGetAddressFamily(sa)) {

	case AF_INET:
		return INADDR_ANY == ntohl(sa->u.in.sin_addr.s_addr);

	case AF_INET6:
		return IN6_IS_ADDR_UNSPECIFIED(&sa->u.in6.sin6_addr);

	default:
		return false;
	}
}


//////// ADDRESS CONVERSION \\\\\\\

#if 1
#ifndef HAVE_INET_PTON


#define NS_INADDRSZ      4       /**< IPv4 T_A */
#define NS_IN6ADDRSZ     16      /**< IPv6 T_AAAA */
#define NS_INT16SZ       2       /**< #/bytes of data in a u_int16_t */

static int inet_pton4(const char *src, u_char *dst);
static int inet_pton6(const char *src, u_char *dst);
static int inet_pton(int af, const char *src, void *dst);

/* int
 * inet_pton4(src, dst)
 *	like inet_aton() but without all the hexadecimal and shorthand.
 * return:
 *	1 if `src' is a valid dotted quad, else 0.
 * notice:
 *	does not touch `dst' unless it's returning 1.
 * author:
 *	Paul Vixie, 1996.
 */
static int
inet_pton4(const char *src, u_char *dst)
{
	static const char digits[] = "0123456789";
	int saw_digit, octets, ch;
	u_char tmp[NS_INADDRSZ], *tp;

	saw_digit = 0;
	octets = 0;
	*(tp = tmp) = 0;
	while ((ch = *src++) != '\0') {
		const char *pch;

		if ((pch = strchr(digits, ch)) != NULL) {
			u_int newVal = (u_int) (*tp * 10 + (pch - digits));

			if (newVal > 255)
				return 0;
			*tp = newVal;
			if (! saw_digit) {
				if (++octets > 4)
					return 0;
				saw_digit = 1;
			}
		}
		else if (ch == '.' && saw_digit) {
			if (octets == 4)
				return 0;
			*++tp = 0;
			saw_digit = 0;
		}
		else
			return 0;
	}
	if (octets < 4)
		return 0;

	memcpy(dst, tmp, NS_INADDRSZ);
	return 1;
}


/* int
 * inet_pton6(src, dst)
 *	convert presentation level address to network order binary form.
 * return:
 *	1 if `src' is a valid [RFC1884 2.2] address, else 0.
 * notice:
 *	(1) does not touch `dst' unless it's returning 1.
 *	(2) :: in a full address is silently ignored.
 * credit:
 *	inspired by Mark Andrews.
 * author:
 *	Paul Vixie, 1996.
 */
static int
inet_pton6(const char *src, u_char *dst)
{
	static const char xdigits_l[] = "0123456789abcdef",
		xdigits_u[] = "0123456789ABCDEF";
	u_char tmp[NS_IN6ADDRSZ], *tp, *endp, *colonp;
	const char *xdigits, *curtok;
	int ch, saw_xdigit;
	u_int val;

	memset((tp = tmp), '\0', NS_IN6ADDRSZ);
	endp = tp + NS_IN6ADDRSZ;
	colonp = NULL;
	/* Leading :: requires some special handling. */
	if (*src == ':')
		if (*++src != ':')
			return 0;
	curtok = src;
	saw_xdigit = 0;
	val = 0;
	while ((ch = *src++) != '\0') {
		const char *pch;

		if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
			pch = strchr((xdigits = xdigits_u), ch);
		if (pch != NULL) {
			val <<= 4;
			val |= (u_int)(pch - xdigits);
			if (val > 0xffff)
				return 0;
			saw_xdigit = 1;
			continue;
		}
		if (ch == ':') {
			curtok = src;
			if (!saw_xdigit) {
				if (colonp)
					return 0;
				colonp = tp;
				continue;
			}
			if (tp + NS_INT16SZ > endp)
				return 0;
			*tp++ = (u_char) (val >> 8) & 0xff;
			*tp++ = (u_char) val & 0xff;
			saw_xdigit = 0;
			val = 0;
			continue;
		}
		if (ch == '.' && ((tp + NS_INADDRSZ) <= endp) &&
		    inet_pton4(curtok, tp) > 0) {
			tp += NS_INADDRSZ;
			saw_xdigit = 0;
			break;	/* '\0' was seen by inet_pton4(). */
		}
		return 0;
	}
	if (saw_xdigit) {
		if (tp + NS_INT16SZ > endp)
			return 0;
		*tp++ = (u_char) (val >> 8) & 0xff;
		*tp++ = (u_char) val & 0xff;
	}
	if (colonp != NULL) {
		/*
		 * Since some memmove()'s erroneously fail to handle
		 * overlapping regions, we'll do the shift by hand.
		 */
		const int n = (int)(tp - colonp);
		int i;

		for (i = 1; i <= n; i++) {
			endp[- i] = colonp[n - i];
			colonp[n - i] = 0;
		}
		tp = endp;
	}
	if (tp != endp)
		return 0;
	memcpy(dst, tmp, NS_IN6ADDRSZ);

	return 1;
}


/**
 * Implementation of inet_pton()
 */
static int
inet_pton(int af, const char *src, void *dst)
{
	if (!src || !dst)
		return 0;

	switch (af) {

	case AF_INET:
		return inet_pton4(src, (u_char*) dst);

	case AF_INET6:
		return inet_pton6(src, (u_char*) dst);

	default:
		syslog (LOG_ERR, "%s: unknown address family %d\n", __func__, af);
		errno = EAFNOSUPPORT;
		return -1;
	}
}

#undef NS_INADDRSZ
#undef NS_IN6ADDRSZ
#undef NS_INT16SZ


#endif	//HAVE_INET_PTON
#endif


/**
 * Convert character string to a network address structure
 *
 * @param addr IP address string
 * @param sa   Returned socket address
 *
 * @return 0 if success, otherwise errorcode
 */
//int net_inet_pton (const char *addr, NetworkSocketAddress *sa)
int ConvertSocketAddressToNetworkFormat (const char *addr, NetworkSocketAddress *sa)
{
	if (inet_pton(AF_INET, addr, &sa->u.in.sin_addr) > 0)
	{
		sa->u.in.sin_family = AF_INET;
	}
	else if (inet_pton(AF_INET6, addr, &sa->u.in6.sin6_addr) > 0)
	{

		if (IN6_IS_ADDR_V4MAPPED(&sa->u.in6.sin6_addr))
		{
			const uint8_t *a = &sa->u.in6.sin6_addr.s6_addr[12];
			sa->u.in.sin_family = AF_INET;
			memcpy(&sa->u.in.sin_addr.s_addr, a, 4);
		}
		else
		{
			sa->u.in6.sin6_family = AF_INET6;
		}
	}
	else
	{
		return EINVAL;
	}

	return 0;
}

#if 1
#ifndef HAVE_INET_NTOP

#define NS_IN6ADDRSZ     16      /**< IPv6 T_AAAA */
#define NS_INT16SZ       2       /**< #/bytes of data in a u_int16_t */

static const char* inet_ntop(int af, const void *src, char *dst, size_t size);
static const char* inet_ntop4(const u_char *src, char *dst, size_t size);
static const char *inet_ntop6(const u_char *src, char *dst, size_t size);

static const char*
inet_ntop4(const u_char *src, char *dst, size_t size)
{
	if (snprintf(dst, size, "%u.%u.%u.%u",
			src[0], src[1], src[2], src[3]) < 0) {
		errno = ENOSPC;
		dst[size-1] = 0;
		return NULL;
	}

	return dst;
}


/* const char *
 * inet_ntop6(src, dst, size)
 *	convert IPv6 binary address into presentation (printable) format
 * author:
 *	Paul Vixie, 1996.
 */

static const char *
inet_ntop6(const u_char *src, char *dst, size_t size)
{
	/*
	 * Note that int32_t and int16_t need only be "at least" large enough
	 * to contain a value of the specified size.  On some systems, like
	 * Crays, there is no such thing as an integer variable with 16 bits.
	 * Keep this in mind if you think this function should have been coded
	 * to use pointer overlays.  All the world's not a VAX.
	 */
	char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
	struct { int base, len; } best, cur;
	u_int words[NS_IN6ADDRSZ / NS_INT16SZ];
	int i;

	/*
	 * Preprocess:
	 *	Copy the input (bytewise) array into a wordwise array.
	 *	Find the longest run of 0x00's in src[] for :: shorthanding.
	 */
	memset(words, '\0', sizeof words);
	for (i = 0; i < NS_IN6ADDRSZ; i++)
		words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
	best.base = -1;
	best.len = 0;
	cur.base = -1;
	cur.len = 0;
	for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
		if (words[i] == 0) {
			if (cur.base == -1)
				cur.base = i, cur.len = 1;
			else
				cur.len++;
		}
		else {
			if (cur.base != -1) {
				if (best.base == -1 || cur.len > best.len)
					best = cur;
				cur.base = -1;
			}
		}
	}
	if (cur.base != -1) {
		if (best.base == -1 || cur.len > best.len)
			best = cur;
	}
	if (best.base != -1 && best.len < 2)
		best.base = -1;

	/*
	 * Format the result.
	 */
	tp = tmp;
	for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
		/* Are we inside the best run of 0x00's? */
		if (best.base != -1 && i >= best.base &&
		    i < (best.base + best.len)) {
			if (i == best.base)
				*tp++ = ':';
			continue;
		}
		/* Are we following an initial run of 0x00s or any real hex?*/
		if (i != 0)
			*tp++ = ':';
		/* Is this address an encapsulated IPv4? */
		if (i == 6 && best.base == 0 &&
		    (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
			if (!inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp)))
				return NULL;
			tp += strlen(tp);
			break;
		}
		tp += sprintf(tp, "%x", words[i]);
	}
	/* Was it a trailing run of 0x00's? */
	if (best.base != -1 && (best.base + best.len) ==
	    (NS_IN6ADDRSZ / NS_INT16SZ))
		*tp++ = ':';
	*tp++ = '\0';

	/*
	 * Check for overflow, copy, and we're done.
	 */
	if ((size_t)(tp - tmp) > size) {
		errno = ENOSPC;
		return NULL;
	}
	strcpy(dst, tmp);

	return dst;
}


/**
 * Implementation of inet_ntop()
 */

static const char*
inet_ntop(int af, const void *src, char *dst, size_t size)
{
	switch (af) {

	case AF_INET:
		return inet_ntop4(src, dst, size);

	case AF_INET6:
		return inet_ntop6(src, dst, size);

	default:
		//DEBUG_WARNING("inet_ntop: unknown address family %d\n", af);
		return NULL;
	}
}

#undef NS_IN6ADDRSZ
#undef NS_INT16SZ

#endif	//HAVE_INET_NTOP
#endif

/**
 * Convert network address structure to a character string
 *
 * @param sa   Socket address
 * @param buf  Buffer to return IP address
 * @param size Size of buffer
 *
 * @return 0 if success, otherwise errorcode
 */
//int net_inet_ntop(const struct sa *sa, char *buf, int size)
int ConvertSocketAddressToReadableFormat(const NetworkSocketAddress *sa, char *buf, int size)
{
	if (!sa || !buf || !size)
		return EINVAL;

	switch (sa->u.sa.sa_family) {

	case AF_INET:
		inet_ntop(AF_INET, &sa->u.in.sin_addr, buf, size);
		break;

	case AF_INET6:
		inet_ntop(AF_INET6, &sa->u.in6.sin6_addr, buf, size);
		break;

	default:
		syslog (LOG_ERR, "%s: unknown address family\n", __func__);
		return EAFNOSUPPORT;
	}

	return 0;
}
