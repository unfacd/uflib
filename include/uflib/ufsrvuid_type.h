/**
 * @file ufsrvuid_type.h
 * @brief Type definitions for UfsrvUid — structs, macros and helper descriptors.
 */

#ifndef UFLIB_UFSRVUID_TYPE_H
#define UFLIB_UFSRVUID_TYPE_H

#include <stdint.h>

/** Number of bits the timestamp field is left-shifted in the 64-bit preamble. */
#define UFSRVUID_TIMESTAMP_SHIFT 23

#ifdef __SIZEOF_INT128__
#define ULIDUINT128
#endif

#ifdef ULIDUINT128_disabled
/*! 128-bit identifier stored in a single __int128 (disabled variant). */
typedef struct UfsrvUid {
  unsigned __int128 data;
} UfsrvUid;

#else

/** Size in bytes of the binary UfsrvUid. */
#define CONFIG_MAX_UFSRV_ID_SZ          16
/** Size in bytes of the Crockford Base32 text form (26 characters). */
#define CONFIG_MAX_UFSRV_ID_ENCODED_SZ  26

/*! 128-bit identifier: 41-bit timestamp | 23-bit instance id | 64-bit sequence id.
 *
 *  IMPORTANT: keep `data` as the first (and only) member — the accessors read it
 *  directly as a raw byte buffer.
 */
typedef struct UfsrvUid {
  uint8_t data[CONFIG_MAX_UFSRV_ID_SZ]; ///< Raw 16 bytes: data[0..7] hold timestamp(41) + instance_id(23); data[8..15] hold the 64-bit sequence id.
} UfsrvUid;
#endif

/** Reinterpret an arbitrary pointer as a UfsrvUid pointer. */
#define AS_UFSRVUID(x) ((UfsrvUid *)(x))

/*! Generation parameters consumed by UfsrvUidGenerate(). */
typedef struct UfsrvUidGeneratorDescriptor {
  unsigned int  instance_id;  ///< 23-bit server instance id (masked to 23 bits on use).
  long long     timestamp;    ///< Absolute timestamp in milliseconds (relative to the custom epoch).
  unsigned long uid;          ///< 64-bit sequence id.
  UfsrvUid      ufsrvuid;     ///< Embedded UfsrvUid (unused by UfsrvUidGenerate).
} UfsrvUidGeneratorDescriptor;

/*! Requester identity: a sequence id paired with its UfsrvUid. */
typedef struct UfsrvUidRequesterDescriptor {
  unsigned long uid;          ///< 64-bit sequence id.
  UfsrvUid      ufsrvuid;     ///< Associated UfsrvUid.
} UfsrvUidRequesterDescriptor;

/*! A sequence id paired with a pointer to its UfsrvUid. */
typedef struct UfsrvUidSequenceIdPair {
  unsigned long uid;          ///< 64-bit sequence id.
  UfsrvUid     *ufsrvuid;     ///< Pointer to the associated UfsrvUid.
} UfsrvUidSequenceIdPair;

#endif //UFSRV_UFSRVUID_TYPE_H
