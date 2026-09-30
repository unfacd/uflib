/**
 * @file ufconfig_defs.h
 * @brief The ufconfig module's public compile-time constants.
 *
 * Only constants a caller could reasonably want to know live here.  The
 * module's own tuning — parse depth, file ceilings, alias recursion — is
 * private and lives in @c src/ufconfig/ufconfig_defs_priv.h, because a caller
 * has no decision to make about it.
 *
 * The split is not cosmetic.  A caller writing a configuration document has to
 * know how long a path may be, because that bound is part of the format it is
 * writing against.  It does not have to know how deep the parser will recurse,
 * because that is a property of this implementation rather than of the format.
 * Exposing the second would make it a compatibility surface the module could
 * never change.
 *
 * These are @c CONFIG_DEFAULT_* values and may be overridden at CMake configure
 * time through the generated @c config_uflib.h.  Each is re-declared under an
 * @c #ifndef guard so this header works whether or not that file is in scope;
 * the generated file remains the single override point.
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

#ifndef UFLIB_UFCONFIG_UFCONFIG_DEFS_H
#define UFLIB_UFCONFIG_UFCONFIG_DEFS_H

#include <uflib/uflib_defs.h>

/*!
 * Version of the driver interface this build implements.
 *
 * A driver declares the version it was written against in its
 * @ref UfConfigDriverCaps, and registration refuses one that does not match.
 * The check exists so that a driver built against an older interface fails at
 * registration with a diagnostic, rather than at the first call with a
 * misread struct.
 */
#define UF_CONFIG_DRIVER_API_VERSION  1u

/*!
 * Longest field path the module accepts, including its terminator.
 *
 * A path longer than this is refused with @c UF_CONFIG_ERR_LIMIT_EXCEEDED
 * rather than truncated.  Truncation would be worse than refusal here: the
 * truncated path might name a different field that also exists, so a caller
 * would silently read or write the wrong one.
 */
#ifndef CONFIG_DEFAULT_UFCONFIG_PATH_MAX
  #define CONFIG_DEFAULT_UFCONFIG_PATH_MAX  512
#endif

#endif /* UFLIB_UFCONFIG_UFCONFIG_DEFS_H */
