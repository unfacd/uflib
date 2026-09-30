/**
 * @file ufcommand_telemetry.h
 * @brief UfCommand telemetry — observing how a registry is actually used.
 *
 * A registry can record how its commands, aliases and bindings are used, and
 * write that record out as JSON.  The observer is an opaque handle: the counters,
 * their layout and the serialiser are private, and there is deliberately no
 * accessor that exposes them.  A host decides *whether* to observe, and reads
 * the result as JSON.
 *
 * ## Two ways to start observing
 *
 *   - declaratively, by setting @c telemetry_enabled in
 *     @c UfCommandRegistryConfig — the registry then owns an observer for its
 *     whole life;
 *   - explicitly, by creating an observer and attaching it with
 *     @c UfCommandRegistrySetTelemetry().  The registry borrows an attached
 *     observer and never outlives it, so the host must destroy it after the
 *     registry, or detach it first.
 *
 * Attaching replaces whatever is attached, so @c SetTelemetry(registry, NULL)
 * **detaches** — which is how a host stops counting, and stops paying for it,
 * without destroying its observer.  Re-attaching later resumes into the same
 * counters.
 *
 * ## Cost
 *
 * Counting is not free — it adds roughly a quarter to a dispatch that resolves
 * through more than one prefix — which is why it is opt-in and why detaching
 * exists.
 *
 * ## What the counts mean
 *
 * A name is counted once per dispatch that resolves through it.  A command
 * invocation counts **every registered prefix** of the matched name, so
 * @c "remote add origin url" increments both @c "remote" and @c "remote add" if
 * both are registered, and the counts therefore do not sum to the number of
 * dispatches.  An alias is counted once per expansion, so each link of a chain
 * counts independently.  A chord with no binding is not counted at all.
 *
 * Instrumentation never participates in resolution: every counter is advisory
 * and the dispatcher discards its status, so a failure while counting cannot
 * change what a command does.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UFCOMMAND_UFCOMMAND_TELEMETRY_H
#define UFLIB_UFCOMMAND_UFCOMMAND_TELEMETRY_H

#include <uflib/uflib_defs.h>
#include <uflib/ufcommand/ufcommand_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief An observer.  Opaque: its counters and layout are implementation detail.
 */
typedef struct UfCommandTelemetry UfCommandTelemetry;

/**
 * @brief Create an observer, independent of any registry.
 *
 * @param out_telemetry Receives the observer; set to NULL on failure.
 * @code{.c}
 * UfCommandTelemetry *telemetry = NULL;
 * UfCommandTelemetryCreate(&telemetry);
 * UfCommandRegistrySetTelemetry(registry, telemetry);
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandTelemetryCreate(UfCommandTelemetry **out_telemetry);

/**
 * @brief Destroy an observer.  @p telemetry may be NULL.
 *
 * Detach it first if a registry still references it: a registry never outlives an
 * attached observer, and nothing here can enforce that.
 */
PUBLIC_API void UfCommandTelemetryDestroy(UfCommandTelemetry *telemetry);

/**
 * @brief Zero every counter, keeping the observer attached.
 *
 * The known names are retained, so a query afterwards succeeds with a count of
 * zero and the dumped JSON still lists each name.  This clears the observations,
 * not the dictionary of what has been observed.
 */
PUBLIC_API void UfCommandTelemetryReset(UfCommandTelemetry *telemetry);

/**
 * @brief Attach an observer to a registry, replacing whatever is attached.
 *
 * Passing NULL **detaches**, which stops the counting cost without discarding the
 * counters; re-attach the same observer to resume accumulating into them.  An
 * attached observer is borrowed — the registry does not destroy it.
 *
 * @return @c UFCOMMAND_OK or @c UFCOMMAND_INVALID_ARGUMENT.
 */
PUBLIC_API UfCommandStatus UfCommandRegistrySetTelemetry(UfCommandRegistry *registry, UfCommandTelemetry *telemetry);

/**
 * @brief The observer attached to @p registry, or NULL.
 *
 * This is how a host reaches an observer the registry created for itself from
 * @c telemetry_enabled.
 */
PUBLIC_API UfCommandTelemetry *UfCommandRegistryGetTelemetry(UfCommandRegistry *registry);

/**
 * @brief Write an observer's record to @p path as JSON.
 *
 * Takes the observer rather than the registry, so it serves an observer the
 * registry owns (reach it with @c UfCommandRegistryGetTelemetry()), one the host
 * owns, and one shared across registries.  The observer need not be attached, so
 * a host can serialise the counters after detaching.
 *
 * The document is deterministic: commands and aliases sorted by name, bindings
 * by (modifiers, key).  Repeated saves of an unchanged observer are
 * byte-identical and diff cleanly.
 *
 * @param telemetry The observer to serialise.
 * @param path Destination file; truncated on open.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_NO_MEMORY
 *         or @c UFCOMMAND_IO_ERROR.
 *
 * @code{.c}
 * UfCommandTelemetry *telemetry = UfCommandRegistryGetTelemetry(registry);
 * UfCommandTelemetrySaveJson(telemetry, "/var/log/myapp/commands.json");
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandTelemetrySaveJson(const UfCommandTelemetry *telemetry, const char *path);

#ifdef __cplusplus
}
#endif
#endif
