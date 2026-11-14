/**
 * @file alias_map_json.c
 * @brief AliasMap JSON descriptor parsing — see alias_map.h for the contract.
 *
 * Strict reader for the bulk-construction document: an array of group objects,
 * each with one string value and a non-empty array of string keys.  The parsed
 * groups are handed to AliasMapCreateWithDescriptor(), so validation, duplicate
 * detection, bucket sizing and linking are shared with the struct pathway; this
 * file owns only the JSON grammar and its error offsets.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "alias_map_priv.h"

#include <uflib/cdt/alias_map/alias_map.h>
#include <uflib/logger/logger.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*! JSON failures also record the byte offset of the offending token. */
static void
sJsonResultFill(AliasMapCreateResult *result_ptr, AliasMapCreateStatus status,
                size_t json_offset)
{
    AliasMapResultFill(result_ptr, status, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0, 0);

    if (result_ptr) result_ptr->json_offset = json_offset;
}

/* ── JSON document parsing ─────────────────────────────────────────────── */

typedef struct {
    const char          *cursor_ptr;
    const char          *start_ptr;
    AliasMapCreateStatus status;
    size_t               offset;
} AliasMapJsonCursor;

typedef struct {
    AliasMapCollector *collectors_ptr;
    size_t             count;
    size_t             capacity;
} AliasMapJsonGroups;

static void
sJsonError(AliasMapJsonCursor *cur, AliasMapCreateStatus status)
{
    cur->status = status;
    cur->offset = (size_t)(cur->cursor_ptr - cur->start_ptr);
}

static void
sJsonSkipWhitespace(AliasMapJsonCursor *cur)
{
    while (*cur->cursor_ptr == ' ' || *cur->cursor_ptr == '\t' ||
           *cur->cursor_ptr == '\n' || *cur->cursor_ptr == '\r')
        cur->cursor_ptr++;
}

static void
sJsonFreeKeys(char **keys_ptr, size_t key_count)
{
    for (size_t i = 0; i < key_count; i++) free(keys_ptr[i]);

    free(keys_ptr);
}

static void
sJsonGroupsFree(AliasMapJsonGroups *groups)
{
    for (size_t i = 0; i < groups->count; i++) {
        AliasMapCollector *collector_ptr = &groups->collectors_ptr[i];

        free((char *)collector_ptr->value_ptr);
        sJsonFreeKeys((char **)collector_ptr->aliases_ptr, collector_ptr->alias_count);
    }

    free(groups->collectors_ptr);
    groups->collectors_ptr = NULL;
    groups->count          = 0;
    groups->capacity       = 0;
}

static bool
sJsonGroupsAppend(AliasMapJsonGroups *groups, char *value_ptr, char **keys_ptr,
                  size_t key_count)
{
    if (groups->count == groups->capacity) {
        size_t             capacity = groups->capacity ? groups->capacity * 2 : 8;
        AliasMapCollector *grown    = realloc(groups->collectors_ptr, capacity * sizeof *grown);

        if (!grown) return false;

        groups->collectors_ptr = grown;
        groups->capacity       = capacity;
    }

    groups->collectors_ptr[groups->count++] = (AliasMapCollector){
        .value_ptr   = value_ptr,
        .aliases_ptr = (const char *const *)keys_ptr,
        .alias_count = key_count,
    };

    return true;
}

static int
sJsonHexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

static size_t
sJsonUtf8Encode(unsigned code_point, char out[4])
{
    if (code_point <= 0x7F) {
        out[0] = (char)code_point;
        return 1;
    }

    if (code_point <= 0x7FF) {
        out[0] = (char)(0xC0 | (code_point >> 6));
        out[1] = (char)(0x80 | (code_point & 0x3F));
        return 2;
    }

    if (code_point <= 0xFFFF) {
        out[0] = (char)(0xE0 | (code_point >> 12));
        out[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code_point & 0x3F));
        return 3;
    }

    if (code_point <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (code_point >> 18));
        out[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code_point & 0x3F));
        return 4;
    }

    return 0;
}

/*! Parse one JSON string, decoding escapes into a fresh allocation. */
static bool
sJsonParseString(AliasMapJsonCursor *cur, char **out_ptr)
{
    if (*cur->cursor_ptr != '"') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
        return false;
    }

    const char *start    = cur->cursor_ptr + 1;
    size_t      capacity = strlen(start) + 1; /* decoding never grows a string */
    char       *buffer   = malloc(capacity);

    if (!buffer) {
        sJsonError(cur, ALIASMAP_CREATE_NO_MEMORY);
        return false;
    }

    size_t      used = 0;
    const char *p    = start;

    for (;;) {
        unsigned char c = (unsigned char)*p;

        if (c == '\0') {
            free(buffer);
            cur->cursor_ptr = p;
            sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
            return false;
        }

        if (c == '"') {
            p++;
            break;
        }

        if (c < 0x20) {
            free(buffer);
            cur->cursor_ptr = p;
            sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
            return false;
        }

        if (c != '\\') {
            buffer[used++] = (char)c;
            p++;
            continue;
        }

        char escape = p[1];

        switch (escape) {
        case '"':  buffer[used++] = '"';  p += 2; break;
        case '\\': buffer[used++] = '\\'; p += 2; break;
        case '/':  buffer[used++] = '/';  p += 2; break;
        case 'b':  buffer[used++] = '\b'; p += 2; break;
        case 'f':  buffer[used++] = '\f'; p += 2; break;
        case 'n':  buffer[used++] = '\n'; p += 2; break;
        case 'r':  buffer[used++] = '\r'; p += 2; break;
        case 't':  buffer[used++] = '\t'; p += 2; break;
        case 'u': {
            unsigned code_point = 0;

            p += 2;

            for (int i = 0; i < 4; i++) {
                int digit = sJsonHexValue(p[i]);

                if (digit < 0) {
                    free(buffer);
                    cur->cursor_ptr = p;
                    sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                    return false;
                }

                code_point = (code_point << 4) | (unsigned)digit;
            }

            p += 4;

            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                if (p[0] != '\\' || p[1] != 'u') {
                    free(buffer);
                    cur->cursor_ptr = p;
                    sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                    return false;
                }

                unsigned low = 0;

                p += 2;

                for (int i = 0; i < 4; i++) {
                    int digit = sJsonHexValue(p[i]);

                    if (digit < 0) {
                        free(buffer);
                        cur->cursor_ptr = p;
                        sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                        return false;
                    }

                    low = (low << 4) | (unsigned)digit;
                }

                p += 4;

                if (low < 0xDC00 || low > 0xDFFF) {
                    free(buffer);
                    cur->cursor_ptr = p;
                    sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                    return false;
                }

                code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
            }
            else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                free(buffer);
                cur->cursor_ptr = p;
                sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                return false;
            }

            char   encoded[4];
            size_t encoded_len = sJsonUtf8Encode(code_point, encoded);

            if (encoded_len == 0) {
                free(buffer);
                cur->cursor_ptr = p;
                sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
                return false;
            }

            for (size_t i = 0; i < encoded_len; i++) buffer[used++] = encoded[i];
            break;
        }
        default:
            free(buffer);
            cur->cursor_ptr = p;
            sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
            return false;
        }
    }

    buffer[used]       = '\0';
    cur->cursor_ptr    = p;
    *out_ptr           = buffer;

    return true;
}

static bool
sJsonParseKeys(AliasMapJsonCursor *cur, char ***keys_out_ptr, size_t *key_count_out_ptr)
{
    char  **keys_ptr  = NULL;
    size_t  key_count = 0;
    size_t  capacity  = 0;

    if (*cur->cursor_ptr != '[') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
        return false;
    }

    cur->cursor_ptr++;
    sJsonSkipWhitespace(cur);

    if (*cur->cursor_ptr == ']') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT); /* empty keys array */
        return false;
    }

    for (;;) {
        char *key_ptr = NULL;

        if (!sJsonParseString(cur, &key_ptr)) {
            sJsonFreeKeys(keys_ptr, key_count);
            return false;
        }

        if (key_count == capacity) {
            size_t  new_capacity = capacity ? capacity * 2 : 8;
            char  **grown        = realloc(keys_ptr, new_capacity * sizeof *grown);

            if (!grown) {
                free(key_ptr);
                sJsonFreeKeys(keys_ptr, key_count);
                sJsonError(cur, ALIASMAP_CREATE_NO_MEMORY);
                return false;
            }

            keys_ptr = grown;
            capacity = new_capacity;
        }

        keys_ptr[key_count++] = key_ptr;

        sJsonSkipWhitespace(cur);

        if (*cur->cursor_ptr == ',') {
            cur->cursor_ptr++;
            sJsonSkipWhitespace(cur);
            continue;
        }

        if (*cur->cursor_ptr == ']') {
            cur->cursor_ptr++;
            break;
        }

        sJsonFreeKeys(keys_ptr, key_count);
        sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
        return false;
    }

    *keys_out_ptr      = keys_ptr;
    *key_count_out_ptr = key_count;

    return true;
}

static bool
sJsonParseGroup(AliasMapJsonCursor *cur, char **value_out_ptr, char ***keys_out_ptr,
                size_t *key_count_out_ptr)
{
    char  *value_ptr  = NULL;
    char **keys_ptr   = NULL;
    size_t key_count  = 0;
    bool   have_value = false;
    bool   have_keys  = false;

    if (*cur->cursor_ptr != '{') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
        return false;
    }

    cur->cursor_ptr++;
    sJsonSkipWhitespace(cur);

    if (*cur->cursor_ptr == '}') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT); /* empty object */
        return false;
    }

    for (;;) {
        char *field_ptr = NULL;

        if (!sJsonParseString(cur, &field_ptr)) goto fail;

        bool is_value_field = strcmp(field_ptr, "value") == 0;
        bool is_keys_field  = strcmp(field_ptr, "keys") == 0;

        free(field_ptr);

        if (!is_value_field && !is_keys_field) {
            sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
            goto fail;
        }

        sJsonSkipWhitespace(cur);

        if (*cur->cursor_ptr != ':') {
            sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
            goto fail;
        }

        cur->cursor_ptr++;
        sJsonSkipWhitespace(cur);

        if (is_value_field) {
            if (have_value) {
                sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
                goto fail;
            }

            if (!sJsonParseString(cur, &value_ptr)) goto fail;

            have_value = true;
        }
        else {
            if (have_keys) {
                sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
                goto fail;
            }

            if (!sJsonParseKeys(cur, &keys_ptr, &key_count)) goto fail;

            have_keys = true;
        }

        sJsonSkipWhitespace(cur);

        if (*cur->cursor_ptr == ',') {
            cur->cursor_ptr++;
            sJsonSkipWhitespace(cur);
            continue;
        }

        if (*cur->cursor_ptr == '}') {
            cur->cursor_ptr++;
            break;
        }

        sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
        goto fail;
    }

    if (!have_value || !have_keys) {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
        goto fail;
    }

    *value_out_ptr     = value_ptr;
    *keys_out_ptr      = keys_ptr;
    *key_count_out_ptr = key_count;

    return true;

fail:
    free(value_ptr);
    sJsonFreeKeys(keys_ptr, key_count);

    return false;
}

static bool
sJsonParseDocument(AliasMapJsonCursor *cur, AliasMapJsonGroups *groups)
{
    sJsonSkipWhitespace(cur);

    if (*cur->cursor_ptr != '[') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_ARGUMENT);
        return false;
    }

    cur->cursor_ptr++;
    sJsonSkipWhitespace(cur);

    if (*cur->cursor_ptr == ']') {
        cur->cursor_ptr++;
        goto trailing;
    }

    for (;;) {
        char  *value_ptr = NULL;
        char **keys_ptr  = NULL;
        size_t key_count = 0;

        if (!sJsonParseGroup(cur, &value_ptr, &keys_ptr, &key_count)) return false;

        if (!sJsonGroupsAppend(groups, value_ptr, keys_ptr, key_count)) {
            free(value_ptr);
            sJsonFreeKeys(keys_ptr, key_count);
            sJsonError(cur, ALIASMAP_CREATE_NO_MEMORY);
            return false;
        }

        sJsonSkipWhitespace(cur);

        if (*cur->cursor_ptr == ',') {
            cur->cursor_ptr++;
            sJsonSkipWhitespace(cur);
            continue;
        }

        if (*cur->cursor_ptr == ']') {
            cur->cursor_ptr++;
            break;
        }

        sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
        return false;
    }

trailing:
    sJsonSkipWhitespace(cur);

    if (*cur->cursor_ptr != '\0') {
        sJsonError(cur, ALIASMAP_CREATE_INVALID_JSON);
        return false;
    }

    return true;
}

PUBLIC_API AliasMap *
AliasMapCreateWithJsonDescriptor(AliasMapJsonDescriptor *descriptor_ptr)
{
    if (!descriptor_ptr) return NULL;

    AliasMap log_ctx = {0};

    AliasMapLogBind(&log_ctx, descriptor_ptr->logger_ptr);

    sJsonResultFill(&descriptor_ptr->result, ALIASMAP_CREATE_OK, SIZE_MAX);

    if (!descriptor_ptr->json_ptr) {
        sJsonResultFill(&descriptor_ptr->result, ALIASMAP_CREATE_INVALID_ARGUMENT, 0);
        log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "alias map rejected: status=%s (no JSON document)",
                    AliasMapCreateStatusName(ALIASMAP_CREATE_INVALID_ARGUMENT));
        return NULL;
    }

    AliasMapJsonCursor cursor = {
        .cursor_ptr = descriptor_ptr->json_ptr,
        .start_ptr  = descriptor_ptr->json_ptr,
        .status     = ALIASMAP_CREATE_OK,
        .offset     = SIZE_MAX,
    };
    AliasMapJsonGroups groups = {0};

    if (!sJsonParseDocument(&cursor, &groups)) {
        sJsonGroupsFree(&groups);
        sJsonResultFill(&descriptor_ptr->result, cursor.status, cursor.offset);
        log_ctx.log(&log_ctx, UF_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "alias map rejected: status=%s json_offset=%zu",
                    AliasMapCreateStatusName(cursor.status), cursor.offset);
        return NULL;
    }

    AliasMapDescriptor document = {
        .collectors_ptr  = groups.collectors_ptr,
        .collector_count = groups.count,
        .logger_ptr      = descriptor_ptr->logger_ptr,
    };

    AliasMap *map_ptr = AliasMapCreateInternal(&document, ALIASMAP_ORIGIN_JSON);

    descriptor_ptr->result = document.result;

    sJsonGroupsFree(&groups);

    return map_ptr;
}

