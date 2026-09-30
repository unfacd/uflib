#ifndef UFCOMMAND_PRIVATE_H
/*
 * THIS HEADER IS NOT INSTALLED.  It is private to the library implementation and
 * is colocated with its sources; consumers must never include it.
 */
#define UFCOMMAND_PRIVATE_H

#include "ufcommand_type_priv.h"

// A token body up to this length is accumulated on the stack instead of being
// heap-allocated at max_token_length(); longer tokens spill to the heap.
#define UFCOMMAND_PRIV_TOKEN_INLINE 256
#include <stddef.h>
#include <stdbool.h>
#include <uflib/logger/logger.h>

/* Logging, mirroring ufconfig's approach: the logger's own macros do not test for
 * NULL, and these have to be macros so that __FILE__ and __LINE__ resolve at the
 * call site rather than inside a wrapper.  A registry with no logger takes the
 * branch and does nothing. */
#define UFCOMMAND_LOG_DEBUG(r, ...) do { if ((r)->uf_logger) UF_LOGGER_DEBUG((r)->uf_logger, __VA_ARGS__); } while (0)
#define UFCOMMAND_LOG_INFO(r, ...)  do { if ((r)->uf_logger) UF_LOGGER_INFO((r)->uf_logger, __VA_ARGS__); } while (0)
#define UFCOMMAND_LOG_WARN(r, ...)  do { if ((r)->uf_logger) UF_LOGGER_WARN((r)->uf_logger, __VA_ARGS__); } while (0)
#define UFCOMMAND_LOG_ERROR(r, ...) do { if ((r)->uf_logger) UF_LOGGER_ERROR((r)->uf_logger, __VA_ARGS__); } while (0)

size_t          UfCommandHashParts(char *const *tokens, size_t count);
char *          UfCommandStrDup(const char *s);
char *          UfCommandStrNDup(const char *s, size_t n);
UfCommandStatus UfCommandResolveAlias(UfCommandRegistry *          registry, char ***tokens_io, size_t *count_io,
                                      const UfCommandParserConfig *parser_config);
UfCommandStatus UfCommandExecuteTokens(UfCommandRegistry *registry, UfCommandContext *context, char **tokens,
                                       size_t             count, UfCommandResult *    result);

#endif
