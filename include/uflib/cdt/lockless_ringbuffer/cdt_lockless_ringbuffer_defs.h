/**
 * @file cdt_lockless_ringbuffer_defs.h
 * @brief Module-specific compile-time defaults for the lock-free bounded ring
 *        buffer.
 *
 * These constants are consumed by cdt_lockless_ringbuffer_priv.h and
 * cdt_lockless_ringbuffer.c.  They are implementation limits, not
 * user-facing knobs — override via a generated config_uflib.h only when the
 * build genuinely needs a different cache geometry.
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

#ifndef UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_DEFS_H
#define UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_DEFS_H

/*!
 * Cache-line size used to isolate the producer cursor (`tail`) and consumer
 * cursor (`head`) onto separate cache lines, eliminating false sharing
 * between the two hot atomic counters.
 */
#ifndef PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE
  #define PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE  64
#endif

#endif /* UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_DEFS_H */
