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


#ifndef UFSRV_UTILS_FILE_H
#define UFSRV_UTILS_FILE_H

#include <sys/types.h>
#include "file_info_type.h"
#include "open_file_type.h"

int MakePidFile(const char *path, pid_t serverpid);
int RemovePidFile	(const char *path);
int GetFileInfo (const char *path, FileInfo *f_info, int mode);
char *LoadFileToMemory (const char *path);
int FileUtilsRenameFile	(const char *orig, const char *dest);
int OpenThisFile (OpenFile *);

#endif //UFSRV_UTILS_FILE_H
