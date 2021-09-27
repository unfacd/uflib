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


#ifndef UFSRV_UTILS_BITS_H
#define UFSRV_UTILS_BITS_H

#include "standard_c_includes.h"

void	set_bit (unsigned char *buffer, int position);
int	get_bit (char byte, int position);
void	set_32bit (int value, unsigned char *buffer);
int get_16bit (const unsigned char *buffer);

#define GET_N_BITS_FROM_REAR(k,n) ((k) & ((1UL<<(n))-1))
//cut out from m(inclusive)->n(exclusive) starting from LSB, starting (0, 1 ..63)
#define GET_BITS_IN_BETWEEN(k,m,n) GET_N_BITS_FROM_REAR((k)>>(m),((n)-(m)))

#endif //UFSRV_UTILS_BITS_H
