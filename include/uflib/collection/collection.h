//
// Created by ayman on 16/11/2026.
//

/**
 * @file collection.h
 * @brief collection
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 16/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFLIB_COLLECTION_H
#define UFLIB_COLLECTION_H

#include "collection/collection_descriptor_v2_type.h"

/**
 * Run the pipeline. `out` is fully initialised on every call.
 *
 * On failure: items = NULL, count = 0, nothing leaks, source released, only `skipped` is meaningful.
 * On success: the caller owns the output and releases it with CollectionResultFree(), passing the
 * same descriptor (needed for on_elem_destroy).
 */

CollectResult CollectorRun(const CollectionDescriptorV2 *descriptor, void *ctx, CollectionResult *out);

/** on_elem_destroy() on each element (if set), free(items), ArenaDestroy(). Idempotent, NULL-safe. */
void CollectionResultFree(const CollectionDescriptorV2 *descriptor, CollectionResult *out, void *ctx);

/** Human-readable name for a CollectResult (for logging). */
const char *CollectorResultStr(CollectResult r);


#endif //UFLIB_COLLECTION_H
