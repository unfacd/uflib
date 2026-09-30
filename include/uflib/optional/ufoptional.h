/**
 * @file ufoptional.h
 * @brief UfOptional — an opaque container holding one payload or nothing.
 *
 * UfOptional follows the semantics of Java's `Optional` where those semantics
 * survive the crossing into C.  C has no generics, no exceptions, no references
 * and no collector, so two things are made explicit that Java leaves implicit:
 * the payload is a `void *`, and every operation that can fail reports a status
 * rather than throwing.
 *
 * ## Ownership
 *
 * The container owns its payload.  A constructor takes ownership of a non-NULL
 * payload on success, `UfOptionalMap()` and `UfOptionalFlatMap()` take ownership
 * of what the mapper publishes, and `UfOptionalTake()` hands ownership back to
 * the caller.  Destruction and transfer both go through the destroy callback
 * fixed when the payload was stored, so the container never has to know what
 * kind of payload it holds.
 *
 * ## Null handling
 *
 * A NULL handle is not a valid container.  Every read accessor tolerates one and
 * reports empty rather than crashing, so a host that has not yet built an
 * optional does not need a guard branch at each use.  Constructors, by contrast,
 * return NULL to report failure, and a caller that ignores the return value will
 * leak the payload it handed over.
 *
 * ## Java mapping
 *
 * | Java | C |
 * |---|---|
 * | `Optional.empty()` | `UfOptionalEmpty()` |
 * | `Optional.of(v)` | `UfOptionalOf()` |
 * | `Optional.ofNullable(v)` | `UfOptionalOfNullable()` |
 * | `isPresent()` / `isEmpty()` | `UfOptionalIsPresent()` / `UfOptionalIsEmpty()` |
 * | `get()` | `UfOptionalGet()` |
 * | `orElse()` / `orElseGet()` / `orElseThrow()` | `UfOptionalOrElse()` / `UfOptionalOrElseGet()` / `UfOptionalOrElseThrow()` |
 * | `ifPresent()` / `filter()` | `UfOptionalIfPresent()` / `UfOptionalFilter()` |
 * | `map()` / `flatMap()` | `UfOptionalMap()` / `UfOptionalFlatMap()` |
 *
 * `get()` cannot throw in C, so on an empty container it returns NULL.
 * `orElseThrow()` reports @c UFOPTIONAL_NOT_PRESENT instead.
 *
 * ## Thread safety
 *
 * None.  A container is single-owner; if one is shared between threads,
 * synchronization belongs at that boundary, outside this API.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_OPTIONAL_UFOPTIONAL_H
#define UFLIB_OPTIONAL_UFOPTIONAL_H

#include <stdbool.h>

#include <uflib/uflib_defs.h>
#include <uflib/optional/ufoptional_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a container holding nothing.
 *
 * @param destroy Releases a payload later stored here, or NULL for none.
 * @param context Context handed to @p destroy alongside the payload.
 * @return The new container, or NULL if the allocator failed.
 *
 * @code{.c}
 * UfOptional *opt = UfOptionalEmpty(free, NULL);
 * if (opt == NULL) {
 *     return EXIT_FAILURE;
 * }
 * UfOptionalDestroy(opt);
 * @endcode
 */
PUBLIC_API UfOptional *UfOptionalEmpty(UfOptionalDestroyCallback destroy, void *context);

/**
 * @brief Create a container holding @p value, which must not be NULL.
 *
 * Ownership of @p value passes to the container on success.  On failure it does
 * not: the payload is released through @p destroy, and the caller must not touch
 * it again.
 *
 * @param value The payload to hold; NULL is rejected.
 * @param destroy Releases the payload, or NULL for none.
 * @param context Context handed to @p destroy alongside the payload.
 * @return The new container, or NULL if @p value was NULL or the allocator
 *         failed.
 *
 * @code{.c}
 * int *n = malloc(sizeof(*n));
 * if (n == NULL) {
 *     return EXIT_FAILURE;
 * }
 * *n = 42;
 *
 * UfOptional *opt = UfOptionalOf(n, free, NULL);
 * if (opt == NULL) {
 *     return EXIT_FAILURE;   // n has already been freed
 * }
 * UfOptionalDestroy(opt);
 * @endcode
 */
PUBLIC_API UfOptional *UfOptionalOf(void *value, UfOptionalDestroyCallback destroy, void *context);

/**
 * @brief Create a container holding @p value, or nothing when it is NULL.
 *
 * The tolerant constructor: a NULL payload yields an empty container rather than
 * a failure, which is what a caller wants when the payload came from somewhere
 * that may legitimately produce nothing.
 *
 * @param value The payload to hold, or NULL for an empty container.
 * @param destroy Releases the payload, or NULL for none.
 * @param context Context handed to @p destroy alongside the payload.
 * @return The new container, or NULL if the allocator failed.
 *
 * @code{.c}
 * UfOptional *opt = UfOptionalOfNullable(lookup(key), free, NULL);
 * if (opt == NULL) {
 *     return EXIT_FAILURE;
 * }
 * if (UfOptionalIsEmpty(opt)) {
 *     puts("no such key");
 * }
 * UfOptionalDestroy(opt);
 * @endcode
 */
PUBLIC_API UfOptional *UfOptionalOfNullable(void *value, UfOptionalDestroyCallback destroy, void *context);

/**
 * @brief Deep-copy a container, cloning its payload.
 *
 * An empty @p source clones to an empty container without invoking @p clone.
 *
 * @param source The container to copy.
 * @param clone Produces the payload copy; NULL is rejected.
 * @param destroy Releases the copy, or NULL for none.
 * @param context Context handed to @p clone, and later to @p destroy.
 * @return The new container, or NULL if an argument was NULL, if @p clone
 *         returned NULL, or if the allocator failed.
 *
 * @code{.c}
 * static void *clone_int(const void *value, void *context)
 * {
 *     (void)context;
 *     int *copy = malloc(sizeof(*copy));
 *     if (copy != NULL) {
 *         *copy = *(const int *)value;
 *     }
 *     return copy;
 * }
 *
 * // ...
 * UfOptional *copy = UfOptionalClone(original, clone_int, free, NULL);
 * if (copy == NULL) {
 *     return EXIT_FAILURE;
 * }
 * UfOptionalDestroy(copy);
 * @endcode
 */
PUBLIC_API UfOptional *UfOptionalClone(const UfOptional *        source, UfOptionalCloneCallback clone,
                                       UfOptionalDestroyCallback destroy, void *                 context);

/**
 * @brief Destroy a container, releasing the payload it still owns.
 *
 * A payload that was taken with `UfOptionalTake()` is not released here, because
 * the container no longer owns it.  NULL is a no-op.
 *
 * @param optional The container to destroy, or NULL.
 *
 * @code{.c}
 * UfOptional *opt = UfOptionalOf(malloc(1), free, NULL);
 * UfOptionalDestroy(opt);   // the payload is freed here
 * opt = NULL;
 * @endcode
 */
PUBLIC_API void UfOptionalDestroy(UfOptional *optional);

/**
 * @brief Whether the container holds a payload.
 *
 * @param optional The container, or NULL.
 * @return `true` only if @p optional is non-NULL and holds a payload.
 *
 * @code{.c}
 * if (UfOptionalIsPresent(opt)) {
 *     puts("have a value");
 * }
 * @endcode
 */
PUBLIC_API bool UfOptionalIsPresent(const UfOptional *optional);

/**
 * @brief Whether the container holds nothing.
 *
 * The complement of `UfOptionalIsPresent()`, with a NULL handle counting as
 * empty.
 *
 * @param optional The container, or NULL.
 * @return `true` if @p optional is NULL or holds no payload.
 *
 * @code{.c}
 * if (UfOptionalIsEmpty(opt)) {
 *     puts("nothing to do");
 * }
 * @endcode
 */
PUBLIC_API bool UfOptionalIsEmpty(const UfOptional *optional);

/**
 * @brief Borrow the payload without taking ownership.
 *
 * The container keeps ownership: the pointer stays valid only until the container
 * is destroyed, taken, set, or filtered away.  To keep it, use
 * `UfOptionalTake()`.
 *
 * @param optional The container, or NULL.
 * @return The payload, or NULL if @p optional is NULL or empty.
 *
 * @code{.c}
 * const char *name = UfOptionalGet(opt);
 * if (name != NULL) {
 *     printf("name: %s\n", name);
 * }
 * @endcode
 */
PUBLIC_API void *UfOptionalGet(const UfOptional *optional);

/**
 * @brief Take the payload, leaving the container empty.
 *
 * Ownership passes to the caller, who must release it.  The container's destroy
 * callback is not invoked and is forgotten along with the payload.
 *
 * @param optional The container, or NULL.
 * @return The payload, or NULL if @p optional is NULL or empty.
 *
 * @code{.c}
 * int *n = UfOptionalTake(opt);
 * if (n != NULL) {
 *     printf("took %d\n", *n);
 *     free(n);
 * }
 * UfOptionalDestroy(opt);   // nothing left to release
 * @endcode
 */
PUBLIC_API void *UfOptionalTake(UfOptional *optional);

/**
 * @brief The payload, or @p other when the container is empty.
 *
 * @p other is returned as given and never owned, so it is usually a pointer to
 * something the caller already holds.
 *
 * @param optional The container, or NULL.
 * @param other Returned when there is no payload.
 * @return The payload, or @p other.
 *
 * @code{.c}
 * int fallback = 0;
 * int *n = UfOptionalOrElse(opt, &fallback);
 * @endcode
 */
PUBLIC_API void *UfOptionalOrElse(const UfOptional *optional, void *other);

/**
 * @brief The payload, or what @p supplier produces when the container is empty.
 *
 * The supplier is called only when it is needed, so an expensive fallback costs
 * nothing when a payload is present.
 *
 * @param optional The container, or NULL.
 * @param supplier Produces the fallback; NULL yields NULL.
 * @param context Context handed to @p supplier.
 * @return The payload, or the supplier's result.  The caller owns neither.
 *
 * @code{.c}
 * void *defaults(void *context)
 * {
 *     return context;
 * }
 *
 * // ...
 * int fallback = 0;
 * int *n = UfOptionalOrElseGet(opt, defaults, &fallback);
 * @endcode
 */
PUBLIC_API void *UfOptionalOrElseGet(const UfOptional *optional, void *(*supplier)(void *context), void *context);

/**
 * @brief The payload, or a status reporting that there was none.
 *
 * Java's `orElseThrow()`, expressed as a status and an out-parameter because C
 * has no exception to throw.  @p out_value is cleared before any test, so it
 * never reports a stale payload on failure.
 *
 * @param optional The container, or NULL.
 * @param out_value Receives the payload; set to NULL unless the call succeeds.
 * @return @c UFOPTIONAL_OK, @c UFOPTIONAL_NOT_PRESENT when the container is
 *         empty, or @c UFOPTIONAL_INVALID_ARGUMENT for a NULL argument.
 *
 * @code{.c}
 * void *value = NULL;
 * UfOptionalStatus st = UfOptionalOrElseThrow(opt, &value);
 * if (st == UFOPTIONAL_NOT_PRESENT) {
 *     fprintf(stderr, "required value is missing\n");
 *     return EXIT_FAILURE;
 * }
 * if (st != UFOPTIONAL_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalOrElseThrow(const UfOptional *optional, void **out_value);

/**
 * @brief Act on the payload when there is one.
 *
 * @param optional The container, or NULL.
 * @param consumer The action; NULL is rejected.
 * @param context Context handed to @p consumer.
 * @return @c UFOPTIONAL_OK, @c UFOPTIONAL_EMPTY when there is no payload,
 *         @c UFOPTIONAL_INVALID_ARGUMENT for a NULL argument, or
 *         @c UFOPTIONAL_CALLBACK_FAILED if @p consumer returned `false`.
 *
 * @code{.c}
 * static bool print_int(const void *value, void *context)
 * {
 *     (void)context;
 *     printf("%d\n", *(const int *)value);
 *     return true;
 * }
 *
 * // ...
 * if (UfOptionalIfPresent(opt, print_int, NULL) != UFOPTIONAL_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalIfPresent(const UfOptional *optional, UfOptionalConsumerCallback consumer,
                                                void *            context);

/**
 * @brief Discard the payload unless it satisfies @p predicate.
 *
 * A rejected payload is released through the container's destroy callback and
 * the container becomes empty.  A container that is already empty is left alone.
 *
 * @param optional The container, or NULL.
 * @param predicate The test; NULL is rejected.
 * @param context Context handed to @p predicate.
 * @return @c UFOPTIONAL_OK, or @c UFOPTIONAL_INVALID_ARGUMENT for a NULL
 *         argument.
 *
 * @code{.c}
 * static bool is_even(const void *value, void *context)
 * {
 *     (void)context;
 *     return (*(const int *)value % 2) == 0;
 * }
 *
 * // ...
 * UfOptionalFilter(opt, is_even, NULL);
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalFilter(UfOptional *                optional,
                                             UfOptionalPredicateCallback predicate, void *context);

/**
 * @brief Transform the payload and store the result in a new container.
 *
 * The source is untouched.  A mapper may publish NULL to produce an empty
 * container, and an empty source produces an empty result without invoking the
 * mapper.
 *
 * @param optional The source container, or NULL.
 * @param mapper The transformation; NULL is rejected.
 * @param context Context handed to @p mapper, and later to @p mapped_destroy.
 * @param mapped_destroy Releases the mapped payload, or NULL for none.
 * @param out_optional Receives the new container; set to NULL unless the call
 *                     succeeds.  Must not be the address of @p optional itself,
 *                     which would lose the only handle to the source.
 * @return @c UFOPTIONAL_OK, @c UFOPTIONAL_EMPTY when the source held nothing,
 *         @c UFOPTIONAL_INVALID_ARGUMENT for a NULL argument,
 *         @c UFOPTIONAL_CALLBACK_FAILED if @p mapper returned `false`, or
 *         @c UFOPTIONAL_ALLOCATION_FAILED.
 *
 * @code{.c}
 * static bool double_int(const void *value, void *context, void **mapped)
 * {
 *     (void)context;
 *     int *n = malloc(sizeof(*n));
 *     if (n == NULL) {
 *         return false;
 *     }
 *     *n = *(const int *)value * 2;
 *     *mapped = n;
 *     return true;
 * }
 *
 * // ...
 * UfOptional *doubled = NULL;
 * if (UfOptionalMap(opt, double_int, NULL, free, &doubled) != UFOPTIONAL_OK) {
 *     return EXIT_FAILURE;
 * }
 * UfOptionalDestroy(doubled);
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalMap(const UfOptional *optional, UfOptionalMapperCallback mapper, void *context,
                                          UfOptionalDestroyCallback mapped_destroy, UfOptional **out_optional);

/**
 * @brief Transform the payload into another container, without nesting.
 *
 * Where `UfOptionalMap()` would yield a container of a container, this yields the
 * inner one directly, so a chain of transformations that may each produce nothing
 * stays flat.
 *
 * @param optional The source container, or NULL.
 * @param mapper The transformation; NULL is rejected.  It must publish a
 *               container on success.
 * @param context Context handed to @p mapper.
 * @param out_optional Receives the new container; set to NULL unless the call
 *                     succeeds.  Must not be the address of @p optional itself.
 * @return @c UFOPTIONAL_OK, @c UFOPTIONAL_EMPTY when the source held nothing,
 *         @c UFOPTIONAL_INVALID_ARGUMENT for a NULL argument,
 *         @c UFOPTIONAL_CALLBACK_FAILED if @p mapper returned `false` or
 *         published nothing, or @c UFOPTIONAL_ALLOCATION_FAILED.
 *
 * @code{.c}
 * static bool parse_int(const void *value, void *context, UfOptional **mapped)
 * {
 *     (void)context;
 *     int *n = malloc(sizeof(*n));
 *     if (n == NULL || sscanf((const char *)value, "%d", n) != 1) {
 *         free(n);
 *         return false;
 *     }
 *     *mapped = UfOptionalOf(n, free, NULL);
 *     return *mapped != NULL;
 * }
 *
 * // ...
 * UfOptional *parsed = NULL;
 * if (UfOptionalFlatMap(opt, parse_int, NULL, &parsed) != UFOPTIONAL_OK) {
 *     return EXIT_FAILURE;
 * }
 * UfOptionalDestroy(parsed);
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalFlatMap(const UfOptional *optional, UfOptionalFlatMapperCallback mapper,
                                              void *            context, UfOptional **                 out_optional);

/**
 * @brief Replace the payload, releasing whatever the container held.
 *
 * A NULL @p value empties the container.  Re-setting the payload the container
 * already holds is a no-op on the payload: it is kept, and only the destroy
 * callback and context are replaced.
 *
 * @param optional The container, or NULL.
 * @param value The new payload, or NULL to empty the container.
 * @param destroy Releases @p value, or NULL for none.
 * @param context Context handed to @p destroy.
 * @return @c UFOPTIONAL_OK, or @c UFOPTIONAL_INVALID_ARGUMENT for a NULL handle.
 *
 * @code{.c}
 * UfOptional *opt = UfOptionalEmpty(free, NULL);
 * int *n = malloc(sizeof(*n));
 * *n = 5;
 * UfOptionalSet(opt, n, free, NULL);
 * UfOptionalSet(opt, NULL, free, NULL);   // empties, freeing n
 * UfOptionalDestroy(opt);
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalSet(UfOptional *optional, void *value, UfOptionalDestroyCallback destroy,
                                          void *      context);

/**
 * @brief The outcome of the most recent state-changing call.
 *
 * Only the calls that can change what the container holds record here:
 * `UfOptionalSet()` always records @c UFOPTIONAL_OK; `UfOptionalTake()` and
 * `UfOptionalFilter()` record @c UFOPTIONAL_OK when they had a payload to work
 * on, and @c UFOPTIONAL_EMPTY when the container was already empty and there was
 * nothing to do.  Read-only calls — `UfOptionalGet()`, `UfOptionalIsPresent()`,
 * `UfOptionalOrElse*()`, `UfOptionalIfPresent()`, `UfOptionalMap()`,
 * `UfOptionalFlatMap()` — never record, because they take a const handle and
 * have no business changing the container at all.  A new container starts at
 * @c UFOPTIONAL_OK.
 *
 * @param optional The container, or NULL.
 * @return The recorded status, or @c UFOPTIONAL_INVALID_ARGUMENT for NULL.
 *
 * @code{.c}
 * UfOptionalSet(opt, value, free, NULL);
 * if (UfOptionalLastStatus(opt) != UFOPTIONAL_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfOptionalStatus UfOptionalLastStatus(const UfOptional *optional);

#ifdef __cplusplus
}
#endif

#endif
