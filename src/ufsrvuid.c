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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <uflib/utils_bits.h>
#include <uflib/ufsrvuid.h>

#define CUSTOM_EPOCH_IN_MILLIS  1401277473000UL//Wed, 28 May 2014 11:44:33 GMT

inline static void _UlidEncodeIdPreamble (long long timestamp, unsigned int instance_id, UfsrvUid *uid_ptr);
inline static void _UlidEncodeIdSequence (unsigned long id_sequence, UfsrvUid *uid_ptr);

inline static void _UlidEncodeIdPreamble (long long timestamp, unsigned int instance_id, UfsrvUid *uid_ptr)
{
  *(unsigned long *)&(uid_ptr->data[0]) = ((timestamp-CUSTOM_EPOCH_IN_MILLIS) << UFSRVUID_TIMESTAMP_SHIFT)| instance_id;

}

inline static void _UlidEncodeIdSequence (unsigned long id_sequence, UfsrvUid *uid_ptr)
{
  *(unsigned long *)&(uid_ptr->data[8]) = id_sequence;

}
__attribute__ ((const, nonnull(1), access(read_only, 1))) unsigned long UfsrvUidGetSequenceId (const UfsrvUid *uid_ptr)
{
  return *(unsigned long *)&(uid_ptr->data[8]);
}

unsigned long UfsrvUidGetSequenceIdFromEncoded (const char *ufsrvuid_encoded)
{
  size_t ufsrvuid_encoded_sz = strlen(ufsrvuid_encoded);
  if (ufsrvuid_encoded_sz != CONFIG_MAX_UFSRV_ID_ENCODED_SZ) {
    return 0;
  }

  UfsrvUid uid = {0};
  UfsrvUidCreateFromEncodedText(ufsrvuid_encoded, &uid);

  return UfsrvUidGetSequenceId(&uid);

}

__attribute__ ((const)) unsigned int UfsrvUidGetInstanceId (const UfsrvUid *uid_ptr)
{
  return GET_N_BITS_FROM_REAR(*(unsigned int *)&(uid_ptr->data[0]), 23);
}

__attribute__ ((const)) unsigned long UfsrvUidGetTimestamp (const UfsrvUid *uid_ptr)
{
  return GET_BITS_IN_BETWEEN(*(unsigned long *)&(uid_ptr->data[0]), 23, 64);
}

__attribute__ ((const)) bool UfsrvUidIsEqual (const UfsrvUid *uid_ptr1, const UfsrvUid *uid_ptr2)
{
  return  (memcmp(uid_ptr1->data, uid_ptr2->data, CONFIG_MAX_UFSRV_ID_SZ)==0);
}

void UfsrvUidCopy (const UfsrvUid *uid_ptr_src, UfsrvUid *uid_ptr_dest)
{
  memcpy(uid_ptr_dest->data, uid_ptr_src->data, CONFIG_MAX_UFSRV_ID_SZ);
}

__attribute__ ((const)) bool UfsrvUidIsSystemUser (const UfsrvUid *uid_ptr)
{
  return  (memcmp(uid_ptr->data, (uint8_t []) {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, CONFIG_MAX_UFSRV_ID_SZ)==0);
}

UfsrvUid *UfsrvUidGenerate (const UfsrvUidGeneratorDescriptor *descriptor_ptr, UfsrvUid *uid_ptr_out)
{
  UfsrvUid *uid_ptr;

  if (IS_EMPTY(uid_ptr_out)) {
    uid_ptr = calloc(1, sizeof(UfsrvUid));
  } else {
    uid_ptr = uid_ptr_out;
  }

  _UlidEncodeIdPreamble(descriptor_ptr->timestamp, descriptor_ptr->instance_id, uid_ptr);
  _UlidEncodeIdSequence(descriptor_ptr->uid, uid_ptr);

#ifdef __UF_TESTING
#include <utils_hex.h>
"standard_c_includes.h"

  char uid_hexified[16+16+1] = {0};
  bin2hex(uid_ptr->data, 16, uid_hexified);
  syslog(LOG_DEBUG, "%s (pid:'%lu'): hex:'%s', timestamp: '%lu', instance_id:'%lu', id_sequence:'%lu'\n", __func__, pthread_self(), uid_hexified,
         GET_BITS_IN_BETWEEN(*(unsigned long *)&(uid_ptr->data[0]), 23, 64), GET_N_BITS_FROM_REAR(*(unsigned int *)&(uid_ptr->data[0]), 23), *(unsigned long *)&(uid_ptr->data[8]));
#endif

  return uid_ptr;
}

/**
 * Crockford's Base32
 * */
const char Encoding[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/**
 * Marshal a UfsrvUid to the passed character array.
 * @dynamic_array: EXPORTS char * if dst_out is NULL
 * */
char * __attribute__((access(write_only, 2)))
UfsrvUidConvertSerialise(const UfsrvUid *uid_ptr, char *dst_out)
{
  char *dst;

  if (IS_EMPTY(dst_out)) {
    dst = calloc(27, sizeof(char));
  } else {
    dst = dst_out;
  }

  dst[0] = Encoding[(uid_ptr->data[0] & 224) >> 5];
  dst[1] = Encoding[uid_ptr->data[0] & 31];
  dst[2] = Encoding[(uid_ptr->data[1] & 248) >> 3];
  dst[3] = Encoding[((uid_ptr->data[1] & 7) << 2) | ((uid_ptr->data[2] & 192) >> 6)];
  dst[4] = Encoding[(uid_ptr->data[2] & 62) >> 1];
  dst[5] = Encoding[((uid_ptr->data[2] & 1) << 4) | ((uid_ptr->data[3] & 240) >> 4)];
  dst[6] = Encoding[((uid_ptr->data[3] & 15) << 1) | ((uid_ptr->data[4] & 128) >> 7)];
  dst[7] = Encoding[(uid_ptr->data[4] & 124) >> 2];

  dst[8] = Encoding[((uid_ptr->data[4] & 3) << 3) | ((uid_ptr->data[5] & 224) >> 5)];
  dst[9] = Encoding[uid_ptr->data[5] & 31];
  dst[10] = Encoding[(uid_ptr->data[6] & 248) >> 3];
  dst[11] = Encoding[((uid_ptr->data[6] & 7) << 2) | ((uid_ptr->data[7] & 192) >> 6)];
  dst[12] = Encoding[(uid_ptr->data[7] & 62) >> 1];
  dst[13] = Encoding[((uid_ptr->data[7] & 1) << 4) | ((uid_ptr->data[8] & 240) >> 4)];
  dst[14] = Encoding[((uid_ptr->data[8] & 15) << 1) | ((uid_ptr->data[9] & 128) >> 7)];
  dst[15] = Encoding[(uid_ptr->data[9] & 124) >> 2];

  dst[16] = Encoding[((uid_ptr->data[9] & 3) << 3) | ((uid_ptr->data[10] & 224) >> 5)];
  dst[17] = Encoding[uid_ptr->data[10] & 31];
  dst[18] = Encoding[(uid_ptr->data[11] & 248) >> 3];
  dst[19] = Encoding[((uid_ptr->data[11] & 7) << 2) | ((uid_ptr->data[12] & 192) >> 6)];
  dst[20] = Encoding[(uid_ptr->data[12] & 62) >> 1];
  dst[21] = Encoding[((uid_ptr->data[12] & 1) << 4) | ((uid_ptr->data[13] & 240) >> 4)];
  dst[22] = Encoding[((uid_ptr->data[13] & 15) << 1) | ((uid_ptr->data[14] & 128) >> 7)];
  dst[23] = Encoding[(uid_ptr->data[14] & 124) >> 2];

  dst[24] = Encoding[((uid_ptr->data[14] & 3) << 3) | ((uid_ptr->data[15] & 224) >> 5)];
  dst[25] = Encoding[uid_ptr->data[15] & 31];

  return dst;
}

/**
 * dec stores decimal encodings for characters.
 * 0xFF indicates invalid character.
 * 48-57 are digits.
 * 65-90 are capital alphabets.
 * */
static const uint8_t dec[256] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        /* 0     1     2     3     4     5     6     7  */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        /* 8     9                                      */
        0x08, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        /*    10(A) 11(B) 12(C) 13(D) 14(E) 15(F) 16(G) */
        0xFF, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
        /*17(H)     18(J) 19(K)       20(M) 21(N)       */
        0x11, 0xFF, 0x12, 0x13, 0xFF, 0x14, 0x15, 0xFF,
        /*22(P)23(Q)24(R) 25(S) 26(T)       27(V) 28(W) */
        0x16, 0x17, 0x18, 0x19, 0x1A, 0xFF, 0x1B, 0x1C,
        /*29(X)30(Y)31(Z)                               */
        0x1D, 0x1E, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

UfsrvUid *UfsrvUidCreateFromEncodedText(const char *str, UfsrvUid *uid_ptr_out)
{
  UfsrvUid *uid_ptr;

  if (IS_PRESENT(uid_ptr_out)) {
    uid_ptr = uid_ptr_out;
  } else {
    uid_ptr = calloc(1, sizeof(UfsrvUid));
  }

  uid_ptr->data[0] = (dec[((int)str[0])] << 5) | dec[(int)(str[1])];
  uid_ptr->data[1] = (dec[(int)(str[2])] << 3) | (dec[(int)(str[3])] >> 2);
  uid_ptr->data[2] = (dec[(int)(str[3])] << 6) | (dec[(int)(str[4])] << 1) | (dec[(int)(str[5])] >> 4);
  uid_ptr->data[3] = (dec[(int)(str[5])] << 4) | (dec[(int)(str[6])] >> 1);
  uid_ptr->data[4] = (dec[(int)(str[6])] << 7) | (dec[(int)(str[7])] << 2) | (dec[(int)(str[8])] >> 3);
  uid_ptr->data[5] = (dec[(int)(str[8])] << 5) | dec[(int)(str[9])];
  uid_ptr->data[6] = (dec[(int)(str[10])] << 3) | (dec[(int)(str[11])] >> 2);
  uid_ptr->data[7] = (dec[(int)(str[11])] << 6) | (dec[(int)(str[12])] << 1) | (dec[(int)(str[13])] >> 4);
  uid_ptr->data[8] = (dec[(int)(str[13])] << 4) | (dec[(int)(str[14])] >> 1);
  uid_ptr->data[9] = (dec[(int)(str[14])] << 7) | (dec[(int)(str[15])] << 2) | (dec[(int)(str[16])] >> 3);
  uid_ptr->data[10] = (dec[(int)(str[16])] << 5) | dec[(int)(str[17])];
  uid_ptr->data[11] = (dec[(int)(str[18])] << 3) | (dec[(int)(str[19])] >> 2);
  uid_ptr->data[12] = (dec[(int)(str[19])] << 6) | (dec[(int)(str[20])] << 1) | (dec[(int)(str[21])] >> 4);
  uid_ptr->data[13] = (dec[(int)(str[21])] << 4) | (dec[(int)(str[22])] >> 1);
  uid_ptr->data[14] = (dec[(int)(str[22])] << 7) | (dec[(int)(str[23])] << 2) | (dec[(int)(str[24])] >> 3);
  uid_ptr->data[15] = (dec[(int)(str[24])] << 5) | dec[(int)(str[25])];

  return uid_ptr;
}
