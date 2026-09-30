/**
 * @file ufconfig_core_tests.cpp
 * @brief Load, accessor and limit behaviour of the config module.
 *
 * The cases carried over from the module's original standalone suite, plus the
 * accessor-equivalence and depth-ceiling checks that were in it.  Each is
 * stated as the property it establishes rather than as a sequence of calls, so
 * that a failure names the property rather than a line number.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/ufconfig/ufconfig.h>
}

#include "ufconfig_test_paths.h"
#include "ufconfig_test_schema.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string CorpusPath(const std::string &leaf) {
    return std::string(UFCONFIG_TEST_FIXTURE_DIR) + "/" + leaf;
}

std::string SamplePath(const std::string &leaf) {
    return std::string(UFCONFIG_TEST_SAMPLE_DIR) + "/" + leaf;
}

UfConfigLoadOptions Lenient() {
    UfConfigLoadOptions opt = {};
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = UF_CONFIG_LOAD_LENIENT;
    return opt;
}

UfConfigLoadOptions Strict() {
    UfConfigLoadOptions opt = {};
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = UF_CONFIG_LOAD_STRICT;
    return opt;
}

}  // namespace

// ── Sample documents ─────────────────────────────────────────────────────────

TEST(UfConfigCore, LenientLoadOfShippedSampleSucceeds) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigDestroy(h);
}

TEST(UfConfigCore, SchemaTransformIsAppliedOnRead) {
    // server_run_mode is declared with transform=toupper, so the document's
    // "shadow" must read back as "SHADOW" — the transform is the schema's, not
    // the document's.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.server_run_mode", &v), UF_CONFIG_OK);
    ASSERT_TRUE(v.present);
    ASSERT_EQ(v.kind, UF_CONFIG_KIND_STRING);
    ASSERT_NE(v.as.str.ptr, nullptr);
    EXPECT_STREQ(v.as.str.ptr, "SHADOW");

    // The pre-transform value is still reachable, which is what makes a lossy
    // transform diagnosable rather than merely surprising.
    ASSERT_NE(v.raw.str.ptr, nullptr);
    EXPECT_EQ(std::string(v.raw.str.ptr, v.raw.str.len), "shadow");

    UfConfigDestroy(h);
}

TEST(UfConfigCore, UndeclaredFieldsAreReportedNotSwallowedInLenientMode) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    UfConfigLoadReport report = {};
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, &report),
              UF_CONFIG_OK);

    ASSERT_NE(report.notes, nullptr);
    size_t unknown = 0;
    for (size_t i = 0; i < report.note_count; i++) {
        if (report.notes[i].kind == UF_CONFIG_NOTE_UNKNOWN) unknown++;
    }
    EXPECT_EQ(unknown, 4u)
        << "the shipped sample carries exactly four fields the schema does not declare";
    for (size_t i = 0; i < report.note_count; i++) {
        EXPECT_NE(report.notes[i].path, nullptr);
    }

    UfConfigDestroy(h);
}

TEST(UfConfigCore, StrictModeRejectsTheSampleThatLenientModeCarries) {
    // The shipped sample declares a table the schema does not.  Strict mode
    // must refuse it; that refusal is the whole reason strict is the default.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    EXPECT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_UNKNOWN_FIELD);

    UfConfigDestroy(h);
}

TEST(UfConfigCore, StrictFixtureLoadsInStrictMode) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    EXPECT_EQ(UfConfigLoadFile(h, SamplePath("sample.strict.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigDestroy(h);
}

// ── The format is not a scripting language ───────────────────────────────────

TEST(UfConfigCore, DocumentRequiresValidationBeforeItIsTrusted) {
    // A document that parses is not therefore a document that validates.  These
    // two are both well-formed and both refused, by different stages.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();

    // Structurally fine, but a required field is absent.
    const char *missing_required = "ufsrv = { main_listener_port = 19701 }\n";
    EXPECT_EQ(UfConfigLoadBuffer(h, missing_required, strlen(missing_required), &opt, nullptr),
              UF_CONFIG_ERR_REQUIRED);

    // A value the field's validator refuses.
    const char *bad_value =
        "ufsrv = {\n"
        "  server_id = 0, server_run_mode = \"normal\", server_cpu_affinity = \"managed\",\n"
        "  intra_ufsrv_classname = \"ufsrv\", protocol_id = 0, ufsrv_geogroup = 1,\n"
        "  main_listener_port = 99999,\n"
        "}\n";
    UfConfigStatus st =
        UfConfigLoadBuffer(h, bad_value, strlen(bad_value), &opt, nullptr);
    EXPECT_TRUE(st == UF_CONFIG_ERR_VALIDATION || st == UF_CONFIG_ERR_REQUIRED)
        << "port 99999 must not be accepted; got " << UfConfigStatusString(st);

    UfConfigDestroy(h);
}

TEST(UfConfigCore, ArithmeticAndConcatenationAreNotEvaluated) {
    // The format mimics Lua's table syntax and has no evaluator.  An operator
    // is a parse error, never something computed.
    const char *files[] = {"arith.lua", "call.lua", "concat.lua", "and.lua",
                           "cmp.lua",   "len.lua",  "require.lua", "ostime.lua"};

    for (const char *leaf : files) {
        SCOPED_TRACE(leaf);
        UfConfig *h = nullptr;
        ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

        UfConfigLoadOptions opt = Lenient();
        EXPECT_EQ(UfConfigLoadFile(h, CorpusPath(std::string("corpus_noeval/") + leaf).c_str(),
                                   &opt, nullptr),
                  UF_CONFIG_ERR_PARSE);

        UfConfigDestroy(h);
    }
}

TEST(UfConfigCore, CircularAliasIsDetectedRatherThanRecursedInto) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, CorpusPath("corpus_alias/cycle.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_CYCLE);

    UfConfigDestroy(h);
}

// ── Accessor equivalence ─────────────────────────────────────────────────────

TEST(UfConfigCore, PathIndexAndGeneratedAccessorAgree) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.strict.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigValue by_path = {};
    UfConfigValue by_index = {};
    UfConfigValue by_getter = {};

    ASSERT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &by_path), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigGetFieldByIndex(h, UF_CONFIG_FIELD_UFSRV_MAIN_LISTENER_PORT, &by_index),
              UF_CONFIG_OK);
    ASSERT_EQ(UfConfigGetUfsrvMainListenerPort(h, &by_getter), UF_CONFIG_OK);

    EXPECT_EQ(by_path.as.i, by_index.as.i);
    EXPECT_EQ(by_path.as.i, by_getter.as.i);

    UfConfigDestroy(h);
}

// ── Limits ───────────────────────────────────────────────────────────────────

TEST(UfConfigCore, NestingBeyondTheCeilingIsRefusedNotTruncated) {
    // Truncation would be worse than refusal: a truncated tree is a valid tree
    // that silently means something else.
    std::string deep = "t = ";
    for (int i = 0; i < 40; i++) deep += "{ a = ";
    for (int i = 0; i < 40; i++) deep += "}";

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    UfConfigStatus st =
        UfConfigLoadBuffer(h, deep.c_str(), deep.size(), &opt, nullptr);
    EXPECT_TRUE(st == UF_CONFIG_ERR_LIMIT_EXCEEDED || st == UF_CONFIG_ERR_PARSE)
        << "got " << UfConfigStatusString(st);

    UfConfigDestroy(h);
}

TEST(UfConfigCore, PathsBeyondThePublicMaximumAreRefused) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    std::string long_path(CONFIG_DEFAULT_UFCONFIG_PATH_MAX + 8, 'a');
    UfConfigValue v;
    EXPECT_EQ(UfConfigGetField(h, long_path.c_str(), &v), UF_CONFIG_ERR_LIMIT_EXCEEDED);

    UfConfigDestroy(h);
}
