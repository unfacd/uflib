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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE //for O_NOATIME
#endif

#include <uflib/standard_c_includes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <uflib/standard_defs.h>
#include <uflib/utils_file.h>

/**
 * @return -1 on error
 */
int
MakePidFile(const char *path, pid_t serverpid)
{
  int fd;
  int ret;
  char pid_str[10];//MAXPIDLEN
  struct flock lock;
  struct stat sb;

  if (stat(path, &sb) == 0)
  {
    /* file exists, perhaps previously kepts by SIGKILL */
    ret = unlink(path);
    if (ret == -1) {
      syslog(LOG_DEBUG, "%s: ERROR: Could not remove old PID-file path: %s", __func__, path);
      return -1;
    }
  }

  if ((fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0444)) < 0) {
    syslog(LOG_DEBUG, "%s: ERROR: COULD NOT CREATE PID-file path: %s", __func__, path);
    return -1;
  }

  /* create a write exclusive lock for the entire file */
  lock.l_type = F_WRLCK;
  lock.l_start = 0;
  lock.l_whence = SEEK_SET;
  lock.l_len = 0;

  if (fcntl(fd, F_SETLK, &lock) < 0) {
    close(fd);

    syslog(LOG_DEBUG, "%s: ERROR: COULD NOT SET LOCK ON PID-file path: %s", __func__, path);

    return -1;
  }

  sprintf(pid_str, "%i", serverpid);
  ssize_t write_len = strlen(pid_str);

  if (write(fd, pid_str, write_len) != write_len) {
    close(fd);
    syslog(LOG_DEBUG, "%s: ERROR: COULD NOT WRITE OUT PID:'%s' TO PID-file path: %s", __func__, pid_str, path);

    return -1;
  }

  close(fd);

  return 0;
}

int RemovePidFile(const char *path)
{
  if (unlink(path)) {
    syslog(LOG_DEBUG, "%s: COULD NOT REMOVE PID FILE: '%s'", __func__, path);

    return -1;
  }

  return 0;
}

int GetFileInfo(const char *path, FileInfo *f_info, int mode)
{
  gid_t EGID;
  gid_t EUID;
  struct stat f, target;

  EUID = geteuid();
  EGID = getegid();

  f_info->exists = false;

  if (lstat(path, &f) == -1) {
    if (errno == EACCES) {
      f_info->exists = true;
    }
    return -1;
  }

  f_info->exists = true;
  f_info->is_file = true;
  f_info->is_link = false;
  f_info->is_directory = false;
  f_info->exec_access = false;
  f_info->read_access = false;

  if (S_ISLNK(f.st_mode)) {
    f_info->is_link = true;
    f_info->is_file = false;
    if (stat(path, &target) == -1) {
      return -1;
    }
  } else {
    target = f;
  }

  f_info->size = target.st_size;
  f_info->last_modification = target.st_mtime;

  if (S_ISDIR(target.st_mode)) {
    f_info->is_directory = true;
    f_info->is_file = false;
  }

  if (mode & FILE_READ) {
    if (((target.st_mode & S_IRUSR) && target.st_uid == EUID) ||
        ((target.st_mode & S_IRGRP) && target.st_gid == EGID) ||
        (target.st_mode & S_IROTH)) {
      f_info->read_access = true;
    }
  }

  if (mode & FILE_EXEC) {
    if ((target.st_mode & S_IXUSR && target.st_uid == EUID) ||
        (target.st_mode & S_IXGRP && target.st_gid == EGID) ||
        (target.st_mode & S_IXOTH)) {
      f_info->exec_access = true;
    }
  }

  f_info->flags_read_only = O_RDONLY | O_NONBLOCK;

#if defined(__linux__)
  /*
   * If the user is the owner of the file or the user is root, it
   * can set the O_NOATIME flag for open(2) operations to avoid
   * inode updates about last accessed time
   */
  if (target.st_uid == EUID || EUID == 0) {
    f_info->flags_read_only |= O_NOATIME;
  }
#endif

  return 0;
}

/* Read file content to a memory buffer,
 * Use this function just for really SMALL files
 */
char *LoadFileToMemory(const char *path)
{
  FILE *fp;
  char *buffer;
  long bytes;
  struct FileInfo finfo;

  if (GetFileInfo(path, &finfo, FILE_READ) != 0) {
    return NULL;
  }

  if (!(fp = fopen(path, "r"))) {
    return NULL;
  }

  buffer = calloc(1, (finfo.size + 1));
  if (!buffer) {
    fclose(fp);
    return NULL;
  }

  bytes = fread(buffer, finfo.size, 1, fp);

  if (bytes < 1) {
    free(buffer);
    fclose(fp);

    return NULL;
  }

  fclose(fp);

  return (char *) buffer;

}

/**
 * @brief  Move file across two locations
 *
 * @return 0: on success
 *
 */
int FileUtilsRenameFile(const char *orig, const char *dest)
{
  int ok = rename(orig, dest);

  if (ok != 0 && errno == EXDEV) { 	// Ok, old way, open both, copy
    //- EXDEV The two file names newname and oldname are on different file systems
    syslog(LOG_DEBUG, "NOTICE: 'rename() returned 'EXDEV': Performing slow rename...");
    ok = 0;
    int fd_orig = open(orig, O_RDONLY);
    if (fd_orig < 0) {
      ok = 1;
      syslog(LOG_DEBUG, "ERROR: Could not open destination for writing (%s)", strerror(errno));
    }
    int fd_dest = open(dest, O_WRONLY |O_CREAT | O_TRUNC, 0666);
    if (fd_dest < 0) {
      ok = 1;
      syslog(LOG_DEBUG, "ERROR: Could not open orig for reading (%s)", strerror(errno));
    }
    if (ok == 0) {
      char tmp[4096];
      int r;
      while ((r = read(fd_orig, tmp, sizeof(tmp))) > 0 ) {
        r = write(fd_dest, tmp, r);
        if (r < 0) {
          syslog(LOG_DEBUG, "ERROR: Could not write to destination file (%s)", strerror(errno));
          ok = 1;
          break;
        }
      }
    }
    if (fd_orig >= 0) {
      close(fd_orig);
      unlink(orig);
    }

    if (fd_dest >= 0)
      close(fd_dest);
  }

  return ok;

}

int OpenThisFile(OpenFile *file)
{
  struct stat st;

  if (!(file->fp = fopen(file->filename, "r"))) {
    file->stat = errno;

    return -1;
  }

  if ((stat(file->filename, &st)) < 0) {
    file->stat = errno;

    return -2;
  }

  if (file->size = st.st_size, file->size == 0) {
    file->stat = errno;

    return -3;
  }

  {
    if (!(file->conts = malloc(file->size + 1))) {
      return -4;
    }

    if ((read(fileno(file->fp), file->conts, file->size)) < 0) {
      file->stat = errno;

      return -5;
    }
  }

  return 1;

}  /**/

#include <sys/vfs.h> //<sys/statfs.h>
ssize_t FileSystemBlockSizeGet	(int fd)
{
  struct statfs st;
  if (fstatfs(fd, &st) != -1) {
    return (ssize_t) st.f_bsize;
  }

  return -1;
}

#include <sys/mman.h>
//using mmap
ssize_t mmap_write(const char *path, int in, int out)
{
  ssize_t w = 0, n;
  size_t len;
  char *p;
  struct FileInfo finfo;
  if (GetFileInfo(path, &finfo, FILE_READ) != 0) {
    return -1;
  }

  len = finfo.size;
  if ((p = mmap(NULL, len, PROT_READ, MAP_SHARED, in, 0))) {

    while (w < len && (n = write(out, p + w, (len - w)))) {
      if (n == -1) {
        if (errno == EINTR) continue;
        else goto error_exit;
      }
      w += n;
    }

    munmap(p, len);

    return w;
  }

  error_exit:
  munmap(p, len);
  return -1;
}

//get bs from FileSystemBlockSizeGet()
ssize_t read_write_bs(const char *path, int in, int out, ssize_t bs)
{
  ssize_t w = 0, r = 0, t, n, m;
  struct FileInfo finfo;

  if (GetFileInfo(path, &finfo, FILE_READ) != 0) {
    return -1;
  }

  char *buf = malloc(bs);
  if (unlikely(buf == NULL))	return -1;

  t = finfo.size;

  while(r < t && (n = read(in, buf, bs))) {
    if(n == -1) {
      if (errno == EINTR)	continue;
      else	goto exit_error;
    }

    r = n;
    w = 0;
    while (w < r && (m = write(out, buf + w, (r - w)))) {
      if (m == -1) {
        if (errno == EINTR)	continue;
        else	goto exit_error;
      }
      w += m;
    }
  }

  free(buf);

  return w;

  exit_error:
  free(buf);
  return -1;

}

#include <sys/sendfile.h>
ssize_t SendFile(const char *path, int in, int out)
{
  struct FileInfo finfo;
  if (GetFileInfo(path, &finfo, FILE_READ) != 0) {
    return -1;
  }

  posix_fadvise(in, 0, finfo.size, POSIX_FADV_WILLNEED);
  posix_fadvise(in, 0, finfo.size, POSIX_FADV_SEQUENTIAL);

  //tell the kernel we are writing finfo.size bytes to disk now, dont care about being full
  //if ((fallocate(out, 0, 0, finfo.size)==-1) && (errno==EOPNOTSUPP))
  //ftruncate(out, finfo.size);

  ssize_t	t	= finfo.size;
  off_t	ofs = 0;

  while (ofs < t) {
    if (sendfile(out, in, &ofs, t - ofs) == -1) {
      if (errno == EINTR)	continue;
      else 				return -1;
    }
  }

  return t;
}

