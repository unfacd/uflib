/**
 * @file ufoptional_type.h
 * @brief UfOptional public types — status codes and payload callbacks.
 *
 * `UfOptional` is opaque: it is declared here as an incomplete type and defined
 * only in the library's private headers, so a consumer cannot depend on its
 * layout and the library can change it without an ABI break.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_OPTIONAL_UFOPTIONAL_TYPE_H
#define UFLIB_OPTIONAL_UFOPTIONAL_TYPE_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An opaque handle to a container holding either one payload or nothing.
 */
typedef struct UfOptional UfOptional;

/*!
 * @brief Produce an independent copy of a payload.
 *
 * The library passes the payload it holds; the callback decides what "copy"
 * means for that payload type.  Returning NULL reports that the copy could not
 * be produced, and the call that needed it fails.
 *
 * @param value The payload to copy; never NULL.
 * @param context The context given to the call that requested the copy.
 * @return The copy, or NULL on failure.
 */
typedef void *(*UfOptionalCloneCallback)(const void *value, void *context);

/*!
 * @brief Release a payload the library owns.
 *
 * Called exactly once per payload the library takes ownership of, and never for
 * a payload the caller has taken back.  NULL is permitted wherever a callback is
 * expected, in which case the payload is dropped without being released.
 *
 * @param value The payload to release; never NULL.
 * @param context The context stored alongside the payload.
 */
typedef void (*UfOptionalDestroyCallback)(void *value, void *context);

/*!
 * @brief Decide whether a payload should be kept.
 *
 * @param value The payload to test; never NULL.
 * @param context The context given to the call that supplied the predicate.
 * @return `true` to keep the payload, `false` to discard it.
 */
typedef bool (*UfOptionalPredicateCallback)(const void *value, void *context);

/*!
 * @brief Act on a payload.
 *
 * @param value The payload to act on; never NULL.
 * @param context The context given to the call that supplied the consumer.
 * @return `true` if the action succeeded, `false` to report
 *         @c UFOPTIONAL_CALLBACK_FAILED.
 */
typedef bool (*UfOptionalConsumerCallback)(const void *value, void *context);

/*!
 * @brief Transform a payload into a new one.
 *
 * On success the library owns whatever is published through @p mapped_value,
 * including when that is NULL.  On failure the library still owns a non-NULL
 * published value and releases it with the call's destroy callback, so the
 * mapper must not release it as well.
 *
 * @param value The payload to transform; never NULL.
 * @param context The context given to the call that supplied the mapper.  The
 *                same context is later handed to the destroy callback.
 * @param mapped_value Receives the transformed payload, or NULL for an empty
 *                     result.  Must be written on success.
 * @return `true` if the transformation succeeded, `false` to report
 *         @c UFOPTIONAL_CALLBACK_FAILED.
 */
typedef bool (*UfOptionalMapperCallback)(const void *value, void *context, void **mapped_value);

/*!
 * @brief Transform a payload into another optional.
 *
 * Ownership of a non-NULL published optional transfers to the library on
 * success.  On failure a non-NULL published optional is destroyed by the
 * library, so the mapper must not destroy it as well.
 *
 * @param value The payload to transform; never NULL.
 * @param context The context given to the call that supplied the mapper.
 * @param mapped_optional Receives the transformed optional, or NULL.  Must be
 *                        written on success.
 * @return `true` if the transformation succeeded, `false` to report
 *         @c UFOPTIONAL_CALLBACK_FAILED.
 */
typedef bool (*UfOptionalFlatMapperCallback)(const void *value, void *context, UfOptional **mapped_optional);

/*!
 * @brief Outcome of an API call.
 *
 * The failures a caller has to distinguish are deliberately separate rather than
 * folded into one code: @c UFOPTIONAL_EMPTY is the container legitimately holding
 * nothing, @c UFOPTIONAL_INVALID_ARGUMENT is the host passing a bad pointer, and
 * @c UFOPTIONAL_ALLOCATION_FAILED is the allocator failing.
 */
typedef enum UfOptionalStatus
{
  UFOPTIONAL_OK = 0,            ///< The call did what was asked of it.
  UFOPTIONAL_EMPTY,             ///< The container held no payload.
  UFOPTIONAL_INVALID_ARGUMENT,  ///< A caller-supplied pointer or value is unusable.
  UFOPTIONAL_NOT_PRESENT,       ///< `OrElseThrow` found nothing to hand back.
  UFOPTIONAL_ALLOCATION_FAILED, ///< The allocator failed.
  UFOPTIONAL_CALLBACK_FAILED    ///< A caller-supplied callback reported failure.
} UfOptionalStatus;

#ifdef __cplusplus
}
#endif

#endif
