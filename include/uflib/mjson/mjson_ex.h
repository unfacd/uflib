//
// Created by ayman on 9/09/2026.
//

/**
 * @file mjson_ex.h
 * @brief mjson_ex
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 9/09/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFLIB_MJSON_EX_H
#define UFLIB_MJSON_EX_H

#include "mjson.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extended find – supports [*], .. and simple filters.
 * Returns the same token types as mjson_find().
 * For wildcards / recursive descent it returns the *first* match.
 */
int mjson_find_ex(const char *s, int len, const char *path,
                  const char **tokptr, int *toklen);

/* Callback called for every match */
typedef void (*mjson_match_cb_t)(const char *tok, int toklen,
                                 int toktype, void *userdata);

/* Find all matches and invoke the callback for each one.
 * Returns the number of matches found (or <0 on error).
 */
int mjson_find_all_ex(const char *s, int len, const char *path,
                      mjson_match_cb_t cb, void *userdata);

/* Convenience getters that use the extended engine */
int mjson_get_number_ex(const char *s, int len, const char *path, double *v);
int mjson_get_bool_ex  (const char *s, int len, const char *path, int *v);
int mjson_get_string_ex(const char *s, int len, const char *path,
                        char *to, int n);

#ifdef __cplusplus
}
#endif

#endif //UFLIB_MJSON_EX_H
