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

#ifndef UFSRV_ULID_H
#define UFSRV_ULID_H

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/ufsrvuid_type.h>

//yields sequence id 1 {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
#define UFSRV_SYSTEMUSER_UID "00000000000000200000000000"

UfsrvUid *UfsrvUidGenerate (const UfsrvUidGeneratorDescriptor *descriptor_ptr, UfsrvUid *ulid_out);
UfsrvUid *UfsrvUidCreateFromBinary (const uint8_t b[16], UfsrvUid *ulid_out);
UfsrvUid *UfsrvUidCreateFromEncodedText(const char *str, UfsrvUid *uid_ptr_out);
uint8_t *UfsrvUidConvertToBinary (const UfsrvUid *ulid, uint8_t dst[16]);
char *UfsrvUidConvertSerialise (const UfsrvUid *uid_ptr, char *dst_out);

unsigned long UfsrvUidGetSequenceId (const UfsrvUid *uid_ptr);
unsigned long UfsrvUidGetSequenceIdFromEncoded (const char *ufsrvuid_encoded);
unsigned int UfsrvUidGetInstanceId (const UfsrvUid *uid_ptr);
unsigned long UfsrvUidGetTimestamp (const UfsrvUid *uid_ptr);
bool UfsrvUidIsEqual (const UfsrvUid *uid_ptr1, const UfsrvUid *uid_ptr2);
bool UfsrvUidIsSystemUser (const UfsrvUid *uid_ptr);
void UfsrvUidCopy (const UfsrvUid *uid_ptr_src, UfsrvUid *uid_ptr_dest);

#endif //UFSRV_ULID_H
