/**
 * @file ufoptional.c
 * @brief UfOptional — an opaque container holding one payload or nothing.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/optional/ufoptional.h>

#include "ufoptional_priv.h"

#include <stdlib.h>

// Presence is derived from the payload, so the two can never disagree.
static UfOptional *
sCreate(void *value, UfOptionalDestroyCallback destroy, void *context)
{
  UfOptional *optional = malloc(sizeof(*optional));

  if (optional == NULL) {
    if (value != NULL && destroy != NULL) {
      destroy(value, context);
    }
    return NULL;
  }

  optional->is_present  = value != NULL;
  optional->value       = value;
  optional->destroy     = destroy;
  optional->context     = context;
  optional->last_status = UFOPTIONAL_OK;
  return optional;
}

static UfOptionalStatus
sRecord(UfOptional *optional, UfOptionalStatus status)
{
  optional->last_status = status;
  return status;
}

// Drops the payload without releasing it: the caller has taken ownership.
static void
sForget(UfOptional *optional)
{
  optional->value      = NULL;
  optional->is_present = false;
  optional->destroy    = NULL;
  optional->context    = NULL;
}

// Releases the payload and leaves the container empty, but still usable.
static void
sClear(UfOptional *optional)
{
  if (optional->is_present && optional->destroy != NULL) {
    optional->destroy(optional->value, optional->context);
  }
  sForget(optional);
}

PUBLIC_API UfOptional *
UfOptionalEmpty(UfOptionalDestroyCallback destroy, void *context)
{
  return sCreate(NULL, destroy, context);
}

PUBLIC_API UfOptional *
UfOptionalOf(void *value, UfOptionalDestroyCallback destroy, void *context)
{
  if (value == NULL) {
    return NULL;
  }
  return sCreate(value, destroy, context);
}

PUBLIC_API UfOptional *
UfOptionalOfNullable(void *value, UfOptionalDestroyCallback destroy, void *context)
{
  return sCreate(value, destroy, context);
}

PUBLIC_API UfOptional *
UfOptionalClone(const UfOptional *source, UfOptionalCloneCallback clone, UfOptionalDestroyCallback destroy,
                void *            context)
{
  void *copy;

  if (source == NULL || clone == NULL) {
    return NULL;
  }
  if (!source->is_present) {
    return sCreate(NULL, destroy, context);
  }

  copy = clone(source->value, context);
  if (copy == NULL) {
    return NULL;
  }
  return sCreate(copy, destroy, context);
}

PUBLIC_API void
UfOptionalDestroy(UfOptional *optional)
{
  if (optional == NULL) {
    return;
  }
  if (optional->is_present && optional->destroy != NULL) {
    optional->destroy(optional->value, optional->context);
  }
  free(optional);
}

PUBLIC_API bool
UfOptionalIsPresent(const UfOptional *optional)
{
  return optional != NULL && optional->is_present;
}

PUBLIC_API bool
UfOptionalIsEmpty(const UfOptional *optional)
{
  return optional == NULL || !optional->is_present;
}

PUBLIC_API void *
UfOptionalGet(const UfOptional *optional)
{
  return (optional != NULL && optional->is_present) ? optional->value : NULL;
}

PUBLIC_API void *
UfOptionalTake(UfOptional *optional)
{
  void *value;

  if (optional == NULL) {
    return NULL;
  }
  if (!optional->is_present) {
    sRecord(optional, UFOPTIONAL_EMPTY);
    return NULL;
  }

  value = optional->value;
  sForget(optional);
  sRecord(optional, UFOPTIONAL_OK);
  return value;
}

PUBLIC_API void *
UfOptionalOrElse(const UfOptional *optional, void *other)
{
  return (optional != NULL && optional->is_present) ? optional->value : other;
}

PUBLIC_API void *
UfOptionalOrElseGet(const UfOptional *optional, void *(*supplier)(void *context), void *context)
{
  if (optional != NULL && optional->is_present) {
    return optional->value;
  }
  return supplier != NULL ? supplier(context) : NULL;
}

PUBLIC_API UfOptionalStatus
UfOptionalOrElseThrow(const UfOptional *optional, void **out_value)
{
  if (out_value == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  *out_value = NULL;

  if (optional == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  if (!optional->is_present) {
    return UFOPTIONAL_NOT_PRESENT;
  }

  *out_value = optional->value;
  return UFOPTIONAL_OK;
}

PUBLIC_API UfOptionalStatus
UfOptionalIfPresent(const UfOptional *optional, UfOptionalConsumerCallback consumer, void *context)
{
  if (optional == NULL || consumer == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  if (!optional->is_present) {
    return UFOPTIONAL_EMPTY;
  }
  return consumer(optional->value, context) ? UFOPTIONAL_OK : UFOPTIONAL_CALLBACK_FAILED;
}

PUBLIC_API UfOptionalStatus
UfOptionalFilter(UfOptional *optional, UfOptionalPredicateCallback predicate, void *context)
{
  if (optional == NULL || predicate == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  if (!optional->is_present) {
    return sRecord(optional, UFOPTIONAL_EMPTY);
  }

  if (!predicate(optional->value, context)) {
    sClear(optional);
  }
  return sRecord(optional, UFOPTIONAL_OK);
}

PUBLIC_API UfOptionalStatus
UfOptionalMap(const UfOptional *        optional, UfOptionalMapperCallback mapper, void *context,
              UfOptionalDestroyCallback mapped_destroy, UfOptional **      out_optional)
{
  void *mapped = NULL;

  if (out_optional == NULL || optional == NULL || mapper == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  *out_optional = NULL;

  if (!optional->is_present) {
    *out_optional = sCreate(NULL, mapped_destroy, context);
    return *out_optional == NULL ? UFOPTIONAL_ALLOCATION_FAILED : UFOPTIONAL_EMPTY;
  }

  if (!mapper(optional->value, context, &mapped)) {
    // A mapper that published before failing leaves the release to us, so the
    // failure path and the success path share one owner.
    if (mapped != NULL && mapped_destroy != NULL) {
      mapped_destroy(mapped, context);
    }
    return UFOPTIONAL_CALLBACK_FAILED;
  }

  *out_optional = sCreate(mapped, mapped_destroy, context);
  return *out_optional == NULL ? UFOPTIONAL_ALLOCATION_FAILED : UFOPTIONAL_OK;
}

PUBLIC_API UfOptionalStatus
UfOptionalFlatMap(const UfOptional *optional, UfOptionalFlatMapperCallback mapper, void *context,
                  UfOptional **     out_optional)
{
  UfOptional *mapped = NULL;

  if (out_optional == NULL || optional == NULL || mapper == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }
  *out_optional = NULL;

  if (!optional->is_present) {
    mapped = sCreate(NULL, NULL, NULL);
    if (mapped == NULL) {
      return UFOPTIONAL_ALLOCATION_FAILED;
    }
    *out_optional = mapped;
    return UFOPTIONAL_EMPTY;
  }

  if (!mapper(optional->value, context, &mapped)) {
    // As in Map: publishing before failing does not transfer the release to the mapper.
    if (mapped != NULL) {
      UfOptionalDestroy(mapped);
    }
    return UFOPTIONAL_CALLBACK_FAILED;
  }
  if (mapped == NULL) {
    return UFOPTIONAL_CALLBACK_FAILED;
  }

  *out_optional = mapped;
  return UFOPTIONAL_OK;
}

PUBLIC_API UfOptionalStatus
UfOptionalSet(UfOptional *optional, void *value, UfOptionalDestroyCallback destroy, void *context)
{
  if (optional == NULL) {
    return UFOPTIONAL_INVALID_ARGUMENT;
  }

  // Re-setting the payload already held must not release it: the stored pointer would dangle the moment it was written back.
  if (value != NULL && optional->is_present && value == optional->value) {
    optional->destroy = destroy;
    optional->context = context;
    return sRecord(optional, UFOPTIONAL_OK);
  }

  sClear(optional);
  optional->is_present = value != NULL;
  optional->value      = value;
  optional->destroy    = destroy;
  optional->context    = context;
  return sRecord(optional, UFOPTIONAL_OK);
}

PUBLIC_API UfOptionalStatus
UfOptionalLastStatus(const UfOptional *optional)
{
  return optional == NULL ? UFOPTIONAL_INVALID_ARGUMENT : optional->last_status;
}
