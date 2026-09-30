#ifndef UFCOMMAND_TYPE_PRIVATE_H
/*
 * THIS HEADER IS NOT INSTALLED.  It is private to the library implementation and
 * is colocated with its sources; consumers must never include it.
 */
#define UFCOMMAND_TYPE_PRIVATE_H

#include <uflib/ufcommand/ufcommand_type.h>
#include <uflib/ufcommand/ufcommand_completion.h>
#include "ufcommand_telemetry_priv.h"
#include <stdbool.h>

typedef struct UfCommandEntryPrivate
{
  char *                   name;
  char *                   description;
  UfCommandHandlerCallback handler;
  void *                   user_data;
  size_t                   min_args;
  size_t                   max_args;
  size_t                   hash; /* UfCommandHash(name), so a probe can reject without strcmp */
  bool                     occupied;
  bool                     tombstone;
} UfCommandEntryPrivate;

typedef struct UfCommandAliasPrivate
{
  char * name;
  char * expansion;
  size_t hash; /* UfCommandHash(name) */
  bool   occupied;
  bool   tombstone;
} UfCommandAliasPrivate;

typedef struct UfCommandBindingPrivate
{
  char *            key;
  char *            command;
  size_t            hash; /* sHashBinding(modifiers, key) */
  // The 4-byte modifier mask is declared LAST on purpose.  Placed first it
  // pushed the pointers to offset 8 and padded the slot to 40 bytes -- a stride
  // that does not divide the 64-byte cache line, so consecutive slots straddled
  // lines.  Declared here the slot is 32 bytes and two share a line.
  UfCommandModifier modifiers;
  bool              occupied;
  bool              tombstone;
} UfCommandBindingPrivate;

struct UfCommandRegistry
{
  UfCommandEntryPrivate *  commands;
  size_t                   command_capacity, command_count, command_tombstones;
  UfCommandAliasPrivate *  aliases;
  size_t                   alias_capacity, alias_count, alias_tombstones;
  UfCommandBindingPrivate *bindings;
  size_t                   binding_capacity, binding_count, binding_tombstones;
  UfCommandKeyConfig       key_config;
  size_t                   max_command_components;
  size_t                   dispatch_depth;
  // NULL when not observing.  `telemetry_owned` distinguishes the observer the
  // config asked the registry to create (which the registry destroys) from one
  // a host attached (which it must not -- the host owns it).
  UfCommandTelemetry *     telemetry;
  bool                     telemetry_owned;
  // Borrowed; NULL when the host did not supply one.  Never destroyed here.
  UfLogger *               uf_logger;
  // NULL when not opted in, and owed by no flag: there is no way to attach one.
  // Declared last so every offset above is unchanged.
  UfCommandCompletion *    completion;
};

struct UfCommandParser
{
  UfCommandParserConfig config;
};

struct UfCommandContext
{
  void *user_data;
};

struct UfCommandResult
{
  UfCommandStatus dispatch_status;
  UfCommandStatus handler_status;
  char *          resolved_command;
  UfCommandArg *  args;
  char **         owned_args;
  size_t          arg_count, arg_capacity;
};

#endif
