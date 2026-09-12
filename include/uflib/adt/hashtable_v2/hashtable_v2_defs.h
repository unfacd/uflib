/*
 Copyright (c) 2015-2025 unfacd works

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

/**
 * @file hashtable_v2_defs.h
 * @brief Compile-time configuration constants for HashTableV2.
 *
 * Follows the UFSRV_CODING_CONVENTIONS.md config_defs.h conventions:
 *  - CONFIG_DEFAULT_*  — may be overridable at configure time
 *  - PRIV_CONFIG_DEFAULT_* — never overridable; private implementation limits
 */

#ifndef UFLIB_ADT_HASHTABLE_V2_DEFS_H
#define UFLIB_ADT_HASHTABLE_V2_DEFS_H

/** Default initial slot count (a prime near 2^16, matches V1 bootstrap). */
#define CONFIG_DEFAULT_HASHTABLE_V2_INITIAL_SIZE  65521UL

/** Load-factor numerator — expand when entries ≥ table_size × NUM / DEN. */
#define PRIV_CONFIG_DEFAULT_HASHTABLE_V2_LOAD_FACTOR_NUM  2
#define PRIV_CONFIG_DEFAULT_HASHTABLE_V2_LOAD_FACTOR_DEN  3

/** Sentinel slot states for open-addressing tombstone discipline. */
#define PRIV_HASHTABLE_V2_SLOT_EMPTY      ((void *)0)
#define PRIV_HASHTABLE_V2_SLOT_TOMBSTONE  ((void *)1)

#endif /* UFLIB_ADT_HASHTABLE_V2_DEFS_H */
