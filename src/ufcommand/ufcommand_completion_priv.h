/*
 * THIS HEADER IS NOT INSTALLED.  It is private to the library implementation and
 * is colocated with its sources; consumers must never include it.
 *
 * The registry side of the completion engine's life.  A host reaches an engine
 * through UfCommandRegistryGetCompletion(); creating and destroying one is the
 * registry's business, so neither appears in the public header.
 */

#ifndef UFCOMMAND_COMPLETION_PRIV_H
#define UFCOMMAND_COMPLETION_PRIV_H

#include <uflib/ufcommand/ufcommand_completion.h>

// Borrows the dictionary; the registry destroys the engine with itself, and
// LoadUserConfig's swap carries it across rather than rebuilding it.
UfCommandStatus UfCommandCompletionCreate(UfCommandRegistry *dictionary, UfCommandCompletion **out_completion);
void            UfCommandCompletionDestroy(UfCommandCompletion *completion);

#endif
