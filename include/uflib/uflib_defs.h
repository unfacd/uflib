/**
 * @file uflib_defs.h
 * @brief Preprocessor definitions shared across all public headers of uflib.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_DEFS_H
#define UFLIB_DEFS_H

/*!
 * Marker for public API functions — exported from the shared library.
 *
 * Place before the return type of every function that is part of a module's
 * public contract.  With -fvisibility=hidden, only PUBLIC_API symbols are
 * visible to consumers.  The attribute is harmless (no-op) in static builds.
 *
 * @code{.c}
 * PUBLIC_API void    HashTableDestroy(HashTable *ht);
 * PUBLIC_API bool    AddToHash(HashTable *ht, const void *key, void *value);
 * PUBLIC_API void   *HashLookup(HashTable *ht, const void *key);
 * @endcode
 */
#ifndef PUBLIC_API
  #if defined(__GNUC__) || defined(__clang__)
    #define PUBLIC_API __attribute__((visibility("default")))
  #else
    #define PUBLIC_API
  #endif
#endif

#endif /* UFLIB_DEFS_H */
