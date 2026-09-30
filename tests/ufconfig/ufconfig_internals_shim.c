/**
 * @file ufconfig_internals_shim.c
 * @brief The one translation unit that includes the module's private header.
 *
 * See ufconfig_internals_shim.h for why this exists.  Nothing here makes a
 * decision: each function calls one internal entry point and reports what it
 * returned.
 */

#include "ufconfig_internals_shim.h"

#include "ufconfig_priv.h"

#include <string.h>

int ufconfigShimTransformRoundTrip(const char *op, const char *in, size_t inlen) {
    UfArena *arena = NULL;
    char *mid = NULL;
    char *back = NULL;
    size_t mid_len = 0;
    size_t back_len = 0;
    int verdict;

    UfConfigStatus forward = ConfigTransformApplyNamed(&arena, op, in, inlen, &mid, &mid_len);
    if (forward != UF_CONFIG_OK) {
        verdict = 1;
    } else {
        UfConfigStatus reverse = ConfigTransformRevertNamed(&arena, op, mid, mid_len, &back, &back_len);
        if (reverse != UF_CONFIG_OK) {
            verdict = 1;
        } else if (back_len != inlen || memcmp(back, in, inlen) != 0) {
            verdict = 2;
        } else {
            verdict = 0;
        }
    }

    ConfigArenaFree(arena);
    return verdict;
}

int ufconfigShimDecodeRejected(const char *op, const char *in, size_t inlen) {
    UfArena *arena = NULL;
    char *out = NULL;
    size_t out_len = 0;

    UfConfigStatus st = ConfigTransformRevertNamed(&arena, op, in, inlen, &out, &out_len);
    ConfigArenaFree(arena);
    return st != UF_CONFIG_OK;
}

int ufconfigShimValid(const char *validator, const char *value) {
    UfConfigStatus st;

    if (strcmp(validator, "date") == 0) {
        st = ConfigValidateDate(value);
    } else if (strcmp(validator, "ip4") == 0) {
        st = ConfigValidateIp4(value);
    } else if (strcmp(validator, "ip6") == 0) {
        st = ConfigValidateIp6(value);
    } else if (strcmp(validator, "email") == 0) {
        st = ConfigValidateEmail(value);
    } else if (strcmp(validator, "url") == 0) {
        st = ConfigValidateUrl(value);
    } else if (strcmp(validator, "fqdn") == 0) {
        st = ConfigValidateFqdn(value);
    } else if (strcmp(validator, "filesize") == 0) {
        st = ConfigValidateFileSize(value);
    } else {
        return 0;
    }

    return st == UF_CONFIG_OK;
}
