/**
 * @file ufconfig_namespace_tests.cpp
 * @brief Walking the tree through the namespace interface.
 *
 * The namespace API hands ownership of each handle to the caller, so a walk is
 * also a test that nothing is leaked and nothing is freed twice.  The walk
 * below frees exactly what it is given and nothing else — running this suite
 * under LSan is what establishes that the ownership rule in the header is the
 * one the implementation follows.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/ufconfig/ufconfig.h>
}

#include "ufconfig_test_paths.h"
#include "ufconfig_test_schema.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string SamplePath(const char *leaf) {
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

// Depth-bounded so that a malformed tree cannot turn a test into a hang.
constexpr int kMaxWalkDepth = 16;

struct WalkStats {
    size_t visited = 0;
    size_t aliases = 0;
    // Resolution must produce its own handle.  Returning the same pointer would
    // mean the caller frees what it was already given, and the walk below is
    // exactly the pattern that would do it.
    bool resolve_returned_the_same_handle = false;
};

// Returns void so that ASSERT_* may be used inside it — those expand to a bare
// `return;` and so cannot appear in a function that yields a value.
void Walk(const UfConfigNamespace *ns, int depth, WalkStats *stats) {
    if (ns == nullptr || depth > kMaxWalkDepth) return;

    size_t entries = UfConfigNamespaceEntryCount(ns);

    for (size_t i = 0; i < entries; i++) {
        UfConfigNamespaceEntry e;
        memset(&e, 0, sizeof(e));
        if (UfConfigNamespaceEntryAt(ns, i, &e) != UF_CONFIG_OK) continue;
        stats->visited++;

        if (e.kind == UF_CONFIG_NS_TABLE_ALIAS) {
            stats->aliases++;
            UfConfigNamespace *resolved = nullptr;
            if (UfConfigNamespaceResolve(ns, &resolved) == UF_CONFIG_OK) {
                if (resolved == ns) {
                    stats->resolve_returned_the_same_handle = true;
                } else {
                    free(resolved);
                }
            }
        }

        if (e.table != nullptr) {
            Walk(e.table, depth + 1, stats);
            free(e.table);
        }
    }
}

}  // namespace

TEST(UfConfigNamespace, RootWalksAndReachesEveryBranch) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigNamespace *root = nullptr;
    ASSERT_EQ(UfConfigRootGet(h, &root), UF_CONFIG_OK);
    ASSERT_NE(root, nullptr);

    WalkStats stats;
    Walk(root, 0, &stats);
    EXPECT_GT(stats.visited, 0u);
    EXPECT_FALSE(stats.resolve_returned_the_same_handle)
        << "resolving an alias handed back the caller's own handle";
    free(root);

    UfConfigDestroy(h);
}

TEST(UfConfigNamespace, NamedLookupAgreesWithPositionalWalk) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigNamespace *root = nullptr;
    ASSERT_EQ(UfConfigRootGet(h, &root), UF_CONFIG_OK);

    // Every entry reachable by position must also be reachable by the name it
    // reports, and the two must describe the same thing.
    //
    // KNOWN DEFECT, and this assertion is what holds it visible.  On a document
    // whose root is an explicit `return { ufsrv = ufsrv, ... }` block, the root
    // namespace carries each aliased name TWICE: once as the alias the return
    // block binds, and once as the inline table the top-level binding declares.
    // EntryAt reports the inline one and EntryByName the alias, so a caller
    // walking the root sees the same name twice and cannot tell that the two
    // are one table.  Reported; needs a fix in the namespace builder, not in
    // this test.
    size_t entries = UfConfigNamespaceEntryCount(root);
    for (size_t i = 0; i < entries; i++) {
        UfConfigNamespaceEntry positional;
        memset(&positional, 0, sizeof(positional));
        if (UfConfigNamespaceEntryAt(root, i, &positional) != UF_CONFIG_OK) continue;
        ASSERT_NE(positional.name, nullptr);

        UfConfigNamespaceEntry named;
        memset(&named, 0, sizeof(named));
        SCOPED_TRACE(positional.name);
        ASSERT_EQ(UfConfigNamespaceEntryByName(root, positional.name, &named), UF_CONFIG_OK);
        EXPECT_EQ(named.kind, positional.kind);

        if (named.table != nullptr) free(named.table);
        if (positional.table != nullptr) free(positional.table);
    }

    free(root);
    UfConfigDestroy(h);
}

TEST(UfConfigNamespace, RootIsReachedThroughAnAliasAndSaysSo) {
    // The shipped sample ends with an explicit return block that binds the root
    // to tables declared above it.  A root entry reached that way is an alias,
    // and the caller has to be able to tell — mutating through it is not the
    // local edit it looks like.
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigNamespace *root = nullptr;
    ASSERT_EQ(UfConfigRootGet(h, &root), UF_CONFIG_OK);

    UfConfigNamespaceEntry e;
    memset(&e, 0, sizeof(e));
    ASSERT_EQ(UfConfigNamespaceEntryByName(root, "ufsrv", &e), UF_CONFIG_OK);
    EXPECT_EQ(e.kind, UF_CONFIG_NS_TABLE_ALIAS);
    if (e.table != nullptr) free(e.table);

    free(root);
    UfConfigDestroy(h);
}

TEST(UfConfigNamespace, ResolvingAPlainTableDescribesTheSameTable) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.strict.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigNamespace *ufsrv = nullptr;
    ASSERT_EQ(UfConfigNamespaceGet(h, "ufsrv", &ufsrv), UF_CONFIG_OK);

    // Resolving something that is already a table is not an error: the result
    // describes the same table, so a caller need not branch on the kind.
    UfConfigNamespace *resolved = nullptr;
    ASSERT_EQ(UfConfigNamespaceResolve(ufsrv, &resolved), UF_CONFIG_OK);
    ASSERT_NE(resolved, nullptr);
    ASSERT_NE(resolved, ufsrv) << "resolve must hand back its own handle";

    const char *via = UfConfigNamespacePath(resolved);
    const char *direct = UfConfigNamespacePath(ufsrv);
    if (via != nullptr && direct != nullptr) EXPECT_STREQ(via, direct);

    free(resolved);
    free(ufsrv);
    UfConfigDestroy(h);
}

TEST(UfConfigNamespace, DefiningPathIsEmptyForTheRoot) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    UfConfigLoadOptions opt = Strict();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.strict.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigNamespace *root = nullptr;
    ASSERT_EQ(UfConfigRootGet(h, &root), UF_CONFIG_OK);

    const char *path = UfConfigNamespacePath(root);
    EXPECT_TRUE(path == nullptr || path[0] == '\0');

    free(root);
    UfConfigDestroy(h);
}
