/**
 * Copyright (C) 2015-2023 unfacd works
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/standard_c_includes.h>
#include <uflib/utils_sys.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>

/**
 * @brief Assuming the process was started as root, drop user privileges to @param username and chroot if
 * provided.
 * @param username system user name to change to
 * @param chroot_dir
 * @return 0 on success,
 */
int
DropRootPrivileges(const char *username, const char *chroot_dir, void(^on_success)(void))
{
  if (chroot_dir && !username) {
    syslog(LOG_WARNING, "chroot without dropping user privileges...");
  }

  struct passwd *pw = getpwnam(username);
  if (pw) {
    if (chroot_dir) {
      if (chroot(chroot_dir) != 0 || chdir("/") != 0) {
        syslog(LOG_ERR, "ERROR: COULD NOT CHROOT '%s' FOR '%s'. ERR: '%s'", chroot_dir, username, strerror(errno));
        return  errno;
      }
    }
    if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
      syslog(LOG_ERR, "ERROR: COULD NOT CHANGE system privileges (uid=%lu gid=%lu) for: '%s'. ERR: '%s' ", (unsigned long)pw->pw_uid, (unsigned long)pw->pw_gid, username, strerror(errno));
      return errno;
    }

    return_success:
    on_success();
    return 0;
  }
  else {
    syslog(LOG_ERR, "ERROR: COULD NOT FIND SYSTEM USER '%s'", username);
    return -1;
  }
}

/**
 * @brief Return the size of @param file_name as stored on the file system. An indirect way to determine if a file exists.
 * @param file_name Full path and file name
 * @return -1 on failure
 */
ssize_t
GetFileSize(const char *file_name)
{
  struct stat st = {0};
  if ((access(file_name, F_OK|R_OK)) < 0)  return -1;
  if ((stat(file_name, &st)) < 0)  return -1;

  return st.st_size;
}

/**
 * @brief Determine if file exists on filesystem. This call will succeed even if file size is '0'.
 * @param file_name Full path and file name
 * @return true if file exists
 */
bool
IsFileExists(const char *file_name)
{
  return GetFileSize(file_name) >= 0;
}

bool
IsFileWithContentPossibly(const char *file_name)
{
  return GetFileSize(file_name) > 0;
}

bool
IsRunningAsRoot()
{
  return geteuid() == 0;
}