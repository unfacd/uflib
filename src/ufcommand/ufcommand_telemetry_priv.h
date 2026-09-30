/*
 * THIS HEADER IS NOT INSTALLED.  It is private to the library implementation and
 * is colocated with its sources; consumers must never include it.
 *
 * The telemetry internals behind the public seam in
 * <uflib/ufcommand/ufcommand_telemetry.h>.  The observer's lifecycle, the
 * attachment seam and the JSON dump are public; what lives here is the counter
 * layout, the per-counter accessors, and the hooks the dispatcher calls.
 */

#ifndef UFCOMMAND_TELEMETRY_PRIV_H
#define UFCOMMAND_TELEMETRY_PRIV_H

#include <stdint.h>

#include <uflib/ufcommand/ufcommand_telemetry.h>

/* White-box accessors, for the module's tests and nothing else.  A host reads the
 * counters as JSON; there is deliberately no accessor in the public header.
 * UFCOMMAND_NOT_FOUND means never observed and leaves the out-parameter at 0; it
 * is not an error. */
UfCommandStatus UfCommandTelemetryGetCommandCount(const UfCommandTelemetry *telemetry, const char *name,
                                                  uint64_t *                out_count);
UfCommandStatus UfCommandTelemetryGetAliasCount(const UfCommandTelemetry *telemetry, const char *name,
                                                uint64_t *                out_count);
UfCommandStatus UfCommandTelemetryGetBindingCount(const UfCommandTelemetry *telemetry, UfCommandModifier modifiers,
                                                  const char *              key, uint64_t *              out_count);

/* Serialise to a heap string the caller frees with free().  The public dump is
 * built on this. */
UfCommandStatus UfCommandTelemetryGetJson(const UfCommandTelemetry *telemetry, char **out_json);

/* Hooks called by the dispatcher.  Advisory: the dispatcher discards the status,
 * so a failure here can never change what a command does. */
UfCommandStatus UfCommandTelemetryCommand(UfCommandTelemetry *telemetry, const char *name);
UfCommandStatus UfCommandTelemetryAlias(UfCommandTelemetry *telemetry, const char *name);
UfCommandStatus UfCommandTelemetryBinding(UfCommandTelemetry *telemetry, UfCommandModifier modifiers, const char *key);

#endif
