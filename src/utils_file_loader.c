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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/utils_file_loader.h>
#include <uflib/standard_defs.h>
#include <uflib/utils_str.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>



static void _HashToHex(const unsigned char *hash, char *hex);
static void _ComputeContentHash(FileLoader * _Nonnull file_loader_ptr);
static bool _IsHashEqual(const unsigned char *hash1, const unsigned char *hash2, size_t digest_length);

/**
 * @brief Memory-map file content and reload if modified since last mapping.
 * @note User must prefill filename.
 * @param file_loader_ptr user allocated
 * @param is_hashed indicates if file content is to be hashed
 * @return as per enum FileLoaderResult
 */
enum FileLoaderResult
FileLoaderLoadIfModified(FileLoader *file_loader_ptr, bool is_hashed) {
  size_t current_file_sz = file_loader_ptr->file_info.size;

  bool is_valid = false;
#if UFLIB_CAPABILITY_UTF8PROC
  size_t file_length_sz = DefensiveStrlenUtf8(file_loader_ptr->filename, FILE_LOADER_MAX_FILENAME, IGNORE_BYTES_READ_PARAM, &is_valid);
#else
  /* No codepoint count without utf8proc, and none is needed: this call only
     establishes that the name is non-empty and terminates in range. */
  size_t file_length_sz = 0;
  is_valid = DefensiveStrlen(file_loader_ptr->filename, FILE_LOADER_MAX_FILENAME, &file_length_sz);
#endif
  if (!is_valid || file_length_sz == 0) {
    return FL_FILENAME_ERROR;
  }

  struct stat st = {0};
  if (stat(file_loader_ptr->filename, &st) != 0) {
    return FL_NON_EXIST;
  }

  //if file_loader_ptr->last_mtime is set means file was previously mapped
  if (file_loader_ptr->last_mtime != 0 && st.st_mtime == file_loader_ptr->last_mtime) {
    return FL_NOT_CHANGED;
  }

  if (file_loader_ptr->is_mapped && IS_CONTENT_LOADED(file_loader_ptr->content)) {
    munmap(file_loader_ptr->content, file_loader_ptr->file_info.size);
    file_loader_ptr->is_mapped = false;
  }

  if (file_loader_ptr->fd != -1) {
    close(file_loader_ptr->fd);
    file_loader_ptr->fd = -1;
  }

  file_loader_ptr->fd = open(file_loader_ptr->filename, O_RDONLY);
  if (file_loader_ptr->fd == -1) {
    return FL_READ_ERROR;
  }

  // Handle empty files specially
  if (st.st_size == 0) {
    if (current_file_sz > 0) {
      return FL_CORRUPTION_ERROR; //file was previously loaded, but now is empty
    }

    file_loader_ptr->content = NULL;
    file_loader_ptr->file_info.size = 0;
    file_loader_ptr->last_mtime = st.st_mtime;
    file_loader_ptr->is_mapped = false;
    if (is_hashed) {
      _ComputeContentHash(file_loader_ptr);
    }

    return FL_SUCCESS_EMPTY_FILE;
  }

  file_loader_ptr->content = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, file_loader_ptr->fd, 0);
  if (file_loader_ptr->content == MAP_FAILED) {
    close(file_loader_ptr->fd);
    file_loader_ptr->fd = -1;
    return FL_MAPPING_ERROR;
  }

  // Advise kernel about access pattern (Linux-specific optimization)
  madvise(file_loader_ptr->content, st.st_size, MADV_SEQUENTIAL);

  file_loader_ptr->file_info.size = st.st_size;
  file_loader_ptr->last_mtime = st.st_mtime;
  file_loader_ptr->is_mapped = true;
  if (is_hashed) {
    _ComputeContentHash(file_loader_ptr);
  }

  return FL_SUCCESS;
}

void
FileLoaderReset(FileLoader * _Nonnull file_loader_ptr)
{
  if (file_loader_ptr->is_mapped && file_loader_ptr->content && file_loader_ptr->file_info.size > 0) {
    munmap(file_loader_ptr->content, file_loader_ptr->file_info.size);
    if (file_loader_ptr->fd != -1) {
      close(file_loader_ptr->fd);
    }
  }

  memset(file_loader_ptr, '\0', sizeof(FileLoader));
}

const unsigned char *
FileLoaderGetContent(FileLoader * _Nonnull file_loader_ptr, size_t *size) {
  if (size) *size = file_loader_ptr->file_info.size;
  return file_loader_ptr->content;
}

const char *
FileLoaderGetHash(FileLoader *file_loader_ptr) {
  return file_loader_ptr->hashed.hash_hex;
}

const unsigned char *
FileLoaderGetHashRaw(FileLoader *file_loader_ptr) {
  return file_loader_ptr->hashed.hash;
}

bool
FileLoaderCompareHashes(FileLoader *fl, const unsigned char *expected_hash) {
  return _IsHashEqual(fl->hashed.hash, expected_hash, SHA256_DIGEST_LENGTH);
}

#include <openssl/sha.h>

static void
_ComputeContentHash(FileLoader * _Nonnull file_loader_ptr) {
  if (!file_loader_ptr || !file_loader_ptr->content || file_loader_ptr->file_info.size == 0) {
    memset(file_loader_ptr->hashed.hash, 0, SHA256_DIGEST_LENGTH);
    _HashToHex(file_loader_ptr->hashed.hash, file_loader_ptr->hashed.hash_hex);

    file_loader_ptr->hashed.is_hashed = true;

    return;
  }

  SHA256((unsigned char *)file_loader_ptr->content, file_loader_ptr->file_info.size, file_loader_ptr->hashed.hash);
  _HashToHex(file_loader_ptr->hashed.hash, file_loader_ptr->hashed.hash_hex);
}

static void _HashToHex(const unsigned char *hash, char *hex) {
  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    sprintf(hex + (i * 2), "%02x", hash[i]);
  }
  hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static bool
_IsHashEqual(const unsigned char *hash1, const unsigned char *hash2, size_t digest_length) {
  return memcmp(hash1, hash2, digest_length) == 0;
}
