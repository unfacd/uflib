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

#ifndef __INCLUDE_FILE_UTILS_CRYPTO_UFLIB__H__
#define __INCLUDE_FILE_UTILS_CRYPTO_UFLIB__H__

#include <stddef.h>
#include "utils_crypto.h"

#ifndef SHA_DIGEST_LENGTH
# define SHA_DIGEST_LENGTH 20 //bytes should be in ssl's sha.h
#endif

struct CryptoMessage {
	unsigned char version[1];
	union {
	unsigned char *msg_b64;//cipher message encoded in b64
	unsigned char *msg_clear;//clear text message
	unsigned char *msg_raw;//cipher message in binary
	} msg;
	unsigned char *hmac;//actual digest
	unsigned char *final_message;//the entire data space on which digest was calculated
	unsigned char *final_message_b64;
	size_t size;
};
typedef struct CryptoMessage EncryptedMessage;
typedef struct CryptoMessage DecryptedMessage;

int ComputeSHA1 (const unsigned char *input, size_t input_len, char *output, size_t output_len, unsigned b64flag) __attribute__((nonnull));
void ComputeHmacSha256(const unsigned char *text, int text_len, const unsigned char *key, int key_len, void *digest) __attribute__((nonnull));
EncryptedMessage *EncryptWithSignallingKey (const unsigned char *cleartext, size_t textlen, unsigned char *key, bool) __attribute__((nonnull));
DecryptedMessage *DecryptWithSignallingKey (const unsigned char *cleartext, size_t textlen, unsigned char *key, bool flag_b64) __attribute__((nonnull));
void EncryptedMessageDestruct (EncryptedMessage *enc_ptr, bool flag_selfdestruct) __attribute__((nonnull));
void DecryptedMessageDestruct (DecryptedMessage *denc_ptr, bool flag_selfdestruct) __attribute__((nonnull));
unsigned long GenerateRandomNumber ();
unsigned long GenerateRandomNumberWithUpper (long max);
long GenerateRandomNumberBounded (long min, long max) __attribute__ ((const));
int GenerateSecureRandom (uint8_t *data, size_t len) __attribute__((nonnull));
unsigned char *GenerateSalt (unsigned length, bool zero_terminated);
unsigned char *hex_print(const unsigned char *pv, size_t len, unsigned char *outbuffer) __attribute__((nonnull(1)));
int strcmp_constant_time(const void *a, const void *b, const size_t size) __attribute__((nonnull));
int strcmp_time_constant2 (char *a, char *b) __attribute__((nonnull));
int memcpy_constant_time(const void *s1, const void *s2, size_t n);
int memcmp_constant_time (const void *s1, const void *s2, size_t n);

#endif
