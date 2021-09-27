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

#ifndef UFSRV_OPEN_FILE_TYPE_H
#define UFSRV_OPEN_FILE_TYPE_H

#include <stdlib.h>
#include <stdio.h>

struct OpenFile {
  const char *filename;
  char *conts; /* contents of the file */
  FILE *fp;
  size_t size;
  unsigned stat; /* errno information */
};
typedef struct OpenFile OpenFile;

#endif //UFSRV_OPEN_FILE_TYPE_H
