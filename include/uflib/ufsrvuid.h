/**
 * @file ufsrvuid.h
 * @brief UfsrvUid — a 128-bit identifier (Crockford Base32 text form) for ufsrv
 *        messaging entities.
 */

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

#ifndef UFLIB_UFSRVUID_H
#define UFLIB_UFSRVUID_H

#include <uflib/uflib_defs.h>

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/ufsrvuid_type.h>

/** Canonical system-user encoding: instance id 1, timestamp 0, sequence id 0. */
#define UFSRV_SYSTEMUSER_UID "01000000000000000000000000"
/** Sentinel for an absent UfsrvUid. */
#define EMPTY_UFSRVUID NULL

/**
 * @brief Generate a UfsrvUid from a timestamp, instance id and sequence id.
 *
 * Packs a 41-bit timestamp (milliseconds since the custom epoch), a 23-bit
 * instance id, and a 64-bit sequence id into the 128-bit UID. Out-of-range
 * fields are clamped: the timestamp delta to [0, 2^41-1] and the instance id to
 * 23 bits.
 *
 * @param[in]  descriptor_ptr  Generation parameters (timestamp, instance_id, uid).
 * @param[out] ulid_out        Optional output. If NULL, a UfsrvUid is allocated.
 *
 * @return The UID (ulid_out, or a heap allocation the caller must free), or NULL
 *         if descriptor_ptr is NULL or allocation fails.
 *
 * @code{.c}
 * UfsrvUidGeneratorDescriptor d = { .instance_id = 1,
 *                                   .timestamp = 1401277473000LL,
 *                                   .uid = 42 };
 * UfsrvUid uid = {0};
 * if (UfsrvUidGenerate(&d, &uid) == NULL) { // handle error
 * }
 * @endcode
 */
PUBLIC_API UfsrvUid *UfsrvUidGenerate(const UfsrvUidGeneratorDescriptor *descriptor_ptr, UfsrvUid *ulid_out);

/**
 * @brief Create a UfsrvUid from its 16-byte binary representation.
 *
 * @param[in]  b        16-byte source buffer (copied).
 * @param[out] ulid_out Optional output. If NULL, a UfsrvUid is allocated.
 *
 * @return The UID, or NULL if b is NULL or allocation fails.
 *
 * @code{.c}
 * const uint8_t raw[16] = {0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
 * UfsrvUid *uid = UfsrvUidCreateFromBinary(raw, NULL);
 * if (uid) { // use uid, then release it
 *   free(uid);
 * }
 * @endcode
 */
PUBLIC_API UfsrvUid *UfsrvUidCreateFromBinary(const uint8_t b[16], UfsrvUid *ulid_out);

/**
 * @brief Decode a 26-character Crockford Base32 text into a UfsrvUid.
 *
 * The input is validated: it must be exactly 26 characters, every symbol must be
 * in the Crockford alphabet (I, L, O, U excluded), and the leading character must
 * be in the canonical 0..7 range (it carries only 3 bits). On any violation the
 * function returns NULL and leaves uid_ptr_out untouched.
 *
 * @param[in]  str         NUL-terminated 26-character encoding.
 * @param[out] uid_ptr_out Optional output. If NULL, a UfsrvUid is allocated.
 *
 * @return The UID, or NULL if str is invalid or allocation fails.
 *
 * @code{.c}
 * UfsrvUid uid = {0};
 * if (UfsrvUidCreateFromEncodedText("01000000000000000000000000", &uid) == NULL) {
 *   // malformed encoding
 * }
 * @endcode
 */
PUBLIC_API UfsrvUid *UfsrvUidCreateFromEncodedText(const char *str, UfsrvUid *uid_ptr_out);

/**
 * @brief Serialise a UfsrvUid to its 16-byte binary representation.
 *
 * @param[in]  ulid  The UID to serialise.
 * @param[out] dst   Destination buffer (16 bytes). Must not be NULL.
 *
 * @return dst, or NULL if ulid or dst is NULL.
 *
 * @code{.c}
 * uint8_t raw[16] = {0};
 * UfsrvUidConvertToBinary(&uid, raw);
 * @endcode
 */
PUBLIC_API uint8_t *UfsrvUidConvertToBinary(const UfsrvUid *ulid, uint8_t dst[16]);

/**
 * @brief Serialise a UfsrvUid to its 26-char Crockford Base32 text form.
 *
 * @param[in]     uid_ptr  The UID to serialise.
 * @param[in,out] dst_out  Optional output buffer (>= CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1
 *                         bytes). If NULL, a buffer of that size is allocated.
 *
 * @return The destination buffer (dst_out, or the allocation on the NULL path),
 *         or NULL if uid_ptr is NULL or allocation fails.
 *
 * @note The output is always NUL-terminated (26 chars + NUL). A caller-provided
 *       buffer must therefore be at least CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1
 *       (27) bytes; the dst_out == NULL path allocates exactly that size.
 *
 * @code{.c}
 * char buf[27] = {0};
 * UfsrvUidConvertSerialise(&uid, buf); // 26 chars + NUL (27 bytes)
 *
 * char *s = UfsrvUidConvertSerialise(&uid, NULL); // NUL-terminated; free(s)
 * @endcode
 */
PUBLIC_API char *UfsrvUidConvertSerialise(const UfsrvUid *uid_ptr, char *dst_out);

/**
 * @brief Return the 64-bit sequence id of a UfsrvUid.
 *
 * @param[in] uid_ptr  The UID. NULL returns 0.
 *
 * @return The sequence id.
 *
 * @code{.c}
 * unsigned long seq = UfsrvUidGetSequenceId(&uid);
 * @endcode
 */
PUBLIC_API unsigned long UfsrvUidGetSequenceId(const UfsrvUid *uid_ptr);

/**
 * @brief Return the sequence id from a 26-character encoded UfsrvUid.
 *
 * @param[in] ufsrvuid_encoded  NUL-terminated encoding. NULL returns ULONG_MAX.
 *
 * @return The sequence id, or ULONG_MAX if the encoding is malformed (wrong
 *         length or invalid content).
 *
 * @code{.c}
 * unsigned long seq = UfsrvUidGetSequenceIdFromEncoded("01000000000000000000000000");
 * if (seq == ULONG_MAX) { // malformed
 * }
 * @endcode
 */
PUBLIC_API unsigned long UfsrvUidGetSequenceIdFromEncoded(const char *ufsrvuid_encoded);

/**
 * @brief Return the 23-bit instance id of a UfsrvUid.
 *
 * @param[in] uid_ptr  The UID. NULL returns 0.
 *
 * @return The instance id.
 *
 * @code{.c}
 * unsigned int inst = UfsrvUidGetInstanceId(&uid);
 * @endcode
 */
PUBLIC_API unsigned int UfsrvUidGetInstanceId(const UfsrvUid *uid_ptr);

/**
 * @brief Return the 41-bit timestamp of a UfsrvUid, in milliseconds since the
 *        custom epoch (2014-05-28 11:44:33 GMT).
 *
 * @param[in] uid_ptr  The UID. NULL returns 0.
 *
 * @return The timestamp delta in milliseconds.
 *
 * @code{.c}
 * unsigned long ts = UfsrvUidGetTimestamp(&uid);
 * @endcode
 */
PUBLIC_API unsigned long UfsrvUidGetTimestamp(const UfsrvUid *uid_ptr);

/**
 * @brief Test two UfsrvUid objects for byte equality.
 *
 * @param[in] uid_ptr1  First UID.
 * @param[in] uid_ptr2  Second UID.
 *
 * @return true if equal (or both NULL), false otherwise.
 *
 * @code{.c}
 * if (UfsrvUidIsEqual(&a, &b)) { // same id
 * }
 * @endcode
 */
PUBLIC_API bool UfsrvUidIsEqual(const UfsrvUid *uid_ptr1, const UfsrvUid *uid_ptr2);

/**
 * @brief Test whether a UfsrvUid is the canonical system user.
 *
 * @param[in] uid_ptr  The UID. NULL returns false.
 *
 * @return true if it matches the system-user UID, false otherwise.
 *
 * @code{.c}
 * if (UfsrvUidIsSystemUser(&uid)) { // system user
 * }
 * @endcode
 */
PUBLIC_API bool UfsrvUidIsSystemUser(const UfsrvUid *uid_ptr);

/**
 * @brief Return a pointer to the shared, read-only system-user UfsrvUid.
 *
 * @return Pointer to the static system-user UID. Do not free or modify.
 *
 * @code{.c}
 * const UfsrvUid *sys = UfsrvUidRawSystemUser();
 * @endcode
 */
PUBLIC_API const UfsrvUid *UfsrvUidRawSystemUser();

/**
 * @brief Return a pointer to the shared, read-only system-user raw bytes.
 *
 * @details Provide binary representation of system UfsrvUid (being of sequence id '0') and and encoded value of "01000000000000000000000000"
 *
 * @return Pointer to the static 16-byte system-user data. Do not free or modify.
 *
 * @code{.c}
 * const uint8_t *raw = UfsrvUidRawDataSystemUser();
 * @endcode
 */
PUBLIC_API const uint8_t *UfsrvUidRawDataSystemUser();

/**
 * @brief Copy one UfsrvUid into another.
 *
 * @param[in]  uid_ptr_src   Source UID.
 * @param[out] uid_ptr_dest  Destination UID.
 *
 * @note No-op if either pointer is NULL.
 *
 * @code{.c}
 * UfsrvUid copy = {0};
 * UfsrvUidCopy(&uid, &copy);
 * @endcode
 */
PUBLIC_API void UfsrvUidCopy(const UfsrvUid *uid_ptr_src, UfsrvUid *uid_ptr_dest);

#endif //UFSRV_ULID_H
