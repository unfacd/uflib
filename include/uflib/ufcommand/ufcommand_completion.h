/**
 * @file ufcommand_completion.h
 * @brief UfCommand predictive completion — advisory candidates for a partial line.
 *
 * Completion answers one question: given a line and a cursor, what could the
 * token under the cursor become?  It answers with a ranked list of candidates
 * and the byte range they would replace.  It never executes anything, and it
 * never creates a command: a suggestion is text until the host feeds it back
 * through @c UfCommandParserExecute(), which stays the only execution seam.
 *
 * ## Opting in
 *
 * Completion is off by default and costs nothing when off.  Set
 * @c completion_enabled in @c UfCommandRegistryConfig, and the registry creates
 * and owns an engine for its whole life:
 *
 * @code{.c}
 * UfCommandRegistryConfig config = { 0 };
 * config.completion_enabled = true;
 * UfCommandRegistry *registry = NULL;
 * UfCommandRegistryCreate(&config, &registry);
 *
 * UfCommandCompletion *completion = UfCommandRegistryGetCompletion(registry);
 * @endcode
 *
 * There is no engine to create, attach, detach or destroy — a registry that did
 * not opt in has none, and @c UfCommandRegistryGetCompletion() returns NULL.
 * The engine dies with the registry and survives a
 * @c UfCommandRegistryLoadUserConfig(), which is worth knowing because the
 * handle a host captured before loading its configuration stays the live one.
 *
 * ## Where candidates come from
 *
 * A registry-created engine already knows the registry's own dictionary: it
 * suggests registered command components and, at the first token, registered
 * aliases.  That provider is private and cannot be removed — it reads the
 * registry's tables directly, which is why the engine has to live inside the
 * module rather than outside it.
 *
 * A host extends that by adding its own providers — history, a domain
 * dictionary, usage-weighted ranking, a local model:
 *
 * @code{.c}
 * static UfCommandStatus OnHistory(void *user_data, const UfCommandCompletionQuery *query,
 *                                  UfCommandCompletionEmit emit, void *emit_context)
 * {
 *     UfCommandCompletionCandidate candidate = {
 *         .text = "checkout", .display = "checkout", .description = "recent",
 *         .kind = UFCOMMAND_COMPLETION_CUSTOM, .score = 500.0
 *     };
 *     (void)user_data; (void)query;
 *     return emit(emit_context, &candidate);
 * }
 *
 * UfCommandCompletionProviderDefinition history = { "history", OnHistory, my_state };
 * UfCommandCompletionAddProvider(UfCommandCompletionGetCompletion(registry), &history);
 * @endcode
 *
 * ## The contract a provider answers to
 *
 * A provider is called with an immutable query and an @c emit callback.  It
 * emits zero or more candidates and returns a status.  Two rules matter:
 *
 *   - **The engine owns the strings.**  A provider may point @c text,
 *     @c display and @c description at its own storage; the engine copies them.
 *     Nothing in the candidate array survives the call, so a provider must not
 *     retain the query or the candidate it passed to @c emit.
 *   - **A failure is loud.**  A provider returning anything other than
 *     @c UFCOMMAND_OK, or emitting a candidate with a NULL or empty @c text,
 *     aborts the whole call: @c UfCommandCompletionSuggest() returns that status
 *     and produces no result.  Candidates already gathered, including the
 *     dictionary's own, are discarded.  This is deliberate — a provider that is
 *     silently broken is worse than one that stops the line from completing.
 *
 * ## Ranking
 *
 * Candidates are ordered by @c score, highest first, with ties broken by
 * @c text ascending; @c display and @c kind break the remaining ties, so a
 * result is a pure function of what the providers emitted and never of the
 * order the registry happens to store its entries in.  A negative or NaN score
 * is read as @c 0.0.  Only the highest-scoring 32 candidates are kept, chosen by
 * score rather than by arrival, so a host provider can outrank the dictionary
 * however many commands match.
 *
 * ## Not thread safe
 *
 * None of this is safe for concurrent use, matching the rest of the module.  A
 * provider runs with the registry's mutation guard raised, so a provider that
 * tries to register, remove or reload through the registry is refused with
 * @c UFCOMMAND_CONFLICT rather than corrupting the iteration in progress.  A
 * provider must not destroy the registry it is called from.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UFCOMMAND_UFCOMMAND_COMPLETION_H
#define UFLIB_UFCOMMAND_UFCOMMAND_COMPLETION_H

#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/ufcommand/ufcommand_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief A completion engine.  Opaque, and owned by its registry.
 */
typedef struct UfCommandCompletion UfCommandCompletion;

/*!
 * @brief One query's candidates.  Opaque, and owned by the caller.
 */
typedef struct UfCommandCompletionResult UfCommandCompletionResult;

/*!
 * @brief What a candidate is.
 *
 * @c UFCOMMAND_COMPLETION_COMMAND and @c UFCOMMAND_COMPLETION_ALIAS are what the
 * registry's own dictionary provider emits.  The rest are for host providers to
 * label their contributions with; the module never produces them itself.
 */
typedef enum UfCommandCompletionKind
{
  UFCOMMAND_COMPLETION_COMMAND  = 1, ///< a registered command component
  UFCOMMAND_COMPLETION_ALIAS    = 2, ///< a registered alias name
  UFCOMMAND_COMPLETION_ARGUMENT = 3, ///< an argument value, from a host provider
  UFCOMMAND_COMPLETION_BINDING  = 4, ///< a key chord, from a host provider
  UFCOMMAND_COMPLETION_CUSTOM   = 5  ///< anything else
} UfCommandCompletionKind;

/*!
 * @brief What a provider is asked, and what it must not assume about.
 *
 * @c line and @c cursor are the host's, exactly as passed to
 * @c UfCommandCompletionSuggest(); @c cursor is a byte offset and may sit
 * anywhere in the line, not just at its end.
 *
 * The other three fields are the engine's reading of that line, offered so a
 * provider does not have to tokenise again.  All three are valid only for the
 * duration of the call.
 *
 * | Field | Meaning |
 * |---|---|
 * | @c token_prefix | the active token's text up to @c cursor — what has been typed so far |
 * | @c token_index | how many complete tokens precede the active token; 0 at the first |
 * | @c command_prefix | those preceding tokens joined by single spaces, i.e. canonicalised |
 *
 * @c command_prefix is canonicalised rather than a slice of @c line on purpose:
 * the dispatcher tokenises, so @c "remote  add  r" runs perfectly, and a
 * provider matching against the raw slice would see a doubled space and match
 * nothing.
 */
typedef struct UfCommandCompletionQuery
{
  const char *line;           ///< the host's input line, borrowed
  size_t      cursor;         ///< byte offset of the cursor within @c line
  const char *token_prefix;   ///< the active token's text before the cursor, borrowed
  size_t      token_index;    ///< number of complete tokens before the active one
  const char *command_prefix; ///< those tokens, canonicalised, borrowed
} UfCommandCompletionQuery;

/*!
 * @brief One suggestion.
 *
 * @c text is what would be inserted; it must be non-NULL and non-empty.
 * @c display and @c description are for presentation and may be NULL, in which
 * case they default to @c text and to the empty string.  All three are copied by
 * the engine, so a provider keeps ownership of whatever it points them at.
 */
typedef struct UfCommandCompletionCandidate
{
  const char *            text;        ///< insertion text; engine copies it
  const char *            display;     ///< NULL defaults to @c text
  const char *            description; ///< NULL defaults to ""
  UfCommandCompletionKind kind;        ///< see @c UfCommandCompletionKind
  double                  score;       ///< higher ranks first; negative and NaN read as 0
} UfCommandCompletionCandidate;

/*!
 * @brief Offer one candidate to the engine.
 *
 * @param emit_context the context the engine passed to the provider
 * @param candidate    the candidate; @c text must be non-NULL and non-empty
 * @return @c UFCOMMAND_OK, or @c UFCOMMAND_INVALID_ARGUMENT for a malformed
 *         candidate — which the provider is expected to return, aborting the
 *         call.  A provider that ignores this return value is choosing to
 *         swallow its own bug.
 */
typedef UfCommandStatus (*UfCommandCompletionEmit)(void *emit_context, const UfCommandCompletionCandidate *candidate);

/*!
 * @brief Contribute candidates for a query.
 *
 * @param provider_context the @c user_data registered with the provider
 * @param query            the query; valid only for the duration of this call
 * @param emit             the callback to offer candidates through
 * @param emit_context     opaque; passed straight back to @c emit
 * @return @c UFCOMMAND_OK to contribute normally.  Anything else aborts
 *         @c UfCommandCompletionSuggest() and becomes its return value.
 */
typedef UfCommandStatus (*UfCommandCompletionProvider)(void *provider_context, const UfCommandCompletionQuery *query,
                                                       UfCommandCompletionEmit emit, void *emit_context);

/*!
 * @brief A provider, registered with @c UfCommandCompletionAddProvider().
 *
 * @c name labels the provider for diagnostics and must be non-NULL and
 * non-empty.  @c user_data is **borrowed** — never freed by the library — and is
 * handed back as the provider's @c provider_context.
 */
typedef struct UfCommandCompletionProviderDefinition
{
  const char *                name;      ///< short label; must be non-empty
  UfCommandCompletionProvider provide;   ///< must not be NULL
  void *                      user_data; ///< borrowed; passed to @c provide
} UfCommandCompletionProviderDefinition;

/*!
 * @brief Where a result's candidates apply.
 *
 * @c replacement_start and @c replacement_end bound the **whole active token**,
 * so replacing @c line[replacement_start:replacement_end] with a candidate's
 * @c text is the operation a line editor wants.  The range is not
 * @c [start, cursor): with the cursor mid-token it still covers the token's tail,
 * which is what stops @c "remote ad" with the cursor after the @c a from being
 * completed into @c "remote dadd".
 *
 * @c replacement_end may therefore be greater than the cursor used for the
 * query.  When the cursor sits in whitespace the active token is empty and the
 * range is empty at the cursor, i.e. an insertion point.
 */
typedef struct UfCommandCompletionResultInfo
{
  size_t count;             ///< number of candidates, at most 32
  size_t replacement_start; ///< inclusive start of the active token
  size_t replacement_end;   ///< exclusive end of the active token
} UfCommandCompletionResultInfo;

/**
 * @brief The engine a registry created for itself, or NULL.
 *
 * Returns NULL when the registry did not opt in with @c completion_enabled, so a
 * host that never asked pays nothing and has nothing to check before calling.
 * The engine is owned by the registry and dies with it; the handle survives
 * @c UfCommandRegistryLoadUserConfig().
 *
 * @code{.c}
 * UfCommandCompletion *completion = UfCommandRegistryGetCompletion(registry);
 * if (completion == NULL) {
 *     return;  // this registry was not configured for completion
 * }
 * @endcode
 */
PUBLIC_API UfCommandCompletion *UfCommandRegistryGetCompletion(UfCommandRegistry *registry);

/**
 * @brief Add a provider to an engine.
 *
 * The registry's own dictionary provider is already installed and is not
 * counted here, so an engine accepts **seven** host providers; the eighth is
 * refused.  Providers are called in the order they were added, which matters
 * only for which of two equally-scored candidates wins.
 *
 * @param completion The engine; NULL is @c UFCOMMAND_INVALID_ARGUMENT.
 * @param provider The provider; @c name and @c provide must be non-NULL, and
 *        @c name non-empty.  Copied — the definition need not outlive the call,
 *        but @c name and @c user_data must, since both are borrowed.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_CONFLICT
 *         while a suggestion is already running, or @c UFCOMMAND_BUFFER_TOO_SMALL
 *         when the engine already holds the maximum.
 *
 * @code{.c}
 * UfCommandCompletionProviderDefinition domain = { "domain", OnDomainWord, my_words };
 * if (UfCommandCompletionAddProvider(completion, &domain) != UFCOMMAND_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandCompletionAddProvider(UfCommandCompletion *                        completion,
                                                          const UfCommandCompletionProviderDefinition *provider);

/**
 * @brief Ask an engine what the token at @p cursor could become.
 *
 * Every provider is called with the same query and the candidates are merged,
 * ranked and capped; the result is a snapshot that the caller owns and must
 * destroy with @c UfCommandCompletionResultDestroy().
 *
 * @param completion The engine; NULL is @c UFCOMMAND_INVALID_ARGUMENT.
 * @param line The input line; NULL is @c UFCOMMAND_INVALID_ARGUMENT.
 * @param cursor Byte offset within @p line; past the end is
 *        @c UFCOMMAND_INVALID_ARGUMENT.  Zero and a mid-line position are both
 *        valid.
 * @param out_result Receives the snapshot; set to NULL before any failure
 *        return.  A non-NULL value already there is **not** freed — the caller
 *        owns it, and reusing that slot without destroying its contents leaks.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_NO_MEMORY,
 *         @c UFCOMMAND_CONFLICT when called re-entrantly from inside a provider
 *         on the same engine, or whatever a provider itself returned.
 *
 * @code{.c}
 * UfCommandCompletionResult *result = NULL;
 * if (UfCommandCompletionSuggest(completion, "remote a", 8, &result) == UFCOMMAND_OK) {
 *     UfCommandCompletionResultInfo info;
 *     UfCommandCompletionResultGetInfo(result, &info);
 *     for (size_t i = 0; i < info.count; ++i) {
 *         const UfCommandCompletionCandidate *candidate =
 *             UfCommandCompletionResultGetCandidate(result, i);
 *         printf("%-20s %s\n", candidate->text, candidate->description);
 *     }
 *     UfCommandCompletionResultDestroy(result);
 * }
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandCompletionSuggest(UfCommandCompletion *completion, const char *line, size_t cursor,
                                                      UfCommandCompletionResult **out_result);

/**
 * @brief Destroy a result and the candidate strings it owns.  @p result may be NULL.
 *
 * Every pointer a @c ResultGet accessor handed out dies here.
 */
PUBLIC_API void UfCommandCompletionResultDestroy(UfCommandCompletionResult *result);

/**
 * @brief Read a result's count and the range its candidates replace.
 *
 * @return @c UFCOMMAND_OK, or @c UFCOMMAND_INVALID_ARGUMENT for a NULL argument.
 */
PUBLIC_API UfCommandStatus UfCommandCompletionResultGetInfo(const UfCommandCompletionResult *result,
                                                            UfCommandCompletionResultInfo *  out_info);

/** @brief Number of candidates, or 0 for a NULL result. */
PUBLIC_API size_t UfCommandCompletionResultGetCount(const UfCommandCompletionResult *result);

/**
 * @brief One candidate, or NULL when @p index is out of range.
 *
 * Borrowed from the result: valid until @c UfCommandCompletionResultDestroy().
 * The strings it points at are the engine's copies, so they outlive the line the
 * query was built from.
 */
PUBLIC_API const UfCommandCompletionCandidate *UfCommandCompletionResultGetCandidate(
    const UfCommandCompletionResult *result, size_t index);

#ifdef __cplusplus
}
#endif
#endif
