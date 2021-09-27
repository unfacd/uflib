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

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
//#include <uflib/standard_ssl_includes.h>
#include <sys/stat.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <uflib/utils.h>

/**
 * @brief Set the thread name for the current calling thread. This gives the thread a descriptive name when looked up
 * in system listing and gdb. There is currently max size lime of 15-bytes.
 * @param thread_name desired name of thread
 * @return 0 on success as per prctl() system call.
 */
int  __attribute__((nonnull(1), access(read_only, 1)))
SetThreadName(const char *thread_name)
{
#define MAX_NAME_LEN 15
  char proc_name [MAX_NAME_LEN + 1] = {0};	/* Name must be <= 15 characters + a null */

  strncpy(proc_name, thread_name, MAX_NAME_LEN);
  int result = prctl(PR_SET_NAME, (unsigned long)&proc_name);

  return result;
#undef MAX_NAME_LEN
}

//http://www.shieldadvanced.com/Blog/uncategorized/validate-email-address-in-c/
//https://stackoverflow.com/questions/42939688/phone-number-validation-in-c
bool
IsEmailAddressValid(const char *EM_Addr, size_t max_sz)
{
  int count = 0;
  int i = 0;
//  char conv_buf[MAX_EMAIL_NAME];
  char *conv_buf = strndupa(EM_Addr, max_sz);
  char *c, *domain;
  char *special_chars = "()<>@,;:\"[]";

/* The input is in EBCDIC so convert to ASCII first */
//  strcpy(conv_buf,EM_Addr);
//  EtoA(conv_buf);
///* convert the special chars to ASCII */
//  EtoA(special_chars);

  for(c = conv_buf; *c; c++) {
    /* if '"' and beginning or previous is a '.' or '"' */
    if (*c == 34 && (c == conv_buf || *(c - 1) == 46 || *(c - 1) == 34)) {
      while (*++c) {
        /* if '"' break, End of name */
        if (*c == 34)
          break;
        /* if '' and ' ' */
        if (*c == 92 && (*++c == 32))
          continue;
        /* if not between ' ' & '~' */
        if (*c <= 32 || *c > 127)
          return 0;
      }
      /* if no more characters error */
      if (!*c++)
        return 0;
      /* found '@' */
      if (*c == 64)
        break;
      /* '.' required */
      if (*c != 46)
        return 0;
      continue;
    }
    if (*c == 64) {
      break;
    }
    /* make sure between ' ' && '~' */
    if (*c <= 32 || *c > 127) {
      return 0;
    }
    /* check special chars */
    if (strchr(special_chars, *c)) {
      return 0;
    }
  } /* end of for loop */
/* found '@' */
/* if at beginning or previous = '.' */
  if (c == conv_buf || *(c - 1) == 46)
    return 0;
/* next we validate the domain portion */
/* if the next character is NULL */
/* need domain ! */
  if (!*(domain = ++c))
    return 0;
  do {
    /* if '.' */
    if (*c == 46) {
      /* if beginning or previous = '.' */
      if (c == domain || *(c - 1) == 46)
        return 0;
      /* count '.' need at least 1 */
      count++;
    }
    /* make sure between ' ' and '~' */
    if (*c <= 32 || *c >= 127)
      return 0;
    if (strchr(special_chars, *c))
      return 0;
  } while (*++c); /* while valid char */
  return (count >= 1); /* return true if more than 1 '.' */
}

/**
 *	similar to asprintf. Also check sprintf_provided_buffer()
 *	@dynamic_memory: EXPORTS char * which the user must deallocate
 */
char * mdsprintf(const char * message, ...)
{
  va_list argp, argp_cpy;
  size_t out_len = 0;
  char * out = NULL;

  va_start(argp, message);
  va_copy(argp_cpy, argp);
  out_len = vsnprintf(NULL, 0, message, argp);
  out = malloc(out_len + sizeof(char));

  if (IS_EMPTY(out)) {
    return NULL;
  }

  vsnprintf(out, (out_len+sizeof(char)), message, argp_cpy);
  va_end(argp);
  va_end(argp_cpy);

  return out;
}

inline void *mymalloc(size_t size)
{
	void *ret = calloc(1, size);
	if(ret == NULL) _exit(-1);
	return (ret);
}

/*
 * 	@brief: for 32-bit numbers
 */
__attribute__((noinline, unused)) unsigned next_pow2(unsigned x)
{
	if (x==0) return 1;
	x -= 1;
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);

	return x + 1;
}

 /* see the definition of macro splitw */
 char *tokenize (char **str, const char c)

 {
  register char *s,
                *p;

    if ((!str)||(!*str))  return *str="";

   s=*str;

    while (*s==' ')  s++;

    if ((p=strchr(s, c)))
     {
      *str=(*p++=0, p);

      return (char *)s;
     }

   *str=0;

   return (char *)s;

 }  /**/

inline bool
IsPrimeNumber(size_t x)
{
    size_t o=4;
    size_t i=5;

    for (i=5; ;i+=o)
    {
        size_t q=x/i;

        if (q < i)	return true;

        if (x == q * i)	return false;

        o ^= 6;
    }

    return true;
}

inline size_t
GetNextPrimeNumber(size_t x)
{
    switch (x)
    {
    case 0:
    case 1:
    case 2:
        return 2;
    case 3:
        return 3;
    case 4:
    case 5:
        return 5;
    }

    size_t k = x / 6;
    size_t i = x - 6 * k;
    size_t o = i < 2 ? 1 : 5;//all numbers which are divisible by neither 2 nor 3

    x = 6 * k + o;

    for (i=(3+o)/2; !IsPrimeNumber(x); x+=i)
        i ^= 6;

    return x;
}

#include <uflib/utils_crypto.h>

/**
 * 	@brief: The onlyrequirement is the user supply a text based password
 * 	@return 0: on success with the computed has saved in creds_ptr->hashed_password
 * 	@returns -1: on error
 * 	@dynamic_memory: ALLOCATES a char * which the user must free
 */
int GeneratePasswordHash(UserCredentials *creds_ptr)
{
	if (!creds_ptr || !creds_ptr->password || *(creds_ptr->password)==0)
	{
		syslog (LOG_DEBUG, "%s: ERROR: undefined parameters...", __func__);

		return -1;
	}

	unsigned char *salt = GenerateSalt(32, 1);

	if (salt) {
		char *password_salted=NULL;

		creds_ptr->salt=salt;

		asprintf(&password_salted, "%s%s", salt, creds_ptr->password);
		if (password_salted) {
			unsigned char password_hashed[SHA_DIGEST_LENGTH*2+1];
			memset (password_hashed, 0, sizeof(password_hashed));

			ComputeSHA1((unsigned char *)password_salted, strlen(password_salted), (char *)password_hashed, sizeof(password_hashed), 1);

			syslog(LOG_DEBUG, "%s: GENERATED INPUT SALTED PASSWORD: '%s' FINAL HASHED PASSWORD: '%s", __func__, password_salted, password_hashed);

			creds_ptr->hashed_password=(unsigned char *)strndup((char *)password_hashed, sizeof(password_hashed));

#if __UF_FULLDEBUG
			syslog(LOG_DEBUG, "%s: GENERATED '%s'", salted_password);
#endif
			free (password_salted);
		}

		return 0;//no error
	}

	return -1;
}

/**
 * 	@brief: classic user authentication, using salted password.
 *
 * 	@dynamic_memory: INTERNALLY ALLOCATES char * and frees it
 */
bool
IsPasswordCorrect(const char *password, const char *token, const char *salt)
{
	char *password_salted=NULL;
	unsigned char password_hashed[SHA_DIGEST_LENGTH*2+1];

	memset (password_hashed, 0, sizeof(password_hashed));

	asprintf(&password_salted, "%s%s", salt, password);

	ComputeSHA1 ((unsigned char *)password_salted, strlen(password_salted), (char *)password_hashed, sizeof(password_hashed), 1);

	syslog(LOG_DEBUG, "%s: GENERATED INPUT SALTED PASSWORD: '%s' FINAL HASHED PASSWORD: '%s", __func__, password_salted, password_hashed);

	if (strcmp((char *)password_hashed, token) == 0) {
		free(password_salted);

		return true;
	}
	else
	{
		free(password_salted);

		return false;
	}

	return false;

}

/**
 * 	@brief:
	@dynamic_memory: ALLOCATES char * which the user responsible for freeing
 */
char *
GenerateCookie(size_t max_sz)
{
	unsigned char *salt = GenerateSalt(max_sz, 1);

	if (salt) {
		return (char *)salt;
	}

	return NULL;
}

//code size=7
/**
 * 	@dynamic_memory: ALLOCATES char * whih the user is responsible for freeing
 */
int
GenerateVerificationCode(VerificationCode *code)
{
	char *verification_code_str=NULL;

  verification_code_str=code->code_formatted;
  code->code  = GenerateRandomNumberBounded(100000, 999999);
//  code->code=99000 + GenerateRandomNumberWithUpper(900000);
  snprintf(verification_code_str, 8, "%lu", code->code); //8 xxx-xxx\0

  //last three bytes
  char save4=verification_code_str[3];//xxxXxx
  char save5=verification_code_str[4];//xxxXx
  char save6=verification_code_str[5];//xxxxX

  verification_code_str[3]='-';
  verification_code_str[4]=save4;
  verification_code_str[5]=save5;
  verification_code_str[6]=save6;
  verification_code_str[7]='\0';

  syslog(LOG_DEBUG, "%s: GENERATED VERIFIATION CODE '%lu' -> '%s'", __func__, code->code, verification_code_str);

	return 0;

}

//------- ATOMIC

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define PAUSE()        __asm__ __volatile__ ("pause");
#else
#define PAUSE()  /* do nothing */
#endif

#if defined(__linux__) || defined(BSD)
#include <sched.h>
#define THREAD_YIELD   sched_yield
#else
#error "Unknown hybrid yield implementation"
#endif

void
DoBusyWait(size_t counter)
{
		if (counter < 10) {
						/* Spin-wait */
						PAUSE();
		} else if (counter < 20) {
				/* A more intense spin-wait */
				int  i;
				for (i = 0; i < 50; i++) {
						PAUSE();
				}
		} else if (counter < 22) {
						THREAD_YIELD();
		} else if (counter < 24) {
						usleep(0);
		} else if (counter < 26) {
						usleep(1);
		} else {
			usleep((counter - 25) * 10);
		}
}
//

//---------------------------------------------------------
//
///**
// * @return -1 on error
// */
//int MakePidFile(const char *path, pid_t serverpid)
//{
//    int fd;
//    int ret;
//    char pid_str[MAXPIDLEN];
//    struct flock lock;
//    struct stat sb;
//
//    if (stat(path, &sb) == 0)
//    {
//        /* file exists, perhaps previously kepts by SIGKILL */
//        ret = unlink(path);
//        if (ret == -1)
//        {
//            syslog(LOG_DEBUG, "%s: ERROR: Could not remove old PID-file path: %s", __func__, path);
//            return -1;
//        }
//    }
//
//    if ((fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0444)) < 0)
//    {
//    	syslog(LOG_DEBUG, "%s: ERROR: COULD NOT CREATE PID-file path: %s", __func__, path);
//        return -1;
//    }
//
//    /* create a write exclusive lock for the entire file */
//    lock.l_type = F_WRLCK;
//    lock.l_start = 0;
//    lock.l_whence = SEEK_SET;
//    lock.l_len = 0;
//
//    if (fcntl(fd, F_SETLK, &lock) < 0)
//    {
//        close(fd);
//
//        syslog(LOG_DEBUG, "%s: ERROR: COULD NOT SET LOCK ON PID-file path: %s", __func__, path);
//
//        return -1;
//    }
//
//    sprintf(pid_str, "%i", serverpid);
//    ssize_t write_len = strlen(pid_str);
//
//    if (write(fd, pid_str, write_len) != write_len)
//    {
//        close(fd);
//        syslog(LOG_DEBUG, "%s: ERROR: COULD NOT WRITE OUT PID:'%s' TO PID-file path: %s", __func__, pid_str, path);
//
//        return -1;
//    }
//
//    close(fd);
//
//    return 0;
//}
//
//
//int RemovePidFile(const char *path)
//{
//    if (unlink(path))
//    {
//        syslog(LOG_DEBUG, "%s: COULD NOT REMOVE PID FILE: '%s'", __func__, path);
//
//        return -1;
//    }
//
//    return 0;
//}
//
//
//int GetFileInfo(const char *path, FileInfo *f_info, int mode)
//{
//	gid_t EGID;
//	gid_t EUID;
//    struct stat f, target;
//
//    EUID = geteuid();
//	EGID = getegid();
//
//    f_info->exists = false;
//
//    if (lstat(path, &f) == -1)
//    {
//        if (errno == EACCES)
//        {
//            f_info->exists = true;
//        }
//        return -1;
//    }
//
//    f_info->exists = true;
//    f_info->is_file = true;
//    f_info->is_link = false;
//    f_info->is_directory = false;
//    f_info->exec_access = false;
//    f_info->read_access = false;
//
//    if (S_ISLNK(f.st_mode))
//    {
//        f_info->is_link = true;
//        f_info->is_file = false;
//        if (stat(path, &target) == -1)
//        {
//            return -1;
//        }
//    }
//    else
//    {
//        target = f;
//    }
//
//    f_info->size = target.st_size;
//    f_info->last_modification = target.st_mtime;
//
//    if (S_ISDIR(target.st_mode))
//    {
//        f_info->is_directory = true;
//        f_info->is_file = false;
//    }
//
//    if (mode & FILE_READ)
//    {
//        if (((target.st_mode & S_IRUSR) && target.st_uid == EUID) ||
//            ((target.st_mode & S_IRGRP) && target.st_gid == EGID) ||
//            (target.st_mode & S_IROTH))
//        {
//            f_info->read_access = true;
//        }
//    }
//
//    if (mode & FILE_EXEC)
//    {
//        if ((target.st_mode & S_IXUSR && target.st_uid == EUID) ||
//            (target.st_mode & S_IXGRP && target.st_gid == EGID) ||
//            (target.st_mode & S_IXOTH))
//        {
//            f_info->exec_access = true;
//        }
//    }
//
//    // Suggest open(2) flags
//    f_info->flags_read_only = O_RDONLY | O_NONBLOCK;
//
//#if defined(__linux__)
//    /*
//     * If the user is the owner of the file or the user is root, it
//     * can set the O_NOATIME flag for open(2) operations to avoid
//     * inode updates about last accessed time
//     */
//    if (target.st_uid == EUID || EUID == 0) {
//        f_info->flags_read_only |=  O_NOATIME;
//    }
//#endif
//
//    return 0;
//}
//
///* Read file content to a memory buffer,
// * Use this function just for really SMALL files
// */
//char *LoadFileToMemory(const char *path)
//{
//    FILE *fp;
//    char *buffer;
//    long bytes;
//    struct FileInfo finfo;
//
//    if (GetFileInfo(path, &finfo, FILE_READ) != 0)
//    {
//        return NULL;
//    }
//
//    if (!(fp = fopen(path, "r")))
//    {
//        return NULL;
//    }
//
//    buffer = calloc(1, (finfo.size + 1));
//    if (!buffer)
//    {
//        fclose(fp);
//        return NULL;
//    }
//
//    bytes = fread(buffer, finfo.size, 1, fp);
//
//    if (bytes < 1)
//    {
//        free(buffer);
//        fclose(fp);
//
//        return NULL;
//    }
//
//    fclose(fp);
//
//    return (char *) buffer;
//
//}
//
//
///**
// * @brief  Move file across two locations
// *
// * @return 0: on success
// *
// */
//int FileUtilsRenameFile(const char *orig, const char *dest)
//{
//	int ok=rename(orig, dest);
//
//	if (ok!=0 && errno==EXDEV)
//	{ 	// Ok, old way, open both, copy
//		//- EXDEV The two file names newname and oldname are on different file systems
//		syslog(LOG_DEBUG, "NOTICE: 'rename() returned 'EXDEV': Performing slow rename...");
//		ok=0;
//		int fd_dest=open(dest, O_WRONLY|O_CREAT, 0666);
//		if (fd_dest<0)
//		{
//			ok=1;
//			syslog(LOG_DEBUG, "ERROR: Could not open destination for writing (%s)", strerror(errno));
//		}
//		int fd_orig=open(orig, O_RDONLY);
//		if (fd_dest<0)
//		{
//			ok=1;
//			syslog(LOG_DEBUG, "ERROR: Could not open orig for reading (%s)", strerror(errno));
//		}
//		if (ok==0)
//		{
//			char tmp[4096];
//			int r;
//			while ( (r=read(fd_orig, tmp, sizeof(tmp))) > 0 )
//			{
//				r=write(fd_dest, tmp, r);
//				if (r<0)
//				{
//					syslog(LOG_DEBUG, "ERROR: Could not write to destination file (%s)", strerror(errno));
//					ok=1;
//					break;
//				}
//			}
//		}
//		if (fd_orig>=0)
//		{
//			close(fd_orig);
//			unlink(orig);
//		}
//
//		if (fd_dest>=0)
//			close(fd_dest);
//	}
//
//	return ok;
//
//}
//
//
//int OpenThisFile(OpenFile *file)
//
//{
// struct stat st;
//
//   if (!(file->fp=fopen(file->filename, "r")))
//    {
//     file->stat=errno;
//
//     return -1;
//    }
//
//   if ((stat(file->filename, &st))<0)
//    {
//     file->stat=errno;
//
//     return -2;
//    }
//
//   if (file->size=st.st_size, file->size==0)
//    {
//     file->stat=errno;
//
//     return -3;
//    }
//
//  {
//     if (!(file->conts=malloc(file->size+1)))
//      {
//       return -4;
//      }
//
//     if ((read(fileno(file->fp), file->conts, file->size))<0)
//      {
//       file->stat=errno;
//
//       return -5;
//      }
//  }
//
//  return 1;
//
//}  /**/
//
//
//#include <sys/vfs.h> //<sys/statfs.h>
//ssize_t FileSystemBlockSizeGet	(int fd)
//{
//    struct statfs st;
//    if (fstatfs(fd, &st) != -1)
//	{
//		return (ssize_t) st.f_bsize;
//	}
//
//	return -1;
//}
//
//
//#include <sys/mman.h>
////using mmap
//ssize_t mmap_write(const char *path, int in, int out)
//{
//    ssize_t w = 0, n;
//    size_t len;
//    char *p;
//    struct FileInfo finfo;
//	if (GetFileInfo(path, &finfo, FILE_READ) != 0)
//	{
//		return -1;
//	}
//
//    len = finfo.size;
//    if ((p=mmap(NULL, len, PROT_READ, MAP_SHARED, in, 0)))
//    {
//
//		while(w < len && (n = write(out, p + w, (len - w))))
//		{
//			if(n == -1)
//			{
//				if (errno == EINTR) continue;
//				else goto error_exit;
//			}
//			w += n;
//		}
//
//		munmap(p, len);
//
//		return w;
//    }
//
//    error_exit:
//	munmap(p, len);
//	return -1;
//}
//
////get bs from FileSystemBlockSizeGet()
//ssize_t read_write_bs(const char *path, int in, int out, ssize_t bs)
//{
//    ssize_t w = 0, r = 0, t, n, m;
//    struct FileInfo finfo;
//
//    if (GetFileInfo(path, &finfo, FILE_READ) != 0)
//	{
//		return -1;
//	}
//
//    char *buf = malloc(bs);
//    if (unlikely(buf==NULL))	return -1;
//
//    t = finfo.size;
//
//    while(r < t && (n = read(in, buf, bs)))
//    {
//        if(n == -1)
//        {
//        	if (errno == EINTR)	continue;
//			else	goto exit_error;
//        }
//
//        r = n;
//        w = 0;
//        while(w < r && (m = write(out, buf + w, (r - w))))
//        {
//            if(m == -1)
//            {
//            	if (errno==EINTR)	continue;
//            	else	goto exit_error;
//            }
//            w += m;
//        }
//    }
//
//    free(buf);
//
//    return w;
//
//    exit_error:
//	free(buf);
//	return -1;
//
//}
//
//
//#include <sys/sendfile.h>
//ssize_t SendFile(const char *path, int in, int out)
//{
//	struct FileInfo finfo;
//	if (GetFileInfo(path, &finfo, FILE_READ) != 0)
//	{
//		return -1;
//	}
//
//	posix_fadvise(in, 0, finfo.size, POSIX_FADV_WILLNEED);
//	posix_fadvise(in, 0, finfo.size, POSIX_FADV_SEQUENTIAL);
//
//	//tell the kernel we are writing finfo.size bytes to disk now, dont care about being full
//	//if ((fallocate(out, 0, 0, finfo.size)==-1) && (errno==EOPNOTSUPP))
//    //ftruncate(out, finfo.size);
//
//    ssize_t	t	= finfo.size;
//    off_t	ofs = 0;
//
//    while (ofs<t)
//    {
//        if(sendfile(out, in, &ofs, t - ofs) == -1)
//        {
//            if (errno==EINTR)	continue;
//            else 				return -1;
//        }
//    }
//
//    return t;
//}

#ifndef _CONFIGDEFAULT_ETAG_SIZE
# define _CONFIGDEFAULT_ETAG_SIZE 32
#endif

void
GenerateEtag(struct stat *st, char etag[_CONFIGDEFAULT_ETAG_SIZE])
{
	size_t size = st->st_size;
	unsigned int	time	= st->st_mtime;

	snprintf(etag, _CONFIGDEFAULT_ETAG_SIZE, "%04X-%04X",(int32_t)size,(int32_t)time);
}

//linux kernel sourced
static inline unsigned long  _hash_64(unsigned long val, unsigned int bits);
/*  2^63 + 2^61 - 2^57 + 2^54 - 2^51 - 2^18 + 1 */
#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001UL
#define BITS_PER_LONG 64

__attribute__((const)) static inline unsigned long
_hash_64(unsigned long val, unsigned int bits)
{
	unsigned long hash = val;

#if defined(CONFIG_ARCH_HAS_FAST_MULTIPLIER) && BITS_PER_LONG == 64
	hash = hash * GOLDEN_RATIO_PRIME_64;
#else
	/*  gcc can't optimise this alone like it does for 32 bits. */
	unsigned long n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;
#endif

	/* High bits are more random, so use them. */
	return hash >> (64 - bits);
}

/**
 * 	@param m: desired unsignedlong 64 value to be hashed
 * 	@param len: use 1024
 */
unsigned long
do_hash(unsigned long m, size_t table_sz)
{
	unsigned long i;
	unsigned long x = 0;

		x = _hash_64(m, BITS_PER_LONG);

	return x%table_sz;

}

//http://web.archive.org/web/20071223173210/http://www.concentric.net/~Ttwang/tech/inthash.htm
__attribute__((const))  uint64_t
inthash_u64 (uint64_t key, size_t key_len)
{
	key = ~key + (key << 21);
	key = key ^ (key >> 24);
	key = key + (key << 3) + (key << 8);
	key = key ^ (key >> 14);
	key = key + (key << 2) + (key << 4);
	key = key ^ (key >> 28);
	key = key + (key << 31);

	return key;
}

