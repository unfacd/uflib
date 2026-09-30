/**
 * @file ufconfig_bench_stubs.c
 * @brief Logger entry points the config module calls, stubbed for the benchmark.
 *
 * The benchmark compiles the module's own sources rather than linking the
 * library, so that it can measure them at -O2 with sanitizers off — a
 * benchmark linked against a sanitizer-instrumented library measures the
 * sanitizer, and ASan moves these timings by a large factor.
 *
 * The module's only external dependency is the logger, reached through the
 * UFCONFIG_LOG_* macros in ufconfig_priv.h.  These are those two entry points
 * and nothing else; they do no work, and none of the module's cost is in them.
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

#include <uflib/logger/logger.h>

#include <stdarg.h>

static UfLoggerStatus sNoop(void)
{
  return UF_LOGGER_STATUS_OK;
}

UfLoggerStatus UfLoggerInfo(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
                            const char *format_ptr, ...)
{
  (void)logger_ptr;
  (void)file_ptr;
  (void)line;
  (void)function_ptr;
  (void)format_ptr;
  return sNoop();
}

UfLoggerStatus UfLoggerError(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
                             const char *format_ptr, ...)
{
  (void)logger_ptr;
  (void)file_ptr;
  (void)line;
  (void)function_ptr;
  (void)format_ptr;
  return sNoop();
}
