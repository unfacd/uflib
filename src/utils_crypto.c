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

#include <uflib/standard_c_includes.h>
#include <uflib/standard_defs.h>

#include <resolv.h>
#include <bsd/stdlib.h>

#include <uflib/utils_crypto.h>
#include <openssl/sha.h>
#include <openssl/rand.h>//crypto random number generator
#include <uflib/utils_time.h>
#include <uflib/utils_base64.h>
#include <openssl/err.h>

/**
  * for crypto sesitive stuff, or use OPENSSL's CRYPTO_memcmp
  */
int strcmp_constant_time(const void * a, const void *b, const size_t size)
{
  const unsigned char *_a = (const unsigned char *) a;
  const unsigned char *_b = (const unsigned char *) b;
  unsigned char result = 0;
  size_t i;

  for (i = 0; i < size; i++) {
    result |= _a[i] ^ _b[i];
  }

  return result; /* returns 0 if equal, nonzero otherwise */
}

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function.
 *
 * Note that limiting the comparison length to strings up to MBUF bytes we
 * can avoid leaking any information about the password length and any
 * possible branch misprediction related leak.
 */
int
strcmp_time_constant2(char *a, char *b)
{
  char bufa[MBUF], bufb[MBUF];

  /* The above two strlen perform len(a) + len(b) operations where either
   * a or b are fixed (our password) length, and the difference is only
   * relative to the length of the user provided string, so no information
   * leak is possible in the following two lines of code. */
  unsigned int alen = strlen(a);
  unsigned int blen = strlen(b);
  unsigned int j;
  int diff = 0;

  /* We can't compare strings longer than our static buffers.
   * Note that this will never pass the first test in practical circumstances
   * so there is no info leak. */
  if (alen > sizeof(bufa) || blen > sizeof(bufb)) return 1;

  memset(bufa,0,sizeof(bufa));        /* Constant time. */
  memset(bufb,0,sizeof(bufb));        /* Constant time. */

  /* Again the time of the following two copies is proportional to
   * len(a) + len(b) so no info is leaked. */
  memcpy(bufa,a,alen);
  memcpy(bufb,b,blen);

  /* Always compare all the chars in the two buffers without
   * conditional expressions. */
  for (j = 0; j < sizeof(bufa); j++)
  {
    diff |= (bufa[j] ^ bufb[j]);
  }

  /* Length must be equal as well. */
  diff |= alen ^ blen;
  return diff; /* If zero strings are the same. */
}

int
memcpy_constant_time(const void *s1, const void *s2, size_t n)
{
  size_t i;
  const unsigned char *c1 = (const unsigned char *) s1;
  const unsigned char *c2 = (const unsigned char *) s2;
  unsigned char result = 0;

  for (i = 0; i < n; i++) {
    result |= c1[i] ^ c2[i];
  }

  return result;
}

int
memcmp_constant_time (const void *s1, const void *s2, size_t n)
{
  size_t i;
  const unsigned char *c1 = (const unsigned char *) s1;
  const unsigned char *c2 = (const unsigned char *) s2;
  unsigned char result = 0;

  for (i = 0; i < n; i++) {
    result |= c1[i] ^ c2[i];
  }

  return result;
}

/*
 * user must allocate output to SHA_DIGEST_LENGTH*2+1
 */
int
ComputeSHA1(const unsigned char *input, size_t input_len, char *output, size_t output_len, unsigned b64flag)
{
  unsigned char raw_buf[SHA_DIGEST_LENGTH];

  memset (raw_buf, 0, sizeof(raw_buf));

  SHA1(input, input_len, raw_buf);

  if (b64flag)
  {
    int result = b64_ntop(raw_buf, SHA_DIGEST_LENGTH, output, output_len);

    return result;
  }
  else
  {
    int i;
    for (i=0; i < SHA_DIGEST_LENGTH; i++)
    {
      sprintf((char *)&(output[i*2]), "%02x", raw_buf[i]);
    }

    output[SHA_DIGEST_LENGTH*2]='\0';

    return sizeof(output);
  }

  return -1;
}

//https://gist.github.com/tsupo/112188/acdbf002acf454bd60c355a776b9a5b58b6dff5e
void
ComputeHmacSha256(
        const unsigned char *text,      /* pointer to data stream        */
        int                 text_len,   /* length of data stream         */
        const unsigned char *key,       /* pointer to authentication key */
        int                 key_len,    /* length of authentication key  */
        void                *digest)    /* caller digest to be filled in */
        {
  unsigned char k_ipad[65];   /* inner padding -
                                 * key XORd with ipad
                                 */
  unsigned char k_opad[65];   /* outer padding -
                                 * key XORd with opad
                                 */
  unsigned char tk[SHA256_DIGEST_LENGTH];
  unsigned char tk2[SHA256_DIGEST_LENGTH];
  unsigned char bufferIn[1024];
  unsigned char bufferOut[1024];
  int           i;

  /* if key is longer than 64 bytes reset it to key=sha256(key) */
  if ( key_len > 64 ) {
    SHA256( key, key_len, tk );
    key     = tk;
    key_len = SHA256_DIGEST_LENGTH;
  }

  /*
   * the HMAC_SHA256 transform looks like:
   *
   * SHA256(K XOR opad, SHA256(K XOR ipad, text))
   *
   * where K is an n byte key
   * ipad is the byte 0x36 repeated 64 times
   * opad is the byte 0x5c repeated 64 times
   * and text is the data being protected
   */

  /* start out by storing key in pads */
  memset( k_ipad, 0, sizeof k_ipad );
  memset( k_opad, 0, sizeof k_opad );
  memcpy( k_ipad, key, key_len );
  memcpy( k_opad, key, key_len );

  /* XOR key with ipad and opad values */
  for ( i = 0; i < 64; i++ ) {
    k_ipad[i] ^= 0x36;
    k_opad[i] ^= 0x5c;
  }

  /*
   * perform inner SHA256
   */
  memset( bufferIn, 0x00, 1024 );
  memcpy( bufferIn, k_ipad, 64 );
  memcpy( bufferIn + 64, text, text_len );

  SHA256( bufferIn, 64 + text_len, tk2 );

  /*
   * perform outer SHA256
   */
  memset( bufferOut, 0x00, 1024 );
  memcpy( bufferOut, k_opad, 64 );
  memcpy( bufferOut + 64, tk2, SHA256_DIGEST_LENGTH );

  SHA256( bufferOut, 64 + SHA256_DIGEST_LENGTH, digest );
        }


        ///----------------------------------------------------
        static int seed_with_urandom(void)
        {
  unsigned int seed;
  int fd;

  fd = open("/dev/urandom", O_RDONLY);
  if(fd >= 0)
  {
    if(read(fd, &seed, sizeof(seed)) == sizeof(seed))
    {
      close(fd);
      srand(seed);
      return 1;
    }
  }
  return 0;
        }


        static void seed_with_clock(struct timeval *time_in)
                {


  const struct timeval *tv;

  set_time(time_in);
  tv = time_in;
  srand(tv->tv_sec ^ (tv->tv_usec | (getpid() << 20)));
                }


                void SeedRandom(struct timeval *time_in)
                        {

  {
    if(!seed_with_urandom())
      seed_with_clock(time_in);
    return;
  }

                        }

                        unsigned long
                        GenerateRandomNumber(void)
                        {
  return llabs((long long int)(rand()*rand()));

                        }

                        /**
                         *	@param length: 128 for strong output
                         * 	@returns salt string or NULL
                         *
                         * 	@dynamic_memory: ALLOCATES unsigned char * which must be freed by the user
                         */
                        unsigned char *
                        GenerateSalt(unsigned length, bool zero_terminated)
                        {
                          unsigned char *generated_saltb;
                          unsigned salt_length = 0;

                          zero_terminated?(salt_length+=length):(salt_length+=(length+1));//allocate extra space for '\0'
                          generated_saltb=calloc(salt_length, sizeof(unsigned char));

                          int rc = RAND_bytes(generated_saltb, length);//note we use length

                          unsigned long err = ERR_get_error();

                          if (rc == 1) {
                            unsigned char *printable_salt;
                            printable_salt=calloc((salt_length*2)+1, sizeof(unsigned char));

                            int i;
                            for (i=0; i < salt_length; i++) {
                              sprintf((char *)&(printable_salt[i*2]), "%02x", generated_saltb[i]);
                            }

                            if (zero_terminated)	printable_salt[salt_length * 2] = '\0';
                            free (generated_saltb);

                            //printable_salt[i*2+0] = hexdigits [generated_saltb[i] >> 4];
                            return printable_salt;
                          } else {
                            free (generated_saltb);

                            return NULL;
                          }

                          return NULL;
                        }

                        int
                        GenerateSecureRandom(uint8_t *data, size_t len)
                        {
                          arc4random_buf(data, len);
                          return 0;
                        }

                        // Assumes 0 <= max <= RAND_MAX
                        // Returns in the half-open interval [0, max]
                        unsigned long
                        GenerateRandomNumberWithUpper(long max)
                        {
                          unsigned long
                          // max <= RAND_MAX < ULONG_MAX, so this is okay.
                          num_bins = (unsigned long) max + 1,
                          num_rand = (unsigned long) RAND_MAX + 1,
                          bin_size = num_rand / num_bins,
                          defect   = num_rand % num_bins;

                          long x;
                          do {
                            x = rand();
                          } while (num_rand - defect <= (unsigned long)x);	// This is carefully written not to overflow

                          // Truncated division is intentional
                          return abs(x/bin_size);
                        }

                        long
                        GenerateRandomNumberBounded(long min, long max)
                        {
                          return rand()%(max-min) + min;
                        }

                        unsigned char *
                        hex_print(const unsigned char *pv, size_t len, unsigned char *outbuffer)
                        {
                          const unsigned char *p    = pv;
                          unsigned char       *out  = NULL;

                          if (outbuffer)	out = outbuffer;
                          else            out = calloc((len * 2) + 1, sizeof(char));

                          size_t i = 0;
                          for (; i<len; ++i)	sprintf((char *)&out[i * 2], "%02X", *p++);

                          return out;
                        }

#include <openssl/aes.h>

//this is protocol specific stuff
#define CIPHER_KEY_SIZE 32
#define MAC_KEY_SIZE    20
#define MAC_VALUE_SIZE	10//we only including the 1st 10 bytes of the value of the hmac
#define VERSION_OFFSET	0
#define VERSION_LENGTH	1
#define IV_OFFSET	VERSION_OFFSET + VERSION_LENGTH;
#define IV_LENGTH	16
#define	CIPHERTEXT_OFFSET IV_OFFSET + IV_LENGTH;
#define KEYLENGTH_AES256	256
//#define AES_BLOCK_SIZE	16 //defined in openssl


/*
 * @brief: the layout of the resultant object: 0--15(IV) 16---end (encrypted)
 * The signalling key layout:
 * 0-19(mac key), 20-51 cipher key
 * b64 is produced on the final layout below
 * +--+--------------+------------------------+--------------+
 *  1V	16 IV			X ciphermsg				MAC_VALUE_SIZE hmac
 * http://stackoverflow.com/questions/18152913/aes-aes-cbc-128-aes-cbc-192-aes-cbc-256-encryption-decryption-with-openssl-c
 */
EncryptedMessage *
EncryptWithSignallingKey(const unsigned char *cleartext, size_t textlen, unsigned char *key, bool flag_b64encoded_key)
{
  int rc_len = 0;
  unsigned char key_cipher[CIPHER_KEY_SIZE+1];
  unsigned char key_mac[MAC_KEY_SIZE+1];
  unsigned char *b64decoded_key = NULL;

  if (flag_b64encoded_key) {
    b64decoded_key = base64_decode(key, strlen((char *)key), &rc_len);

    if (!b64decoded_key)	return NULL;
  } else {
    b64decoded_key = key;
  }

  if (rc_len < (CIPHER_KEY_SIZE + MAC_KEY_SIZE)) {
    syslog(LOG_DEBUG, "%s: ERROR KEY LEN INVALID: '%d'", __func__, rc_len);

    return NULL;
  }

  //isolate the mac key and cipher key
  memcpy (key_cipher, b64decoded_key, CIPHER_KEY_SIZE);
  memcpy (key_mac, b64decoded_key + CIPHER_KEY_SIZE, MAC_KEY_SIZE);

  const size_t encslength = ((textlen + AES_BLOCK_SIZE) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;//allocate in block sizes
  unsigned char *enc_out  = calloc(encslength+AES_BLOCK_SIZE, sizeof(unsigned char));//room for both, IV and final encrypted object

  //embed the init vector with 16 random bytes at the beginning
  unsigned char enc_iv[AES_BLOCK_SIZE]={0};
  RAND_bytes(enc_iv, AES_BLOCK_SIZE);
  memcpy(enc_out, enc_iv, AES_BLOCK_SIZE);//iv must be mutated in here before used in encryption shifted 1 byte from start of buffer

#if __UF_TESTING
{
  unsigned char key_cipher_out[MBUF] = {0}; unsigned char key_mac_out[MBUF] = {0}; unsigned char iv_out[MBUF] = {0};
  syslog(LOG_DEBUG, "%s: {key_cipher:'%s', key_mac:'%s', iv:'%s'}", __func__, hex_print(key_cipher, 32, key_cipher_out), hex_print(key_mac,20, key_mac_out), hex_print(enc_iv, 16, iv_out));
}
#endif

AES_KEY enc_key;
  AES_set_encrypt_key(key_cipher, KEYLENGTH_AES256, &enc_key);
  AES_cbc_encrypt(cleartext, enc_out + AES_BLOCK_SIZE, textlen, &enc_key, enc_iv, AES_ENCRYPT);//desposit past version+IV

#ifdef __UF_FULLDEBUG
//TODO: MEMORY LEAK AS RETURNED HEAP STRING from hex_print NOT FREED
syslog(LOG_DEBUG, "%s: {cipher_text:'%s'}", __func__, hex_print(enc_out+AES_BLOCK_SIZE, encslength, NULL));
#endif

#if 0
{//test block
  AES_KEY denc_key;
  AES_set_decrypt_key(key_cipher, KEYLENGTH_AES256, &denc_key);
  unsigned char denc_iv[AES_BLOCK_SIZE]={0};
  memcpy(denc_iv, enc_out, AES_BLOCK_SIZE);

  unsigned char *denc_out=calloc(encslength+1, sizeof(unsigned char));

  AES_cbc_encrypt(enc_out+AES_BLOCK_SIZE, denc_out, encslength, &denc_key, denc_iv, AES_DECRYPT);

  syslog(LOG_DEBUG, "DECRYPTED:'%s'", denc_out);
  free(denc_out);
}
#endif

EncryptedMessage *enc_ptr = calloc(1, sizeof(EncryptedMessage));
  enc_ptr->version[0] = (unsigned char)1;
  enc_ptr->msg.msg_b64 = base64_encode(enc_out, encslength + AES_BLOCK_SIZE, NULL);

  //allocate room
  size_t hmac_final_size = VERSION_LENGTH + encslength + AES_BLOCK_SIZE;
  enc_ptr->hmac = calloc(hmac_final_size, sizeof(unsigned char));//the actual digest
  unsigned char *hmac_input = calloc(hmac_final_size, sizeof(unsigned char));//data stream on which hmac digest is being calculated

  //Concatenate data in contiguous space: version->iv->cipher text
  memcpy(hmac_input, enc_ptr->version, sizeof(enc_ptr->version));
  memcpy(hmac_input + sizeof(enc_ptr->version), enc_out, encslength+AES_BLOCK_SIZE);

  //calculate on concatenated data space
  ComputeHmacSha256(hmac_input, hmac_final_size, key_mac, MAC_KEY_SIZE, enc_ptr->hmac);

  {
    unsigned char digest_raw[hmac_final_size+1];
    memset(digest_raw, 0, sizeof(digest_raw));

    syslog(LOG_DEBUG, "%s: {hmac_hex:'%s'}", __func__, hex_print(enc_ptr->hmac, hmac_final_size, digest_raw));
  }

  //Concatenate into final data stream
  enc_ptr->final_message = calloc(hmac_final_size + MAC_VALUE_SIZE, sizeof(unsigned char));//only including MAC_KEY amount from hmac value
  memcpy(enc_ptr->final_message, hmac_input, hmac_final_size);
  memcpy(enc_ptr->final_message + hmac_final_size, enc_ptr->hmac, MAC_VALUE_SIZE);
  enc_ptr->final_message_b64 = base64_encode(enc_ptr->final_message, hmac_final_size + MAC_VALUE_SIZE, NULL);

  enc_ptr->size = VERSION_LENGTH + encslength + AES_BLOCK_SIZE + MAC_VALUE_SIZE;

  if (flag_b64encoded_key)	free(b64decoded_key);
  free (hmac_input);
  memset(enc_out, 0, (encslength + AES_BLOCK_SIZE) * sizeof(unsigned char));
  free (enc_out);

  return enc_ptr;

}

/**
 * @params ciphertext: b64 encoded cipher text, with the iv embedded as IV_LENGTH bytes at the begining
 */
DecryptedMessage *
DecryptWithSignallingKey(const unsigned char *ciphertext_b64, size_t ciphertext_len, unsigned char *key, bool flag_b64encoded_key)
{
  int rc_len = 0;
  unsigned char key_cipher[CIPHER_KEY_SIZE+1];
  unsigned char key_mac[MAC_KEY_SIZE+1];
  unsigned char *b64decoded_key = NULL;

  if (flag_b64encoded_key) {
    b64decoded_key = base64_decode(key, strlen((char *)key), &rc_len);

    if (!b64decoded_key)	return NULL;
  } else {
    b64decoded_key = key;
  }

  if (rc_len < (CIPHER_KEY_SIZE+MAC_KEY_SIZE))
  {
    syslog(LOG_DEBUG, "%s: ERROR KEY LEN INVALID: '%d'", __func__, rc_len);

    goto ciphertext_b64_decode_error;
  }

  //isolate the mac key and the cipher key
  memcpy (key_cipher, b64decoded_key, CIPHER_KEY_SIZE);
  memcpy (key_mac, b64decoded_key+CIPHER_KEY_SIZE, MAC_KEY_SIZE);

  unsigned char *ciphertext_b64_decoded = NULL;
  ciphertext_b64_decoded = base64_decode(ciphertext_b64, ciphertext_len/*strlen((char *)ciphertext_b64)*/, &rc_len);

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s {size_encoded:'%lu', size_decoded:'%d'}: b64-Decoded message...", __func__, ciphertext_len, rc_len);
#endif

  if (unlikely(ciphertext_b64_decoded == NULL)) {
    syslog(LOG_DEBUG, "%s: ERROR: COULD NOT b64-DECODE CIPHER TEXT...", __func__);

    goto ciphertext_b64_decode_error;
  }

  if (ciphertext_b64_decoded[0] != 1) {
    syslog(LOG_DEBUG, "%s: ERROR: WRONG PROTOCOL VERSION '%d' ", __func__, (unsigned char)ciphertext_b64_decoded[0]);

    goto ciphertext_b64_version_error;
  }

  //unsigned char *denc_out=calloc(rc_len+1, sizeof(unsigned char));//bit more than we need

  //retrieve embedded init vector
  unsigned char iv_denc[AES_BLOCK_SIZE] = {0};
  memcpy(iv_denc, ciphertext_b64_decoded + VERSION_LENGTH, AES_BLOCK_SIZE);

#if __UF_TESTING
  {
    //TODO: buffer overflow potential for large payloads>MBUF Dont use in production without compensating for that first
    unsigned char key_cipher_out[MBUF]={0}; unsigned char key_mac_out[MBUF]={0}; unsigned char iv_out[MBUF]={0};
    syslog(LOG_DEBUG, "%s: {key_cipher:'%s', key_mac:'%s', iv:'%s'}", __func__, hex_print(key_cipher, 32, key_cipher_out), hex_print(key_mac,20, key_mac_out), hex_print(iv_denc, 16, iv_out));
    //syslog(LOG_DEBUG, "%s: {cipher_text:'%s'}", __func__, hex_print(ciphertext_b64_decoded+AES_BLOCK_SIZE, ciphertext_len-AES_BLOCK_SIZE, NULL));
  }

#endif

size_t ciphertext_offset = rc_len - (VERSION_LENGTH + AES_BLOCK_SIZE + MAC_VALUE_SIZE);
  unsigned char *mac_provided = (ciphertext_b64_decoded + (ciphertext_offset + VERSION_LENGTH + AES_BLOCK_SIZE));
  unsigned char mac_provided_hex[MAC_VALUE_SIZE] = {0};
  syslog(LOG_DEBUG, "%s: {mac_provided_hex:'%s'}", __func__, hex_print(mac_provided, MAC_VALUE_SIZE, mac_provided_hex));

  //calculate our own mac
  unsigned char mac_calculated[MAC_VALUE_SIZE] = {0};
  ComputeHmacSha256(ciphertext_b64_decoded, rc_len - MAC_VALUE_SIZE, key_mac, MAC_KEY_SIZE, mac_calculated);
  {
    unsigned char mac_calculated_hex[MAC_VALUE_SIZE+1] = {0};

    syslog(LOG_DEBUG, "%s: {mac_calculated_hex:'%s'}", __func__, hex_print(mac_calculated, MAC_VALUE_SIZE, mac_calculated_hex));
  }

  if (!(CRYPTO_memcmp(mac_provided, mac_calculated, MAC_VALUE_SIZE) == 0)) {
    syslog(LOG_DEBUG, "%s: ERROR: : MAC MISMATCH: THIS MESSAGE IS POTENTIALLY CORRUPTED OR TAMPERED WITH", __func__);
    goto ciphertext_b64_mac_error;
  }

  unsigned char *denc_out = calloc(rc_len, sizeof(unsigned char));//bit more than we need

  AES_KEY denc_key;
  AES_set_decrypt_key(key_cipher, KEYLENGTH_AES256, &denc_key);

  AES_cbc_encrypt(ciphertext_b64_decoded + VERSION_LENGTH + AES_BLOCK_SIZE, denc_out, rc_len - (VERSION_LENGTH + AES_BLOCK_SIZE + MAC_VALUE_SIZE), &denc_key, iv_denc, AES_DECRYPT);

  //start cleaning up
  if (flag_b64encoded_key)	free(b64decoded_key);
  free (ciphertext_b64_decoded);

  DecryptedMessage *dec_ptr = calloc(1, sizeof(DecryptedMessage));
  dec_ptr->msg.msg_clear = denc_out;
  dec_ptr->size = rc_len-(VERSION_LENGTH+AES_BLOCK_SIZE+MAC_VALUE_SIZE);

  return dec_ptr;

  ciphertext_b64_version_error:
  ciphertext_b64_mac_error:
  free(ciphertext_b64_decoded);

  ciphertext_b64_decode_error:
  if (flag_b64encoded_key)	free (b64decoded_key);
  return NULL;

#if 0
  int rc_len=0;
  unsigned char key_cipher[CIPHER_KEY_SIZE+1];
  unsigned char key_mac[MAC_KEY_SIZE+1];
  unsigned char *b64decoded_key=NULL;

  if (flag_b64encoded_key)
  {
    b64decoded_key=base64_decode(key, strlen((char *)key), &rc_len);

    if (!b64decoded_key)	return NULL;
  }
  else
  {
    b64decoded_key=key;
  }

  if (rc_len<(CIPHER_KEY_SIZE+MAC_KEY_SIZE))
  {
    syslog(LOG_DEBUG, "%s: ERROR KEY LEN INVALID: '%d'", __func__, rc_len);

    return NULL;
  }

  //isolate the mac key and cipher key
  memcpy (key_cipher, b64decoded_key, CIPHER_KEY_SIZE);
  memcpy (key_mac, b64decoded_key+CIPHER_KEY_SIZE, MAC_KEY_SIZE);

  unsigned char *denc_out=calloc(ciphertext_len+1, sizeof(unsigned char));

  unsigned char *ciphertext_b64_decoded=NULL;
  ciphertext_b64_decoded=base64_decode(ciphertext_b64, strlen((char *)ciphertext_b64), &rc_len);

  if (unlikely(ciphertext_b64_decoded==NULL))
  {
    if (flag_b64encoded_key)	free (b64decoded_key);
    free (denc_out);

    syslog(LOG_DEBUG, "%s: ERROR: COULD NOT b64 DECODE CIPHER TEXT...", __func__);

    return NULL;
  }

  //retrieve embedded init vector
  unsigned char iv_denc[AES_BLOCK_SIZE]={0};
  memcpy(iv_denc, ciphertext_b64_decoded, AES_BLOCK_SIZE);

#ifdef __UF_TESTING
  {
    //TODO: buffer overflow potential for large payloads>MBUF Dont use in production without compensating for that first
    unsigned char key_cipher_out[MBUF]={0}; unsigned char key_mac_out[MBUF]={0}; unsigned char iv_out[MBUF]={0};
    syslog(LOG_DEBUG, "%s: {key_cipher:'%s', key_mac:'%s', iv:'%s'}", __func__, hex_print(key_cipher, 32, key_cipher_out), hex_print(key_mac,20, key_mac_out), hex_print(iv_denc, 16, iv_out));
    //syslog(LOG_DEBUG, "%s: {cipher_text:'%s'}", __func__, hex_print(ciphertext_b64_decoded+AES_BLOCK_SIZE, ciphertext_len-AES_BLOCK_SIZE, NULL));
  }

#endif


AES_KEY denc_key;
  AES_set_decrypt_key(key_cipher, KEYLENGTH_AES256, &denc_key);

  AES_cbc_encrypt(ciphertext_b64_decoded+AES_BLOCK_SIZE, denc_out, ciphertext_len-AES_BLOCK_SIZE, &denc_key, iv_denc, AES_DECRYPT);

  if (flag_b64encoded_key)	free(b64decoded_key);
  free (ciphertext_b64_decoded);

  DecryptedMessage *dec_ptr=malloc(sizeof(DecryptedMessage));
  dec_ptr->msg.msg_clear=denc_out;
  dec_ptr->size=ciphertext_len;

  return dec_ptr;
#endif
}

void
EncryptedMessageDestruct(EncryptedMessage*enc_ptr, bool flag_selfdestruct)
{
  if (enc_ptr->final_message)	free (enc_ptr->final_message);
  if (enc_ptr->final_message_b64)	free(enc_ptr->final_message_b64);
  if (enc_ptr->hmac)	free(enc_ptr->hmac);
  if(enc_ptr->msg.msg_b64)	free (enc_ptr->msg.msg_b64);

  memset(enc_ptr, 0, sizeof(EncryptedMessage));

  if (flag_selfdestruct) {
    free(enc_ptr);
    enc_ptr=NULL;
  }
}

void
DecryptedMessageDestruct(DecryptedMessage*denc_ptr, bool flag_selfdestruct)
{
  if (denc_ptr->final_message)	free (denc_ptr->final_message);
  if (denc_ptr->final_message_b64)	free(denc_ptr->final_message_b64);
  if (denc_ptr->hmac)	free(denc_ptr->hmac);
  if(denc_ptr->msg.msg_clear)	free (denc_ptr->msg.msg_clear);

  memset(denc_ptr, 0, sizeof(EncryptedMessage));

  if (flag_selfdestruct) {
    free(denc_ptr);
    denc_ptr=NULL;
  }
}


