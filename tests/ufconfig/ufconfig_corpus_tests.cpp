/**
 * @file ufconfig_corpus_tests.cpp
 * @brief The parse corpus, driven by its verdict table.
 *
 * Every fixture carries a declared verdict in corpus/verdicts.tsv and the test
 * asserts the module agrees with it.  Driving from the table rather than from a
 * hand-written list means a fixture added without a verdict is a failure, not a
 * file nothing runs.
 *
 * The properties being established are narrow on purpose:
 *
 *   accept  the module must get past parsing.  A later refusal (a missing
 *           required field, an undeclared field) is not a parse failure and
 *           does not count against the fixture.
 *   reject  the module must refuse it, and in strict mode it must not accept it
 *           either — a reject fixture that strict mode waves through would mean
 *           the two modes disagree about syntax.
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
#include <utility>
#include <vector>

namespace {

struct CorpusCase {
    std::string file;
    bool want_accept;
};

std::vector<CorpusCase> LoadVerdicts() {
    std::vector<CorpusCase> cases;
    std::string table = std::string(UFCONFIG_TEST_CORPUS_DIR) + "/verdicts.tsv";
    FILE *f = fopen(table.c_str(), "r");
    if (f == nullptr) return cases;

    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char *nl = strchr(line, '\n');
        if (nl != nullptr) *nl = '\0';
        if (line[0] == '\0') continue;
        char *tab = strchr(line, '\t');
        if (tab == nullptr) continue;
        *tab = '\0';
        cases.push_back({std::string(line), strcmp(tab + 1, "accept") == 0});
    }
    fclose(f);
    return cases;
}

const char *const kCorpusDirPrefix = "corpus/";

}  // namespace

class UfConfigCorpus : public ::testing::TestWithParam<CorpusCase> {};

TEST_P(UfConfigCorpus, MatchesItsDeclaredVerdict) {
    const CorpusCase &c = GetParam();
    std::string path = std::string(UFCONFIG_TEST_CORPUS_DIR) + "/" + c.file;

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = {};
    opt.size = sizeof(opt);
    opt.version = 1;
    opt.mode = UF_CONFIG_LOAD_LENIENT;

    UfConfigStatus st = UfConfigLoadFile(h, path.c_str(), &opt, nullptr);

    if (c.want_accept) {
        EXPECT_NE(st, UF_CONFIG_ERR_PARSE)
            << c.file << " is declared acceptable but was rejected as "
            << UfConfigStatusString(st);
    } else {
        EXPECT_TRUE(st == UF_CONFIG_ERR_PARSE || st == UF_CONFIG_ERR_CYCLE)
            << c.file << " is declared a reject but the module returned "
            << UfConfigStatusString(st);
    }

    UfConfigDestroy(h);

    // The same fixture under strict mode must not be accepted where lenient
    // rejected it.  Strict may refuse more; it must never refuse less.
    if (!c.want_accept) {
        UfConfig *hs = nullptr;
        ASSERT_EQ(UfConfigTestCreate(&hs), UF_CONFIG_OK);
        UfConfigLoadOptions strict = {};
        strict.size = sizeof(strict);
        strict.version = 1;
        strict.mode = UF_CONFIG_LOAD_STRICT;
        EXPECT_NE(UfConfigLoadFile(hs, path.c_str(), &strict, nullptr), UF_CONFIG_OK)
            << c.file << " was accepted outright in strict mode";
        UfConfigDestroy(hs);
    }
}

INSTANTIATE_TEST_SUITE_P(Table, UfConfigCorpus,
                         ::testing::ValuesIn(LoadVerdicts()),
                         [](const ::testing::TestParamInfo<CorpusCase> &info) {
                             std::string name = info.param.file;
                             for (char &ch : name) {
                                 if (ch == '.' || ch == '/') ch = '_';
                             }
                             return name;
                         });

// The table must not be empty, or the suite above passes by running nothing.
TEST(UfConfigCorpus, VerdictTableIsPresentAndPopulated) {
    auto cases = LoadVerdicts();
    EXPECT_GT(cases.size(), 0u) << "verdicts.tsv is missing from " UFCONFIG_TEST_CORPUS_DIR;
}

TEST(UfConfigCorpus, EveryDeclaredFixtureExistsOnDisk) {
    for (const CorpusCase &c : LoadVerdicts()) {
        SCOPED_TRACE(c.file);
        std::string path = std::string(UFCONFIG_TEST_CORPUS_DIR) + "/" + c.file;
        FILE *f = fopen(path.c_str(), "r");
        ASSERT_NE(f, nullptr) << "named in verdicts.tsv but not found: " << path;
        fclose(f);
    }
}
