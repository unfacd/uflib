#ifndef UFSRV_UFSRVUID_TYPE_H
#define UFSRV_UFSRVUID_TYPE_H

#include <stdint.h>

#define UFSRVUID_TIMESTAMP_SHIFT 23

#ifdef __SIZEOF_INT128__
#define ULIDUINT128
#endif

#ifdef ULIDUINT128_disabled
typedef struct UfsrvUid {
  unsigned __int128 data;
} UfsrvUid;

#else

#define CONFIG_MAX_UFSRV_ID_SZ												16	//16 bytes of id
#define CONFIG_MAX_UFSRV_ID_ENCODED_SZ								26	//in bytes when id is encoded in  Crockford's Base32. 01020CA9CT9G86G08000000000

//IMPORTANT: Keep data as first member, as this data type is cast regularly casted to other types to facilitate quick mapping of data/value access
typedef struct UfsrvUid {
  uint8_t data[CONFIG_MAX_UFSRV_ID_SZ]; //41 bit of time in millis since custom epoch, 23 bit for server instance id (max 8388607), 64 bits for sql sequenceid
} UfsrvUid;
#endif

typedef struct UfsrvUidGeneratorDescriptor {
  unsigned int  instance_id;
  long long     timestamp;
  unsigned long uid;
  UfsrvUid      ufsrvuid;
} UfsrvUidGeneratorDescriptor;

typedef struct UfsrvUidRequesterDescriptor {
  unsigned long uid;
  UfsrvUid      ufsrvuid;
} UfsrvUidRequesterDescriptor;

#endif //UFSRV_UFSRVUID_TYPE_H
