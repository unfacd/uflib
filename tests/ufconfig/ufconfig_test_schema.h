/**
 * @file ufconfig_test_schema.h
 * @brief Binds the test suite's schema, so a test creates a handle the way a
 *        server does rather than the way the module used to.
 *
 * The library holds no schema, so a test that wants a handle has to supply one.
 * These tests run against the schema the module ships as a sample — the same
 * one the example application and the stress tool use — because what they are
 * exercising is the module, not a schema.
 *
 * The generated artefacts are produced by this directory's CMakeLists into
 * @c generated/, and the target that includes this header links them.
 */

#ifndef UFCONFIG_TEST_SCHEMA_H
#define UFCONFIG_TEST_SCHEMA_H

#include <uflib/ufconfig/ufconfig.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);

#ifdef __cplusplus
}
#endif

/*! Create a handle bound to the test schema. */
static inline UfConfigStatus UfConfigTestCreate(UfConfig **out) {
    UfConfigDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fields      = g_ufconfig_fields;
    descriptor.field_count = (size_t)g_ufconfig_field_count;
    descriptor.lookup      = UfConfigLookupPath;
    descriptor.kind        = UF_CONFIG_BACKEND_FILE;
    return UfConfigCreate(out, &descriptor);
}

#endif /* UFCONFIG_TEST_SCHEMA_H */
