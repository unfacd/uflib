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

#ifndef UFSRV_STANDARD_VALGRIND_INCLUDES_H
#define UFSRV_STANDARD_VALGRIND_INCLUDES_H

//http://valgrind.org/docs/manual/mc-manual.html#mc-manual.mempools
#define __VALGRIND_DRD 1

#if __VALGRIND_DRD
# include <valgrind/valgrind.h>
# include <valgrind/drd.h>
# include <valgrind_drd_inlines.h>
#	include <valgrind/memcheck.h>
#endif

#endif //UFSRV_STANDARD_VALGRIND_INCLUDES_H
