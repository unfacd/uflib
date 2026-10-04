/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

//
// Created by devops on 6/22/26.
//

#ifndef UFLIB_UTILS_FILE_LOADER_H
#define UFLIB_UTILS_FILE_LOADER_H

#include <uflib/uflib_defs.h>

#include "file_loader_type.h"

enum FileLoaderResult FileLoaderLoadIfModified(FileLoader *file_loader_ptr, bool is_hashed);
PUBLIC_API bool FileLoaderCompareHashes(FileLoader *fl, const unsigned char *expected_hash);
PUBLIC_API void FileLoaderReset(FileLoader *file_loader_ptr) __attribute__((nonnull(1)));

#endif //UFSRV_UTILS_FILE_LOADER_H
