/*
** utils.h Copyright (c) 1999 Ayman Akt
**
** See the COPYING file for terms of use and conditions.
**
MODULEID("$Id: utils.h,v 1.1 1999/07/26 01:46:59 ayman Exp $")
**
*/
#ifndef __INCLUDE_FILE_UTILS__H__
#define __INCLUDE_FILE_UTILS__H__

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "standard_c_includes.h"
#include "utils_base64.h"
#include "utils_time.h"
#include "utils_file.h"
#include <sys/time.h>

enum AccountRegoStatus {
	REGOSTATUS_UNKNOWN		=	0,
	REGOSTATUS_PENDING		=	1, //pre- or re-registration with verification code issued, but not verified
	REGOSTATUS_ACTIVE			=	2,
	REGOSTATUS_INACTIVE		=	3,
	REGOSTATUS_SUSPENDED	=	4,
	REGOSTATUS_VERIFIED   = 5 //user verified, but not active yet (logged on with signon cookie)
};

typedef struct UserCredentials {
	unsigned char *username;
	unsigned char *password;
	unsigned char *salt;
	unsigned char *hashed_password;
	unsigned char *e164number; //todo: this may need to be removed in the future
	enum AccountRegoStatus rego_status;

} UserCredentials;

//static initialiser
#define _USERCREDENTIALS_INIT(x) \
		x.username=NULL;x.password=NULL;x.salt=NULL;x.hashed_password=NULL; x.rego_status=REGOSTATUS_UNKNOWN

#ifndef CONFIG_MAX_VERIFICATION_CODE_FORMATTED_SZ
#define CONFIG_MAX_VERIFICATION_CODE_FORMATTED_SZ 7
#endif

typedef struct VerificationCode {
	unsigned long code;
	char code_formatted[CONFIG_MAX_VERIFICATION_CODE_FORMATTED_SZ + 1]; //extra for '\0'
} VerificationCode;

//typedef struct FileInfo
//{
//    size_t size;
//    time_t last_modification;
//
//    /* Suggest flags to open this file */
//    int flags_read_only;
//
//    bool exists;
//    bool is_file;
//    bool is_link;
//    bool is_directory;
//    bool exec_access;
//    bool read_access;
//} FileInfo;
//
// typedef struct LogFile {
//          FILE *file;
//          char *fname;
//         } LogFile;
//
// struct OpenFile {
//  const char *filename;
//  char *conts; /* contents of the file */
//  FILE *fp;
//  size_t size;
//  unsigned stat; /* errno information */
// };
// typedef struct OpenFile OpenFile;

 #define splitw(x) tokenize((x), ' ')

 /* needs GCC */
 #define isdigits(x) ({ \
                      int numbered=1; \
                      register int n=strlen(x)-1; \
                                         \
                        while (n>=0) \
                         {  \
                           if ((x[n]<'0')||(x[n]>'9')) \
                            { \
                             numbered=0; \
                             break; \
                            } \
                          n--; \
                         } \
                                \
                       (numbered?1:0); \
                     })

#define _max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define _min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _b : _a; })

 static inline int
 isPowerOfTwo(unsigned int x)
 {
	 return ((x != 0) && !(x & (x - 1)));
 }

bool IsEmailAddressValid(const char *EM_Addr, size_t max_sz);

 //http://locklessinc.com/articles/next_pow2/
__attribute__((noinline)) unsigned next_pow2(unsigned x);
char * mdsprintf(const char * message, ...) __attribute__ ((format (printf, 1, 2)));

int SetThreadName(const char *thread_name);
void *mymalloc(size_t);

 unsigned char *mystrndup(const unsigned char *, size_t);
 char *io_error (int);

 char *tokenize (char **, const char);

 static inline double
 GetRandomFromRangeInDoubles (double x0, double x1) {
	 return x0 + (x1 - x0) * rand() / ((double) RAND_MAX);
 }

bool IsPrimeNumber (size_t x);
 size_t GetNextPrimeNumber (size_t x);

 void SeedRandom(struct timeval *);
 int GeneratePasswordHash(UserCredentials *creds_ptr);
 int GenerateVerificationCode(VerificationCode *);
 bool IsPasswordCorrect(const char *password, const char *token, const char *salt);
 char *GenerateCookie(size_t max_sz);

 void DoBusyWait(size_t counter);

#ifndef _CONFIGDEFAULT_ETAG_SIZE
# define _CONFIGDEFAULT_ETAG_SIZE 32
#endif
 void GenerateEtag(struct stat *st, char etag[_CONFIGDEFAULT_ETAG_SIZE]);

 uint64_t inthash_u64(uint64_t key, size_t key_len);

#endif
