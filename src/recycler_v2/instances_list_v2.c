/**
 * Copyright (C) 2015-2026 unfacd works
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

/**
 * @file instances_list_v2.c
 * @brief RecyclerV2 fallback multi-holder list — spinlock-protected linked list
 *
 * This file is only compiled for the multi-InstanceHolderV2 fallback path.
 * In the common case (single InstanceHolderV2 per object), the atomic
 * holder_word on RecyclerV2PoolTypeEnvelop is used via CAS — this code
 * is never exercised.
 *
 * The InstancesListV2 operations are in recycler_v2_priv.h (inline for the
 * hot single-holder path) and in recycler_v2.c (holder add/remove).
 * This file exists as a build target placeholder for the CMakeLists.txt
 * target_sources() entry.  The actual list operations are implemented
 * inline in recycler_v2.c's sHolderAddV2/sHolderRemoveV2 functions.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "recycler_v2_priv.h"

/* This file intentionally contains no additional code.
 * InstancesListV2 operations (sHolderAddV2 slow path, sHolderRemoveV2 slow path)
 * are implemented in recycler_v2.c alongside the fast-path CAS holder word
 * operations to keep the hot/cold path logic in a single compilation unit. */
