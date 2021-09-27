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

//
// Created by devops on 10/18/20.
//

#ifndef UFSRV_FILE_INFO_TYPE_H
#define UFSRV_FILE_INFO_TYPE_H

#include <stdlib.h>
#include <stdbool.h>

#define FILE_EXISTS 1
#define FILE_READ   2
#define FILE_EXEC   4

typedef struct FileInfo {
  size_t size;
  time_t last_modification;

  /* Suggest flags to open this file */
  int flags_read_only;

  bool exists;
  bool is_file;
  bool is_link;
  bool is_directory;
  bool exec_access;
  bool read_access;
} FileInfo;


#endif //UFSRV_FILE_INFO_TYPE_H
