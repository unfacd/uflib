/**
 * @file ufconfig_internals_shim.h
 * @brief Plain-C access to the config module's internals, for the test suite.
 *
 * The module's internal entry points — the transform catalogue and the
 * validators — are declared in ufconfig_priv.h, which pulls in C17 <stdatomic.h>
 * and pthread.  That header does not survive a C++ translation unit, so it is
 * included by exactly one C file, ufconfig_internals_shim.c, and the tests see
 * only the declarations below.
 *
 * Every function here is a thin wrapper with no logic of its own.  A wrapper
 * that decided something would be testing the wrapper.
 *
 * Naming follows the C file's linkage rather than the module's public
 * convention: nothing here is part of any contract, and the names are scoped to
 * the test binary.
 */

#ifndef UFCONFIG_INTERNALS_SHIM_H
#define UFCONFIG_INTERNALS_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Applies a transform and then its inverse to @p in.
 *
 * @return 0 if both directions succeeded and the round trip is lossless.
 * @return 1 if either direction failed.
 * @return 2 if both succeeded but the result differs from the input.
 *
 * @note Lossy operations are expected to return 2 and the caller decides
 *       whether that is correct for the operation; this function does not know
 *       which transforms are lossy.
 */
int ufconfigShimTransformRoundTrip(const char *op, const char *in, size_t inlen);

/*!
 * @brief Decodes @p in with @p op's inverse.
 *
 * @return 1 if the decoder rejected it, 0 if it accepted it.
 *
 * @note Exists for the strict-decode cases: malformed input must be refused,
 *       and a decoder that returns success on garbage is the defect.
 */
int ufconfigShimDecodeRejected(const char *op, const char *in, size_t inlen);

/*!
 * @brief Runs a named validator.
 *
 * @return 1 if the value is accepted, 0 if it is rejected.
 */
int ufconfigShimValid(const char *validator, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* UFCONFIG_INTERNALS_SHIM_H */
