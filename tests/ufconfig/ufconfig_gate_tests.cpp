/**
 * @file ufconfig_gate_tests.cpp
 * @brief Behaviour at the boundaries a failed operation leaves behind.
 *
 * These are the cases where the interesting question is not what a successful
 * call returns but what state a failed one leaves the handle in.  A
 * configuration loader that loses a working configuration because a reload hit
 * a syntax error is worse than one that refuses the reload.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/ufconfig/ufconfig.h>
}

#include "ufconfig_test_paths.h"
#include "ufconfig_test_schema.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string SamplePath(const char *leaf) {
    return std::string(UFCONFIG_TEST_SAMPLE_DIR) + "/" + leaf;
}

UfConfigLoadOptions Lenient() {
    UfConfigLoadOptions opt = {};
    opt.mode = UF_CONFIG_LOAD_LENIENT;
    return opt;
}

UfConfigLoadOptions Strict() {
    UfConfigLoadOptions opt = {};
    opt.mode = UF_CONFIG_LOAD_STRICT;
    return opt;
}

// A temporary file removed when the test scope ends, so an assertion failure
// part-way through does not leave the fixture behind.
class TempFile {
  public:
    explicit TempFile(const std::string &contents) {
        char tmpl[] = "/tmp/ufconfig_gate_XXXXXX";
        int fd = mkstemp(tmpl);
        EXPECT_NE(fd, -1);
        if (fd == -1) return;
        path_ = tmpl;
        FILE *f = fdopen(fd, "w");
        if (f != nullptr) {
            fwrite(contents.data(), 1, contents.size(), f);
            fclose(f);
        }
    }
    ~TempFile() {
        if (!path_.empty()) unlink(path_.c_str());
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    const std::string &path() const { return path_; }

    void Replace(const std::string &contents) {
        FILE *f = fopen(path_.c_str(), "w");
        if (f != nullptr) {
            fwrite(contents.data(), 1, contents.size(), f);
            fclose(f);
        }
    }

  private:
    std::string path_;
};

// The mtime-based staleness check has one-second resolution, so a rewrite must
// be separated from the load that preceded it.
void CrossSecondBoundary() { sleep(1); }

std::string ReadFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Every field the test schema requires, so a pair-set built from these loads
// without tripping the rule each of these tests is about, plus one ordinary
// mutable field to read back afterwards.  The paths are borrowed literals: a
// pair-set assembled this way must not be handed to UfConfigFreePairs.
std::vector<UfConfigFieldPair> RequiredPairs() {
    return {
        {"ufsrv.ufsrv_geogroup", UF_PAIR_INT, "1"},
        {"ufsrv.main_listener_port", UF_PAIR_INT, "19701"},
        {"ufsrv.sys_runtime.chroot", UF_PAIR_STRING, "\"/\""},
        {"ufnet.db_backend.port", UF_PAIR_INT, "19800"},
        {"ufnet.db_backend.address", UF_PAIR_STRING, "\"db.example\""},
        {"ufnet.db_backend.username", UF_PAIR_STRING, "\"ufsrv\""},
        {"ufnet.db_backend.password", UF_PAIR_STRING, "\"secret\""},
        {"testharn.knob_a", UF_PAIR_INT, "1"},
    };
}

}  // namespace

TEST(UfConfigGate, FailedLoadPublishesNothing) {
    // A refused load must not leave a partial tree behind.  A caller that reads
    // after a failure has to see absence, not half a configuration.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_UNKNOWN_FIELD);

    UfConfigValue v;
    memset(&v, 0, sizeof(v));
    EXPECT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &v), UF_CONFIG_ERR_NOFIELD);
    EXPECT_FALSE(v.present);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, FailedPairsLoadPublishesNothing) {
    // The pair-set path carries a store's contents, so a refused load through
    // it must leave the working configuration in force exactly as a refused
    // document load does.  A store that hands over an incomplete configuration
    // must fail, not quietly replace a working one with half a tree.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    std::vector<UfConfigFieldPair> good = RequiredPairs();
    ASSERT_EQ(UfConfigLoadPairs(h, good.data(), good.size(), nullptr, &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &v), UF_CONFIG_OK);
    ASSERT_EQ(v.as.i, 19701);

    // The same set with one required field removed.
    std::vector<UfConfigFieldPair> incomplete;
    for (const UfConfigFieldPair &p : good) {
        if (strcmp(p.path, "ufsrv.main_listener_port") == 0) continue;
        incomplete.push_back(p);
    }
    EXPECT_EQ(UfConfigLoadPairs(h, incomplete.data(), incomplete.size(), nullptr, &opt, nullptr),
              UF_CONFIG_ERR_REQUIRED);

    memset(&v, 0, sizeof(v));
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &v), UF_CONFIG_OK)
        << "the refused load replaced the working configuration";
    EXPECT_EQ(v.as.i, 19701);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, StrictPairsLoadRefusesAnUndeclaredPathAndPublishesNothing) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    std::vector<UfConfigFieldPair> good = RequiredPairs();
    ASSERT_EQ(UfConfigLoadPairs(h, good.data(), good.size(), nullptr, &opt, nullptr), UF_CONFIG_OK);

    std::vector<UfConfigFieldPair> with_unknown = good;
    with_unknown.push_back({"no.such.path", UF_PAIR_INT, "7"});
    EXPECT_EQ(UfConfigLoadPairs(h, with_unknown.data(), with_unknown.size(), nullptr, &opt, nullptr),
              UF_CONFIG_ERR_UNKNOWN_FIELD);

    UfConfigValue v;
    memset(&v, 0, sizeof(v));
    ASSERT_EQ(UfConfigGetField(h, "testharn.knob_a", &v), UF_CONFIG_OK)
        << "the refused load replaced the working configuration";
    EXPECT_EQ(v.as.i, 1);

    memset(&v, 0, sizeof(v));
    EXPECT_EQ(UfConfigGetField(h, "no.such.path", &v), UF_CONFIG_ERR_NOFIELD);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, LenientPairsLoadStillCarriesUndeclaredPaths) {
    // Leniency is a mode, not a property of the path a configuration arrives
    // by: an undeclared path a document may carry, a pair-set may carry too.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    std::vector<UfConfigFieldPair> pairs = RequiredPairs();
    pairs.push_back({"no.such.path", UF_PAIR_INT, "7"});
    ASSERT_EQ(UfConfigLoadPairs(h, pairs.data(), pairs.size(), nullptr, &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "no.such.path", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 7);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, FileLoadRefusesADocumentMissingARequiredField) {
    // The same rule at the document level, so a change to the path a file is
    // read by cannot weaken it unnoticed.
    TempFile doc("ufsrv = { main_listener_port = 19701 }\nreturn { ufsrv = ufsrv }\n");

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Strict();
    EXPECT_EQ(UfConfigLoadFile(h, doc.path().c_str(), &opt, nullptr), UF_CONFIG_ERR_REQUIRED);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, FailedLoadStillLeavesTheSourceRecorded) {
    // A handle that failed its first load must still know where it was asked to
    // load from, so that a later reload is answerable rather than an argument
    // error.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_UNKNOWN_FIELD);

    UfConfigReloadReport report = {};
    EXPECT_NE(UfConfigReload(h, &report), UF_CONFIG_ERR_INVALID_ARG);

    UfConfigDestroy(h);
}

TEST(UfConfigGate, BadReloadKeepsTheLastGoodConfiguration) {
    std::string good = ReadFile(SamplePath("sample.strict.lua"));
    ASSERT_FALSE(good.empty()) << "the strict fixture must be readable to seed this test";

    TempFile file(good);
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, file.path().c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue before;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &before), UF_CONFIG_OK);
    const int64_t seeded = before.as.i;

    file.Replace("this is { not = valid + syntax\n");
    CrossSecondBoundary();

    UfConfigReloadReport report = {};
    EXPECT_NE(UfConfigReload(h, &report), UF_CONFIG_OK);

    // The last good configuration must still be answering.
    UfConfigValue after;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.main_listener_port", &after), UF_CONFIG_OK)
        << "a failed reload discarded a working configuration";
    EXPECT_EQ(after.as.i, seeded);

    UfConfigDestroy(h);
}

// Both tests below start from the complete strict fixture and take one line out
// of it, rather than from a hand-written partial document.
//
// A partial document fails validation for the fields it omits and never reaches
// the behaviour under test — which is exactly how the original suite's version
// of this check passed while asserting nothing: it guarded the assertion with
// the load's status, and the load was quietly failing.
//
// server_id is the field under test.  It is defaulted and not required, so
// removing it leaves a document that still loads.

namespace {

const char *const kServerIdLine = "    server_id               = 0,\n";

}  // namespace

TEST(UfConfigGate, DefaultsAreMaterialisedAndReadable) {
    std::string body = ReadFile(SamplePath("sample.strict.lua"));
    ASSERT_FALSE(body.empty()) << "the strict fixture must be readable";

    size_t at = body.find(kServerIdLine);
    ASSERT_NE(at, std::string::npos)
        << "the strict fixture no longer contains the line this test removes; update the test";
    body.erase(at, strlen(kServerIdLine));

    TempFile file(body);
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, file.path().c_str(), &opt, nullptr), UF_CONFIG_OK)
        << "omitting a defaulted, non-required field must not break the load";

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.server_id", &v), UF_CONFIG_OK)
        << "a field with a schema default was absent because the document omitted it";
    EXPECT_TRUE(v.present) << "schema default was not materialised";

    UfConfigDestroy(h);
}

TEST(UfConfigGate, AValueTheDocumentStatesOverridesItsDefault) {
    std::string body = ReadFile(SamplePath("sample.strict.lua"));
    ASSERT_FALSE(body.empty());

    // The fixture states 0 for server_id, which is also its default.  Substitute
    // a value no default could produce, so a pass cannot come from the default.
    size_t at = body.find(kServerIdLine);
    ASSERT_NE(at, std::string::npos) << "the fixture no longer contains the line to substitute";
    body.replace(at, strlen(kServerIdLine), "    server_id               = 4321,\n");

    TempFile file(body);
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, file.path().c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "ufsrv.server_id", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 4321) << "the document's value did not win over the schema default";

    UfConfigDestroy(h);
}
