/**
 * @file rfc.c
 * @brief IPv4 / IPv6 private-address classification.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 7/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */


/*
 * ip_private.h
 *
 * C17
 *
 * IPv4: RFC1918
 * IPv6: Unique Local Addresses (RFC4193), fc00::/7
 *
 * Text parsing: POSIX inet_pton()
 *
 * Binary IPv4: struct in_addr (network byte order)
 * Binary IPv6: struct in6_addr
 *
 * Public frontend:
 *     sIsIpAddressPrivate()
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include "net/RFC4193.h"

/*
 * Internal IPv4 RFC1918 classifier.
 *
 * Input: host-byte-order uint32_t.
 */
static inline bool
IsIpAddressPrivateIpv4(uint32_t ip)
{
  return ((ip & UINT32_C(0xFF000000)) == UINT32_C(0x0A000000)) || ((ip & UINT32_C(0xFFF00000)) == UINT32_C(0xAC100000))
    || ((ip & UINT32_C(0xFFFF0000)) == UINT32_C(0xC0A80000));
}


/*
 * Internal IPv6 ULA classifier.
 *
 * RFC4193: fc00::/7
 *
 * First byte:
 *     0xFC = private
 *     0xFD = private
 */
static inline bool
sIsIpAddressPrivateIpv6(const struct in6_addr *ip)
{
  return (ip->s6_addr[0] & 0xFEu) == 0xFCu;
}

/*
 * Public frontend.
 *
 * @param family      IP_FAMILY_IPV4 or IP_FAMILY_IPV6
 *
 * @param input       IP_INPUT_BINARY or IP_INPUT_TEXT
 *
 * @param address     Pointer to:
 *                      - struct in_addr    (binary IPv4)
 *                      - struct in6_addr   (binary IPv6)
 *                      - NUL-terminated string (text)
 *
 * @param is_private  Output classification.
 *
 * @return true  = input successfully processed false = invalid arguments, family, representation, or invalid textual address.
 *
 * On failure, *is_private is set to false when the output pointer is valid.
 */
bool
IsIpAddressPrivate(NetIpFamilyEnum family, NetIpFormatEnum input, const void *address, bool *is_private)
{
  struct in_addr  ipv4;
  struct in6_addr ipv6;

  uint32_t host_ip;

  if (is_private == NULL) return false;

  *is_private = false;

  if (address == NULL) return false;

  if (input != IP_INPUT_BINARY && input != IP_INPUT_TEXT) return false;

  if (family == IP_FAMILY_IPV4) {
    if (input == IP_INPUT_TEXT) {
      if (inet_pton(AF_INET, (const char*)address, &ipv4) != 1) return false;
    }
    else {
      memcpy(&ipv4, address, sizeof(ipv4));
    }

    host_ip = ntohl(ipv4.s_addr);

    *is_private = IsIpAddressPrivateIpv4(host_ip);

    return true;
  }

  if (family == IP_FAMILY_IPV6) {
    if (input == IP_INPUT_TEXT) {
      if (inet_pton(AF_INET6, (const char*)address, &ipv6) != 1) return false;
    }
    else {
      memcpy(&ipv6, address, sizeof(ipv6));
    }

    *is_private = sIsIpAddressPrivateIpv6(&ipv6);

    return true;
  }

  return false;
}

NetIpFamilyEnum
IpAddressFamily(const char *address)
{
  if (address == NULL || *address == '\0') return IP_FAMILY_UNKNOWN;

  /*
   * Every conventional IPv6 textual address contains ':'.
   * This selects the parser candidate; inet_pton validates it.
   */
  int af = strchr(address, ':') != NULL ? AF_INET6 : AF_INET;

  struct in_addr  ipv4;
  struct in6_addr ipv6;

  if (af == AF_INET) {
    return inet_pton(AF_INET, address, &ipv4) == 1 ? IP_FAMILY_IPV4 : IP_FAMILY_UNKNOWN;
  }

  return inet_pton(AF_INET6, address, &ipv6) == 1 ? IP_FAMILY_IPV6 : IP_FAMILY_UNKNOWN;
}