/**
 * @file lockless_treiber_stack_priv.h
 * @brief Private definition of the opaque LocklessTreiberStack handle.
 *
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

#ifndef LOCKLESS_TREIBER_STACK_PRIV_H
#define LOCKLESS_TREIBER_STACK_PRIV_H

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack_type.h>
#include <uflib/logger/logger_type.h>

/*!
 * A lock-free Treiber stack: a single atomically-accessed head.
 *
 * Multi-producer push; the single consumer "steals all" by atomically exchanging
 * the head to NULL. The stack owns no memory — nodes are caller-allocated.
 *
 * This struct is private: the public header advertises only the opaque
 * LocklessTreiberStack handle.
 *
 * @p uf_logger is borrowed, never owned, and is never touched by push/steal_all:
 * it is read only when the handle is created or released, so it stays off the
 * hot path and out of any atomic protocol.  NULL means the stack reports
 * nothing, which is the behaviour every caller had before the field existed.
 */
struct LocklessTreiberStack {
    _Atomic(struct LocklessTreiberStackNode *) head;      /*!< Top of the stack. */
    UfLogger                                  *uf_logger; /*!< Borrowed diagnostic sink; NULL = silent. */
};

#endif /* LOCKLESS_TREIBER_STACK_PRIV_H */
