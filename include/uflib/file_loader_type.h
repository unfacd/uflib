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

#ifndef UFLIB_FILE_LOADER_TYPE_H
#define UFLIB_FILE_LOADER_TYPE_H

#include <stdbool.h>
#include <time.h>
#include <openssl/sha.h>
#include <bits/posix1_lim.h>
#include "file_info_type.h"

#define FILE_LOADER_MAX_FILENAME (PATH_MAX) //PATH_MAX is the maximum length of a filesystem path, including file name. NAME_MAX is the name component within a path sequence

typedef struct {
    FileInfo file_info;
    char *filename;
    unsigned char *content;      /// Pointer to memory-mapped content
    int fd;                      /// File descriptor
    bool is_mapped;              /// Whether content is currently memory-mapped
    time_t last_mtime;          /// Last modification time
    struct {
        bool is_hashed;
        unsigned char hash[SHA256_DIGEST_LENGTH]; // SHA256 hash of content
        char hash_hex[SHA256_DIGEST_LENGTH * 2 + 1]; // Hex string representation
    } hashed;
} FileLoader;

enum FileLoaderResult {
    FL_SUCCESS = 0,
    FL_SUCCESS_EMPTY_FILE = 0,
    FL_NON_EXIST,
    FL_READ_ERROR,
    FL_MAPPING_ERROR,
    FL_HASH_ERROR,
    FL_FILENAME_ERROR,
    FL_CORRUPTION_ERROR,
    FL_NOT_CHANGED
};

#define IS_HASHED_FALSE false
#define IS_HASHED_TRUE  true


#endif //UFSRV_FILE_LOADER_TYPE_H
