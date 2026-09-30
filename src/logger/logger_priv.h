/**
 * @file logger_priv.h
 * @brief Internal entry points of the logger module.
 *
 * Not installed, and not reachable from a consumer — @c src/ is on the
 * library's PRIVATE include path only, and the install rule copies @c include/
 * and nothing else.
 *
 * This header exists to expose two things the public interface deliberately
 * withholds: the configuration compiler, so its output can be inspected; and a
 * create entry point that takes a driver table directly, so the core can be
 * exercised against a test double.
 *
 * The second is the injection hook the library's private-header convention
 * provides for.  The public interface has no way to name or supply a driver —
 * that is the point of it — so a test that needs to observe what the core does
 * with a record reaches it through @ref LoggerCreateWithDriver instead.  The
 * hook is not a back door into the public contract: it is declared here, it is
 * never installed, and nothing outside this module and its tests can see it.
 *
 * Functions here carry no @c Uf prefix.  That prefix marks the public
 * contract, and none of this is part of it.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_LOGGER_LOGGER_PRIV_H
#define UFLIB_LOGGER_LOGGER_PRIV_H

#include <uflib/logger/logger.h>

#include "logger_type_priv.h"

/* ── Configuration compiler ─────────────────────────────────────────────── */

/*!
 * Compile a caller's configuration into the routing model a driver consumes.
 *
 * Resolves every unset field to its default, then expresses the result as
 * rules.  A caller states one destination, so this produces one rule today; the
 * model holds an array because the configuration-file front-end will be able to
 * state several, and the driver should not have to change when it does.
 *
 * @p out_config_ptr borrows from @p config_ptr and from static literals — it
 * owns nothing.  Its @c rules is the exception: that array is allocated, and
 * the caller must free it.  The level map is a module-level static and must not
 * be freed.
 *
 * @param config_ptr      Caller configuration; must not be NULL.
 * @param out_config_ptr  Receives the compiled model.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
UfLoggerStatus
LoggerCompileConfig(const UfLoggerConfig *config_ptr, LoggerCompiledConfig *out_config_ptr);

/*!
 * Release the allocations made by @ref LoggerCompileConfig.
 *
 * Frees the rule array and nothing else — every other pointer in the compiled
 * model is borrowed or static, and freeing one would be freeing the caller's
 * memory or a string literal.
 *
 * @param config_ptr  A compiled model; NULL is a no-op.
 */
void
LoggerCompiledConfigRelease(LoggerCompiledConfig *config_ptr);

/* ── Injection hook ─────────────────────────────────────────────────────── */

/*!
 * Create a logger bound to a caller-supplied driver.
 *
 * The production create path binds the module's own driver and is the only one
 * a consumer can reach.  This entry point takes the table explicitly so a test
 * can bind a recording double and assert on what the core did — which is the
 * only way to test routing, level gating and format resolution without a real
 * backend in the picture.
 *
 * Ownership is the same as @ref UfLoggerCreate: the logger owns the driver
 * state it opens, and destroying the logger closes it.
 *
 * @param config_ptr   Configuration to apply.
 * @param table_ptr    Driver to bind; must not be NULL.
 * @param out_logger_ptr  Receives the new logger on success, NULL on failure.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
UfLoggerStatus
LoggerCreateWithDriver(const UfLoggerConfig *config_ptr, const LoggerDriverTranslationTable *table_ptr,
                       UfLogger **out_logger_ptr);

/* ── Production binding ─────────────────────────────────────────────────── */

/*!
 * The driver table the module binds when a consumer creates a logger.
 *
 * Returns NULL when this build has no driver compiled in, which the create path
 * reports as @ref UF_LOGGER_STATUS_ERR_UNSUPPORTED.  The seam is a function
 * rather than a symbol reference so that the binding can move — to a different
 * backend, or behind a compile-time choice — without the core changing.
 *
 * @return The table, or NULL when no driver is available.
 */
const LoggerDriverTranslationTable *
LoggerBuiltinDriverTable(void);

#endif /* UFLIB_LOGGER_LOGGER_PRIV_H */
