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

#ifndef UFLIB_UTILS_SYS_H
#define UFLIB_UTILS_SYS_H

#include <stdbool.h>
#include <uflib/uflib_defs.h>

PUBLIC_API int DropRootPrivileges(const char *username, const char *chroot_dir, void (*on_success)(void));
PUBLIC_API ssize_t GetFileSize(const char *file_name);
PUBLIC_API bool IsFileExists(const char *file_name);
PUBLIC_API bool IsFileWithContentPossibly(const char *file_name);
PUBLIC_API bool IsRunningAsRoot();


#endif //UFSRV_UTILS_SYS_H
