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
 * @file recycler_v2_log_strings.h
 * @brief Log format string literals for the RecyclerV2 module
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_LOG_STRINGS_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_LOG_STRINGS_H

/* ── Log code offsets ────────────────────────────────────────────────────── */

/** Base log code for RecyclerV2 (distinct from V1's 1200+ range). */
#define LOGCODE_RECYCLER_V2_BASE                    1400

/* ── Get/Put codes ───────────────────────────────────────────────────────── */

#define LOGCODE_RECYCLER_V2_UNDEFINED_POOLDEF      (LOGCODE_RECYCLER_V2_BASE + 1)
#define LOGCODE_RECYCLER_V2_GET_FULLY_LEASED       (LOGCODE_RECYCLER_V2_BASE + 2)
#define LOGCODE_RECYCLER_V2_ENQUE_REFCNT_SHORT     (LOGCODE_RECYCLER_V2_BASE + 3)
#define LOGCODE_RECYCLER_V2_PUT_ON_FULL            (LOGCODE_RECYCLER_V2_BASE + 4)
#define LOGCODE_RECYCLER_V2_MARSHALLER_INSTANCE     (LOGCODE_RECYCLER_V2_BASE + 5)
#define LOGCODE_RECYCLER_V2_UNCOLLECTED_INSTANCES  (LOGCODE_RECYCLER_V2_BASE + 6)
#define LOGCODE_RECYCLER_V2_INSTANCE_NOT_ON_LIST   (LOGCODE_RECYCLER_V2_BASE + 7)
#define LOGCODE_RECYCLER_V2_NEW_INSTANCE_ERROR     (LOGCODE_RECYCLER_V2_BASE + 8)

/* ── Expansion codes ─────────────────────────────────────────────────────── */

#define LOGCODE_RECYCLER_V2_EXPANSION              (LOGCODE_RECYCLER_V2_BASE + 10)
#define LOGCODE_RECYCLER_V2_EXPANSION_FAILED       (LOGCODE_RECYCLER_V2_BASE + 11)

/* ── Marshaller codes ────────────────────────────────────────────────────── */

#define LOGCODE_RECYCLER_V2_MARSHAL_TRIGGERED       (LOGCODE_RECYCLER_V2_BASE + 20)
#define LOGCODE_RECYCLER_V2_MARSHAL_SUCCESS        (LOGCODE_RECYCLER_V2_BASE + 21)
#define LOGCODE_RECYCLER_V2_MARSHAL_FAILED         (LOGCODE_RECYCLER_V2_BASE + 22)
#define LOGCODE_RECYCLER_V2_MARSHAL_CAS_FAILED     (LOGCODE_RECYCLER_V2_BASE + 23)
#define LOGCODE_RECYCLER_V2_UNMARSHAL_TRIGGERED     (LOGCODE_RECYCLER_V2_BASE + 24)
#define LOGCODE_RECYCLER_V2_UNMARSHAL_SUCCESS      (LOGCODE_RECYCLER_V2_BASE + 25)
#define LOGCODE_RECYCLER_V2_UNMARSHAL_FAILED       (LOGCODE_RECYCLER_V2_BASE + 26)
#define LOGCODE_RECYCLER_V2_UNMARSHAL_CAS_FAILED   (LOGCODE_RECYCLER_V2_BASE + 27)

/* ── Storage codes ───────────────────────────────────────────────────────── */

#define LOGCODE_RECYCLER_V2_STORAGE_WRITE_FAILED   (LOGCODE_RECYCLER_V2_BASE + 30)
#define LOGCODE_RECYCLER_V2_STORAGE_READ_FAILED    (LOGCODE_RECYCLER_V2_BASE + 31)
#define LOGCODE_RECYCLER_V2_STORAGE_DELETE_FAILED  (LOGCODE_RECYCLER_V2_BASE + 32)
#define LOGCODE_RECYCLER_V2_STORAGE_INIT_FAILED    (LOGCODE_RECYCLER_V2_BASE + 33)

/* ── Log format strings ──────────────────────────────────────────────────── */

#define LOGSTR_RECYCLER_V2_GET_FULLY_LEASED \
    "%s {pid:'%lu', e:'%d'}: Fully leased '%s' (groups: %d)"

#define LOGSTR_RECYCLER_V2_ENQUE_REFCNT_SHORT \
    "%s {pid:'%lu', e:'%d'}: Refcount short tail:'%lu' refcount_read:'%lu' groupid:'%d' instance:'%p' oid:'%lu' type:'%s' leased:'%lu'"

#define LOGSTR_RECYCLER_V2_PUT_ON_FULL \
    "%s {pid:'%lu', e:'%d'}: Put on full queue head:'%lu' tail:'%lu' type:'%s' leased:'%lu'"

#define LOGSTR_RECYCLER_V2_MARSHALLER_INSTANCE \
    "%s {pid:'%lu', e:'%d'}: Marshaller instance attempted put holder:'%p' id:'%lu'"

#define LOGSTR_RECYCLER_V2_UNCOLLECTED_INSTANCE \
    "%s {pid:'%lu', e:'%d'}: Uncollected instances groupid:'%d' tail:'%lu' head:'%lu' env:'%p' oid:'%lu' type:'%s' leased:'%lu' list_sz:'%lu'"

#define LOGSTR_RECYCLER_V2_INSTANCE_NOT_ON_LIST \
    "%s {pid:'%lu', e:'%d'}: Instance not on list tail:'%lu' head:'%lu' env:'%p' oid:'%lu' type:'%s' leased:'%lu' list_sz:'%lu'"

#define LOGSTR_RECYCLER_V2_NEW_INSTANCE_ERROR \
    "%s {pid:'%lu', e:'%d'}: New instance error ih:'%p' type:'%s'"

#define LOGSTR_RECYCLER_V2_EXPANSION \
    "%s {pid:'%lu', e:'%d'}: Expansion groupid:'%d' type:'%s' new_capacity:'%lu'"

#define LOGSTR_RECYCLER_V2_EXPANSION_FAILED \
    "%s {pid:'%lu', e:'%d'}: Expansion failed type:'%s' allocated_groups_sz:'%d' group_allocation_sz:'%lu'"

#define LOGSTR_RECYCLER_V2_MARSHAL_TRIGGERED \
    "%s {pid:'%lu', e:'%d'}: Marshal triggered type:'%s' usage:'%f' watermark:'%f'"

#define LOGSTR_RECYCLER_V2_MARSHAL_SUCCESS \
    "%s {pid:'%lu', e:'%d'}: Marshal success type:'%s' oid:'%lu' marshaller_id:'%lu'"

#define LOGSTR_RECYCLER_V2_MARSHAL_FAILED \
    "%s {pid:'%lu', e:'%d'}: Marshal failed type:'%s' oid:'%lu'"

#define LOGSTR_RECYCLER_V2_UNMARSHAL_TRIGGERED \
    "%s {pid:'%lu', e:'%d'}: Unmarshal triggered type_idx:'%u' marshaller_id:'%lu'"

#define LOGSTR_RECYCLER_V2_UNMARSHAL_SUCCESS \
    "%s {pid:'%lu', e:'%d'}: Unmarshal success type_idx:'%u' marshaller_id:'%lu'"

#define LOGSTR_RECYCLER_V2_UNMARSHAL_FAILED \
    "%s {pid:'%lu', e:'%d'}: Unmarshal failed type_idx:'%u' marshaller_id:'%lu'"

#define LOGSTR_RECYCLER_V2_MARSHAL_CAS_FAILED \
    "%s {pid:'%lu', e:'%d'}: Marshal CAS failed type:'%s'"

#define LOGSTR_RECYCLER_V2_STORAGE_WRITE_FAILED \
    "%s: write failed id:'%lu' error:'%s'"

#define LOGSTR_RECYCLER_V2_STORAGE_READ_FAILED \
    "%s: read failed id:'%lu' error:'%s'"

#define LOGSTR_RECYCLER_V2_STORAGE_DELETE_FAILED \
    "%s: delete failed id:'%lu' error:'%s'"

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_LOG_STRINGS_H */
