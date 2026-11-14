/**
 * @file ufconfig_driver_tests.cpp
 * @brief The driver boundary: what crosses it, and what must not change in the
 *        crossing.
 *
 * Two properties carry the weight here.
 *
 * C1 — a document parsed directly and the same document flattened to pairs and
 * loaded back must produce the same digest.  That is the claim that makes the
 * pair-set a faithful representation rather than an approximation.
 *
 * C3 — the same holds when the pairs are presented in reverse order.  A driver
 * is entitled to return pairs in any order, so if order changed the digest, no
 * store could be trusted to round-trip a configuration.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/ufconfig/ufconfig.h>
}

#include "ufconfig_test_paths.h"
#include "ufconfig_test_schema.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string SamplePath(const char *leaf) {
    return std::string(UFCONFIG_TEST_SAMPLE_DIR) + "/" + leaf;
}

std::string ReadWholeFile(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool WriteWholeFile(const std::string &path, const std::string &bytes) {
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}

// A directory owned by one test scope, removed with its files when the scope
// ends, so an assertion failure part-way through leaves nothing behind.
class TempDir {
  public:
    TempDir() {
        char tmpl[] = "/tmp/ufconfig_persist_XXXXXX";
        char *d = mkdtemp(tmpl);
        if (d != nullptr) path_ = d;
    }
    ~TempDir() {
        if (path_.empty()) return;
        for (const std::string &leaf : files_) std::remove((path_ + "/" + leaf).c_str());
        rmdir(path_.c_str());
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    bool ok() const { return !path_.empty(); }
    const std::string &Directory() const { return path_; }
    std::string Path(const std::string &leaf) {
        files_.push_back(leaf);
        return path_ + "/" + leaf;
    }

  private:
    std::string path_;
    std::vector<std::string> files_;
};

// Bounds every file write this process makes, so a store that writes through
// its destination fails part-way rather than at open.  Restores both the limit
// and the disposition of SIGXFSZ, which a write over the bound raises.
class FileSizeLimit {
  public:
    explicit FileSizeLimit(rlim_t bytes) {
        struct sigaction sa = {};
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGXFSZ, &sa, &old_sig_) == 0) sig_installed_ = true;
        struct rlimit lim = {};
        if (getrlimit(RLIMIT_FSIZE, &old_lim_) == 0) {
            lim.rlim_cur = bytes;
            lim.rlim_max = old_lim_.rlim_max;
            if (setrlimit(RLIMIT_FSIZE, &lim) == 0) limit_set_ = true;
        }
    }
    ~FileSizeLimit() {
        if (limit_set_) setrlimit(RLIMIT_FSIZE, &old_lim_);
        if (sig_installed_) sigaction(SIGXFSZ, &old_sig_, nullptr);
    }
    FileSizeLimit(const FileSizeLimit &) = delete;
    FileSizeLimit &operator=(const FileSizeLimit &) = delete;

    bool ok() const { return limit_set_ && sig_installed_; }

  private:
    struct rlimit old_lim_ = {};
    struct sigaction old_sig_ = {};
    bool limit_set_ = false;
    bool sig_installed_ = false;
};

UfConfigLoadOptions Lenient() {
    UfConfigLoadOptions opt = {};
    opt.mode = UF_CONFIG_LOAD_LENIENT;
    return opt;
}

std::vector<unsigned char> DigestOf(UfConfig *h) {
    std::vector<unsigned char> d(32, 0);
    UfConfigDigest(h, d.data());
    return d;
}

UfConfigOpStatus CreateWithFileDestination(UfConfig **out, const char *dest) {
    UfConfigDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fields           = g_ufconfig_fields;
    descriptor.field_count      = (size_t)g_ufconfig_field_count;
    descriptor.lookup           = UfConfigLookupPath;
    descriptor.backend             = UF_CONFIG_BACKEND_FILE;
    descriptor.config_file_path = dest;
    return UfConfigCreate(out, &descriptor);
}

// Restores the working directory when the scope ends, so a test that persists
// to the cwd default cannot leak its chdir into the next one.
class CwdGuard {
  public:
    explicit CwdGuard(const std::string &to) {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)) != nullptr) old_ = buf;
        ok_ = chdir(to.c_str()) == 0;
    }
    ~CwdGuard() {
        if (!old_.empty()) chdir(old_.c_str());
    }
    CwdGuard(const CwdGuard &) = delete;
    CwdGuard &operator=(const CwdGuard &) = delete;

    bool ok() const { return ok_; }

  private:
    std::string old_;
    bool ok_ = false;
};

}  // namespace

// ── Registry ─────────────────────────────────────────────────────────────────

TEST(UfConfigDriver, BuiltInDriversAreRegistered) {
    // Always built.
    EXPECT_NE(UfConfigDriverFindName("mem"), nullptr);
    EXPECT_NE(UfConfigDriverFindName("file"), nullptr);

    // The Redis and SQL drivers each carry a dependency on a client library the
    // library does not vendor, so each is built only when its capability is on
    // and the consumer links the client itself.  Present or absent, the kind
    // must resolve to something consistent — asserting presence here would make
    // this test the thing that fails when the driver is correctly absent.
    const UfConfigDriver *redis_by_name = UfConfigDriverFindName("redis");
    const UfConfigDriver *redis_by_kind = UfConfigDriverFindKind(UF_CONFIG_BACKEND_REDIS);
    EXPECT_EQ(redis_by_name == nullptr, redis_by_kind == nullptr)
        << "the Redis driver resolves by name but not by kind, or the reverse";

    const UfConfigDriver *sql_by_name = UfConfigDriverFindName("sql");
    const UfConfigDriver *sql_by_kind = UfConfigDriverFindKind(UF_CONFIG_BACKEND_SQL);
    EXPECT_EQ(sql_by_name == nullptr, sql_by_kind == nullptr)
        << "the SQL driver resolves by name but not by kind, or the reverse";
}

TEST(UfConfigDriver, UnknownNameAndKindResolveToNothing) {
    EXPECT_EQ(UfConfigDriverFindName("no-such-driver"), nullptr);
    EXPECT_EQ(UfConfigDriverFindKind(static_cast<UfConfigBackendStore>(9999)), nullptr);
}

TEST(UfConfigDriver, RegistrationRefusesAVersionItCannotDrive) {
    const UfConfigDriver *real = UfConfigDriverFindName("mem");
    ASSERT_NE(real, nullptr);

    // A driver built against another ABI is refused with UNIMPLEMENTED, not
    // STORE_VERSION: the latter describes a store whose *content* format cannot
    // be read, which is a different failure from a mismatched struct layout.
    UfConfigDriver future = *real;
    future.caps.api_version = UF_CONFIG_DRIVER_API_VERSION + 1;
    future.caps.name = "future-version";
    EXPECT_EQ(UfConfigRegisterDriver(&future), UF_CONFIG_ERR_UNIMPLEMENTED);

    UfConfigDriver unnamed = *real;
    unnamed.caps.name = nullptr;
    EXPECT_EQ(UfConfigRegisterDriver(&unnamed), UF_CONFIG_ERR_INVALID_ARG);
}

// ── Emitting from an empty handle ────────────────────────────────────────────

TEST(UfConfigDriver, SerialisingAnEmptyHandleIsRefused) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);

    char *json = nullptr;
    EXPECT_EQ(UfConfigToJsonAlloc(h, &json), UF_CONFIG_ERR_NOFIELD);
    free(json);

    UfConfigDestroy(h);
}

// ── The mem driver ───────────────────────────────────────────────────────────

TEST(UfConfigDriver, MemDriverRefusesAReadBeforeAnythingIsWritten) {
    const UfConfigDriver *mem = UfConfigDriverFindName("mem");
    ASSERT_NE(mem, nullptr);

    void *ctx = nullptr;
    UfConfigBackendSpec spec = {};
    spec.backend_store = UF_CONFIG_BACKEND_MEM;
    ASSERT_EQ(mem->open(&ctx, &spec), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    EXPECT_EQ(mem->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_ERR_NOT_CONFIGURED);

    mem->close(ctx);
}

TEST(UfConfigDriver, MemDriverRoundTripsAndAdvancesItsChangeToken) {
    const UfConfigDriver *mem = UfConfigDriverFindName("mem");
    ASSERT_NE(mem, nullptr);

    void *ctx = nullptr;
    UfConfigBackendSpec spec = {};
    spec.backend_store = UF_CONFIG_BACKEND_MEM;
    ASSERT_EQ(mem->open(&ctx, &spec), UF_CONFIG_OK);

    UfConfigFieldPair input[3] = {
        {.path = "a.x", .vtype = UF_PAIR_INT, .encoded = "1"},
        {.path = "a.y", .vtype = UF_PAIR_INT, .encoded = "2"},
        {.path = "arr.0", .vtype = UF_PAIR_STRING, .encoded = "\"z\""},
    };
    UfConfigMeta written = {};
    written.fmt = 1;
    ASSERT_EQ(mem->write_all(ctx, input, 3, &written), UF_CONFIG_OK);

    UfConfigStamp stamp = {};
    ASSERT_EQ(mem->stat(ctx, &stamp), UF_CONFIG_OK);
    EXPECT_GT(stamp.token, 0u);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    ASSERT_EQ(mem->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_OK);
    EXPECT_EQ(n, 3u);
    free(pairs);

    mem->close(ctx);
}

// ── C1 and C3 ────────────────────────────────────────────────────────────────

TEST(UfConfigDriver, FlattenThenLoadReproducesTheDigest) {
    const std::string path = SamplePath("sample.config.lua");
    UfConfigLoadOptions opt = Lenient();

    UfConfig *direct = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&direct), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(direct, path.c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    ASSERT_EQ(UfConfigFlattenHandle(direct, &pairs, &n, nullptr), UF_CONFIG_OK);
    ASSERT_GT(n, 0u);

    UfConfig *rebuilt = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&rebuilt), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadPairs(rebuilt, pairs, n, nullptr, &opt, nullptr), UF_CONFIG_OK);

    EXPECT_EQ(DigestOf(direct), DigestOf(rebuilt))
        << "the pair-set is not a faithful representation of the parsed document";

    UfConfigFreePairs(pairs, n);
    UfConfigDestroy(direct);
    UfConfigDestroy(rebuilt);
}

TEST(UfConfigDriver, ReversingThePairOrderDoesNotChangeTheDigest) {
    const std::string path = SamplePath("sample.config.lua");
    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    ASSERT_EQ(UfConfigFlattenHandle(h, &pairs, &n, nullptr), UF_CONFIG_OK);
    ASSERT_GT(n, 0u);

    std::vector<UfConfigFieldPair> reversed(pairs, pairs + n);
    std::reverse(reversed.begin(), reversed.end());

    UfConfig *from_reversed = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&from_reversed), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadPairs(from_reversed, reversed.data(), reversed.size(), nullptr, &opt, nullptr),
              UF_CONFIG_OK);

    EXPECT_EQ(DigestOf(h), DigestOf(from_reversed))
        << "pair order leaked into the digest; no store could round-trip this";

    UfConfigFreePairs(pairs, n);
    UfConfigDestroy(h);
    UfConfigDestroy(from_reversed);
}

TEST(UfConfigDriver, PairsFromLuaAgreeWithFlatteningTheParsedHandle) {
    const std::string path = SamplePath("sample.config.lua");
    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigFieldPair *via_handle = nullptr;
    size_t via_handle_n = 0;
    ASSERT_EQ(UfConfigFlattenHandle(h, &via_handle, &via_handle_n, nullptr), UF_CONFIG_OK);

    // Reading the file back and flattening it without a handle must land on the
    // same pair count: the two paths parse the same text.
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    fclose(f);

    UfConfigFieldPair *via_text = nullptr;
    size_t via_text_n = 0;
    ASSERT_EQ(UfConfigPairsFromLua(text.data(), text.size(), &via_text, &via_text_n), UF_CONFIG_OK);

    // They are not equal, and should not be.  `PairsFromLua` parses without a
    // schema, so it returns what the document said.  `FlattenHandle` runs on a
    // handle the schema was applied to, so it also carries every default that
    // was materialised.  The relationship is containment, not equality: every
    // pair the document produced must appear in the schema-bound set, and the
    // extra ones must be defaults.
    EXPECT_GT(via_text_n, 0u);
    EXPECT_GE(via_handle_n, via_text_n)
        << "the schema-bound set is missing pairs the document itself produced";

    for (size_t i = 0; i < via_text_n; i++) {
        bool found = false;
        for (size_t j = 0; j < via_handle_n; j++) {
            if (strcmp(via_text[i].path, via_handle[j].path) == 0) { found = true; break; }
        }
        EXPECT_TRUE(found) << "PairsFromLua produced " << via_text[i].path
                           << ", which flattening the parsed handle does not";
    }

    UfConfigFreePairs(via_handle, via_handle_n);
    UfConfigFreePairs(via_text, via_text_n);
    UfConfigDestroy(h);
}

// The two Redis cases drive a real server, so neither is a CTest gate: they
// will not run without one to talk to.  Point them anywhere with
// UFCONFIG_REDIS_HOST and UFCONFIG_REDIS_PORT — the isolated container created
// by tests/ufconfig/redis_live/run.sh is the intended way to provide one, and
// it exists precisely so these cases never have to reach for whatever Redis
// happens to be running on the machine.
static bool RedisTargetFromEnv(std::string &host, unsigned &port) {
    const char *h = getenv("UFCONFIG_REDIS_HOST");
    if (h == nullptr || h[0] == '\0') return false;
    host = h;
    const char *p = getenv("UFCONFIG_REDIS_PORT");
    port = p ? (unsigned)atoi(p) : 6379u;
    return true;
}

// ── Redis and SQL drivers ────────────────────────────────────────────────────

TEST(UfConfigDriver, RedisDriverIsNamespacedAndRefusesBeforeAFirstWrite) {
    const UfConfigDriver *redis = UfConfigDriverFindName("redis");
    if (redis == nullptr) {
        GTEST_SKIP() << "built without the Redis capability (UFLIB_CAPABILITY_HIREDIS)";
    }
    std::string rhost;
    unsigned    rport = 0;
    if (!RedisTargetFromEnv(rhost, rport)) {
        GTEST_SKIP() << "set UFCONFIG_REDIS_HOST to run this against a live Redis";
    }

    void *ctx = nullptr;
    UfConfigBackendSpec spec = {};
    spec.backend_store = UF_CONFIG_BACKEND_REDIS;
    spec.u.redis.host = rhost.c_str();
    spec.u.redis.port = rport;
    spec.u.redis.ns = "alice";
    spec.u.redis.config_name = "ufsrvwebsock";
    ASSERT_EQ(redis->open(&ctx, &spec), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    EXPECT_EQ(redis->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_ERR_NOT_CONFIGURED);

    UfConfigFieldPair input[3] = {
        {.path = "a.x", .vtype = UF_PAIR_INT, .encoded = "1"},
        {.path = "a.y", .vtype = UF_PAIR_INT, .encoded = "2"},
        {.path = "arr.0", .vtype = UF_PAIR_STRING, .encoded = "\"z\""},
    };
    UfConfigMeta written = {};
    written.fmt = 1;
    ASSERT_EQ(redis->write_all(ctx, input, 3, &written), UF_CONFIG_OK);
    ASSERT_EQ(redis->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_OK);
    EXPECT_EQ(n, 3u);
    free(pairs);

    redis->close(ctx);
}

TEST(UfConfigDriver, TwoNamespacesDoNotSeeEachOther) {
    // The whole point of the namespace: two users holding a configuration of
    // the same name must not observe one another's fields.
    const UfConfigDriver *redis = UfConfigDriverFindName("redis");
    if (redis == nullptr) {
        GTEST_SKIP() << "built without the Redis capability (UFLIB_CAPABILITY_HIREDIS)";
    }
    std::string rhost;
    unsigned    rport = 0;
    if (!RedisTargetFromEnv(rhost, rport)) {
        GTEST_SKIP() << "set UFCONFIG_REDIS_HOST to run this against a live Redis";
    }

    UfConfigFieldPair alice_in[1] = {
        {.path = "only.alice", .vtype = UF_PAIR_INT, .encoded = "1"}};
    UfConfigMeta written = {};
    written.fmt = 1;

    void *alice = nullptr;
    UfConfigBackendSpec alice_spec = {};
    alice_spec.backend_store = UF_CONFIG_BACKEND_REDIS;
    alice_spec.u.redis.host = rhost.c_str();
    alice_spec.u.redis.port = rport;
    alice_spec.u.redis.ns = "alice";
    alice_spec.u.redis.config_name = "ufsrvwebsock";
    ASSERT_EQ(redis->open(&alice, &alice_spec), UF_CONFIG_OK);
    ASSERT_EQ(redis->write_all(alice, alice_in, 1, &written), UF_CONFIG_OK);

    void *bob = nullptr;
    UfConfigBackendSpec bob_spec = {};
    bob_spec.backend_store = UF_CONFIG_BACKEND_REDIS;
    bob_spec.u.redis.host = rhost.c_str();
    bob_spec.u.redis.port = rport;
    bob_spec.u.redis.ns = "bob";
    bob_spec.u.redis.config_name = "ufsrvwebsock";
    ASSERT_EQ(redis->open(&bob, &bob_spec), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    EXPECT_EQ(redis->read_all(bob, &pairs, &n, &meta), UF_CONFIG_ERR_NOT_CONFIGURED)
        << "bob observed a configuration alice wrote";

    redis->close(alice);
    redis->close(bob);
}

TEST(UfConfigDriver, SqlDriverReconstructsArraysRatherThanScalars) {
    const UfConfigDriver *sql = UfConfigDriverFindName("sql");
    if (sql == nullptr) {
        GTEST_SKIP() << "built without the SQL capability (UFLIB_CAPABILITY_SQL)";
    }

    // This case drives a real server — it opens a connection and writes a
    // field — so it is not a CTest gate and will not run without a store to
    // talk to.  tests/ufconfig/sql_live/ is where that arrangement is made
    // deliberately, for this driver and for a test-supplied one; this case
    // exists to reach the driver from inside the suite when a developer has a
    // server handy.  Point it anywhere with UFCONFIG_SQL_DSN.
    const char *dsn = getenv("UFCONFIG_SQL_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        GTEST_SKIP() << "set UFCONFIG_SQL_DSN to run this against a live MariaDB";
    }

    void *ctx = nullptr;
    UfConfigBackendSpec spec = {};
    spec.backend_store = UF_CONFIG_BACKEND_SQL;
    spec.u.sql.dsn = dsn;
    spec.u.sql.ns = "alice";
    spec.u.sql.config_name = "ufsrvwebsock";
    ASSERT_EQ(sql->open(&ctx, &spec), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    EXPECT_EQ(sql->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_ERR_NOT_CONFIGURED);

    UfConfigFieldPair one = {.path = "t.0", .vtype = UF_PAIR_STRING, .encoded = "\"x\""};
    UfConfigMeta written = {};
    written.fmt = 1;
    ASSERT_EQ(sql->write_all(ctx, &one, 1, &written), UF_CONFIG_OK);
    ASSERT_EQ(sql->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_OK);
    ASSERT_EQ(n, 1u);

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadPairs(h, pairs, n, &meta, &opt, nullptr), UF_CONFIG_OK);

    char *json = nullptr;
    if (UfConfigToJsonAlloc(h, &json) == UF_CONFIG_OK && json != nullptr) {
        EXPECT_NE(strchr(json, '['), nullptr)
            << "an index-named child must serialise as an array, not an object: " << json;
        free(json);
    }

    UfConfigDestroy(h);
    free(pairs);
    sql->close(ctx);
}

// ── The file driver ──────────────────────────────────────────────────────────

TEST(UfConfigDriver, FileDriverReadsTheDocumentItWasGiven) {
    const UfConfigDriver *file = UfConfigDriverFindName("file");
    ASSERT_NE(file, nullptr);

    const std::string path = SamplePath("sample.config.lua");
    void *ctx = nullptr;
    UfConfigBackendSpec spec = {};
    spec.backend_store = UF_CONFIG_BACKEND_FILE;
    spec.u.file.path = path.c_str();
    ASSERT_EQ(file->open(&ctx, &spec), UF_CONFIG_OK);

    UfConfigFieldPair *pairs = nullptr;
    size_t n = 0;
    UfConfigMeta meta = {};
    ASSERT_EQ(file->read_all(ctx, &pairs, &n, &meta), UF_CONFIG_OK);
    EXPECT_GT(n, 0u);
    UfConfigFreePairs(pairs, n);

    file->close(ctx);
}

TEST(UfConfigDriver, FileDriverWritesBackADocumentThatReloadsToTheSameConfiguration) {
    TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::string path = dir.Path("persist.config");
    const std::string seed = ReadWholeFile(SamplePath("sample.config.lua"));
    ASSERT_FALSE(seed.empty());
    ASSERT_TRUE(WriteWholeFile(path, seed));

    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigSetInt(h, "testharn.knob_a", 42), UF_CONFIG_OK);

    char *before = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(h, &before), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigPersist(h), UF_CONFIG_OK);

    UfConfig *back = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&back), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(back, path.c_str(), &opt, nullptr), UF_CONFIG_OK)
        << "the document the driver wrote cannot be loaded back:\n"
        << ReadWholeFile(path).substr(0, 200);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(back, "testharn.knob_a", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 42) << "a value the caller set did not survive write-back";

    char *after = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(back, &after), UF_CONFIG_OK);
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);
    EXPECT_STREQ(before, after) << "write-back lost or altered fields";

    free(before);
    free(after);
    UfConfigDestroy(h);
    UfConfigDestroy(back);
}

TEST(UfConfigDriver, FileDriverLeavesTheDocumentIntactWhenTheWriteCannotComplete) {
    TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::string path = dir.Path("persist.config");
    const std::string seed = ReadWholeFile(SamplePath("sample.config.lua"));
    ASSERT_FALSE(seed.empty());
    ASSERT_TRUE(WriteWholeFile(path, seed));

    UfConfigLoadOptions opt = Lenient();
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigSetInt(h, "testharn.knob_a", 42), UF_CONFIG_OK);

    {
        FileSizeLimit limit(64);
        ASSERT_TRUE(limit.ok()) << "cannot bound a file write in this environment";
        EXPECT_NE(UfConfigPersist(h), UF_CONFIG_OK)
            << "a write that could not be completed was reported as success";
    }

    EXPECT_EQ(ReadWholeFile(path), seed)
        << "a write that could not be completed destroyed the document it was replacing";

    UfConfigDestroy(h);
}

TEST(UfConfigDriver, FileDriverPreservesTheDocumentsMode) {
    TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::string path = dir.Path("persist.config");
    const std::string seed = ReadWholeFile(SamplePath("sample.config.lua"));
    ASSERT_FALSE(seed.empty());
    ASSERT_TRUE(WriteWholeFile(path, seed));
    ASSERT_EQ(chmod(path.c_str(), 0600), 0);

    UfConfigLoadOptions opt = Lenient();
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigPersist(h), UF_CONFIG_OK);

    struct stat sb = {};
    ASSERT_EQ(stat(path.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 07777, 0600u) << "the write changed the mode the operator had set";

    UfConfigDestroy(h);
}

TEST(UfConfigDriver, FilePersistWritesToTheDestinationTheDescriptorNames) {
    TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::string loaded = dir.Path("loaded.config");
    const std::string dest   = dir.Path("destination.config");
    const std::string seed   = ReadWholeFile(SamplePath("sample.config.lua"));
    ASSERT_FALSE(seed.empty());
    ASSERT_TRUE(WriteWholeFile(loaded, seed));

    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(CreateWithFileDestination(&h, dest.c_str()), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, loaded.c_str(), &opt, nullptr), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigSetInt(h, "testharn.knob_a", 42), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigPersist(h), UF_CONFIG_OK);

    EXPECT_TRUE(ReadWholeFile(loaded) == seed)
        << "persist rewrote the document it was loaded from instead of the destination";

    UfConfig *back = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&back), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(back, dest.c_str(), &opt, nullptr), UF_CONFIG_OK)
        << "the destination the descriptor named holds no usable document:\n"
        << ReadWholeFile(dest).substr(0, 200);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(back, "testharn.knob_a", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 42);

    UfConfigDestroy(h);
    UfConfigDestroy(back);
}

TEST(UfConfigDriver, FilePersistFallsBackToTheWorkingDirectoryWhenNoDestinationIsKnown) {
    TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::string dumped = dir.Path("ufconfig.dumped.config");
    const std::string seed   = ReadWholeFile(SamplePath("sample.config.lua"));
    ASSERT_FALSE(seed.empty());

    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadBuffer(h, seed.data(), seed.size(), &opt, nullptr), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigSetInt(h, "testharn.knob_a", 42), UF_CONFIG_OK);

    {
        CwdGuard cwd(dir.Directory());
        ASSERT_TRUE(cwd.ok()) << "cannot enter the temporary directory";
        ASSERT_EQ(UfConfigPersist(h), UF_CONFIG_OK)
            << "persist had no destination and did not fall back to the working directory";
    }

    EXPECT_FALSE(ReadWholeFile(dumped).empty()) << "the default document was not written";

    UfConfig *back = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&back), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(back, dumped.c_str(), &opt, nullptr), UF_CONFIG_OK);
    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(back, "testharn.knob_a", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 42);

    UfConfigDestroy(h);
    UfConfigDestroy(back);
}

// ── Serialisers and mutation ─────────────────────────────────────────────────

TEST(UfConfigDriver, EverySerialiserProducesSomethingForALoadedHandle) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    char *json = nullptr;
    char *yaml = nullptr;
    char *ini = nullptr;
    char *lua = nullptr;

    ASSERT_EQ(UfConfigToJsonAlloc(h, &json), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigToYamlAlloc(h, &yaml), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigToIniAlloc(h, &ini), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigToLuaStyleAlloc(h, &lua), UF_CONFIG_OK);

    EXPECT_NE(json, nullptr);
    EXPECT_NE(yaml, nullptr);
    EXPECT_NE(ini, nullptr);
    EXPECT_NE(lua, nullptr);
    if (json) EXPECT_NE(json[0], '\0');
    if (yaml) EXPECT_NE(yaml[0], '\0');
    if (ini) EXPECT_NE(ini[0], '\0');
    if (lua) EXPECT_NE(lua[0], '\0');

    free(json);
    free(yaml);
    free(ini);
    free(lua);
    UfConfigDestroy(h);
}

TEST(UfConfigDriver, ReloadingIdenticalBytesYieldsAnIdenticalDigest) {
    const std::string path = SamplePath("sample.config.lua");
    UfConfigLoadOptions opt = Lenient();

    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);
    const std::vector<unsigned char> first = DigestOf(h);

    char *json_before = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(h, &json_before), UF_CONFIG_OK);

    ASSERT_EQ(UfConfigLoadFile(h, path.c_str(), &opt, nullptr), UF_CONFIG_OK);
    EXPECT_EQ(first, DigestOf(h)) << "canonical form is not stable across an identical reload";

    char *json_after = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(h, &json_after), UF_CONFIG_OK);
    ASSERT_NE(json_before, nullptr);
    ASSERT_NE(json_after, nullptr);
    EXPECT_STREQ(json_before, json_after);

    free(json_before);
    free(json_after);
    UfConfigDestroy(h);
}

TEST(UfConfigDriver, MutableFieldsAcceptWritesAndImmutableOnesDoNot) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    // The schema marks these mutable.
    EXPECT_EQ(UfConfigSetInt(h, "testharn.knob_a", 3), UF_CONFIG_OK);
    EXPECT_EQ(UfConfigSetFloat(h, "testharn.knob_f", 2.5), UF_CONFIG_OK);
    EXPECT_EQ(UfConfigUnset(h, "testharn.knob_c"), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "testharn.knob_a", &v), UF_CONFIG_OK);
    EXPECT_EQ(v.as.i, 3);

    // The listener port is not marked mutable, so a write must be refused
    // rather than applied and forgotten.
    EXPECT_EQ(UfConfigSetInt(h, "ufsrv.main_listener_port", 1234), UF_CONFIG_ERR_IMMUTABLE);

    UfConfigDestroy(h);
}

TEST(UfConfigDriver, ARefusedSetLeavesTheValueInPlace) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    UfConfigValue before;
    ASSERT_EQ(UfConfigGetField(h, "testharn.knob_b", &before), UF_CONFIG_OK);
    ASSERT_EQ(before.value_type, UF_CONFIG_VALUE_TYPE_STRING);
    const std::string kept(before.as.str.ptr, before.as.str.len);

    EXPECT_EQ(UfConfigSetString(h, "testharn.knob_b", "not a valid value!"),
              UF_CONFIG_ERR_VALIDATION);

    UfConfigValue after;
    ASSERT_EQ(UfConfigGetField(h, "testharn.knob_b", &after), UF_CONFIG_OK);
    EXPECT_EQ(std::string(after.as.str.ptr, after.as.str.len), kept);

    UfConfigDestroy(h);
}

TEST(UfConfigDriver, SettingAFieldTheInstanceDoesNotHoldIsRefused) {
    UfConfig *h = nullptr;
    ASSERT_EQ(UfConfigTestCreate(&h), UF_CONFIG_OK);
    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, SamplePath("sample.config.lua").c_str(), &opt, nullptr),
              UF_CONFIG_OK);

    EXPECT_EQ(UfConfigSetInt(h, "no.such.path", 1), UF_CONFIG_ERR_NOFIELD);

    UfConfigDestroy(h);
}
