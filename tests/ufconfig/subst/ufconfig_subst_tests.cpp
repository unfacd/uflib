#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <uflib/ufconfig/ufconfig.h>

extern "C" {
extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int UfConfigLookupPath(const char *path);
}

namespace {

struct Substitutions {
    const char *names[16];
    const char *values[16];
    int         count      = 0;
    int         calls      = 0;
    std::string last_name;  ///< the name the callback was asked for last
};

const char *
Lookup(void *ctx, const char *name)
{
    Substitutions *s = static_cast<Substitutions *>(ctx);

    s->calls++;
    s->last_name = name;
    for (int i = 0; i < s->count; i++)
        if (std::strcmp(s->names[i], name) == 0) return s->values[i];
    return nullptr;
}

class Subst : public ::testing::Test {
  protected:
    static UfConfig *Create()
    {
        UfConfigDescriptor descriptor;
        UfConfig *          handle = nullptr;

        std::memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fields      = g_ufconfig_fields;
        descriptor.field_count = (size_t)g_ufconfig_field_count;
        descriptor.lookup      = UfConfigLookupPath;
        descriptor.backend     = UF_CONFIG_BACKEND_FILE;
        EXPECT_EQ(UfConfigCreate(&handle, &descriptor), UF_CONFIG_OK);
        return handle;
    }

    static UfConfigLoadOptions Options(Substitutions *s, bool recursive)
    {
        UfConfigLoadOptions opt = {};

        opt.mode            = UF_CONFIG_LOAD_STRICT;
        opt.subst_get       = Lookup;
        opt.subst_ctx       = s;
        opt.subst_recursive = recursive;
        return opt;
    }

    static UfConfigOpStatus Load(UfConfig *h, const char *document,
                                 UfConfigLoadOptions *opt)
    {
        return UfConfigLoadBuffer(h, document, std::strlen(document), opt, nullptr);
    }

    static std::string Effective(UfConfig *h, const char *path)
    {
        UfConfigValue value;

        std::memset(&value, 0, sizeof(value));
        if (UfConfigGetField(h, path, &value) != UF_CONFIG_OK ||
            value.value_type != UF_CONFIG_VALUE_TYPE_STRING || !value.as.str.ptr)
            return "<absent>";
        return std::string(value.as.str.ptr, value.as.str.len);
    }

    static std::string Raw(UfConfig *h, const char *path)
    {
        UfConfigValue value;

        std::memset(&value, 0, sizeof(value));
        if (UfConfigGetField(h, path, &value) != UF_CONFIG_OK ||
            value.value_type != UF_CONFIG_VALUE_TYPE_STRING || !value.raw.str.ptr)
            return "<absent>";
        return std::string(value.raw.str.ptr, value.raw.str.len);
    }
};

}  // namespace

TEST_F(Subst, ResolvesAndKeepsTheDocumentTextInRaw)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "who";
    s.values[0] = "bob";

    UfConfig *           h     = Create();
    UfConfigLoadOptions  opt   = Options(&s, false);
    const char *         doc   = R"(return { sub = { plain = "v=$$who" } })";

    ASSERT_EQ(Load(h, doc, &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.plain"), "v=bob");
    EXPECT_EQ(Raw(h, "sub.plain"), "v=$$who");

    char *json = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(h, &json), UF_CONFIG_OK);
    EXPECT_NE(std::strstr(json, "$$who"), nullptr);
    EXPECT_EQ(std::strstr(json, "v=bob"), nullptr);
    std::free(json);

    UfConfigDestroy(h);
}

TEST_F(Subst, BracedNameIsTheSameName)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "who";
    s.values[0] = "bob";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { braced = "$${who}" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.braced"), "bob");
    UfConfigDestroy(h);
}

TEST_F(Subst, BackslashEscapesTheReference)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "who";
    s.values[0] = "bob";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { escaped = "\\$$who" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.escaped"), "$$who");
    EXPECT_EQ(s.calls, 0);
    UfConfigDestroy(h);
}

TEST_F(Subst, UnknownNameRefusesTheLoad)
{
    Substitutions s;

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    EXPECT_EQ(Load(h, R"(return { sub = { plain = "$$nobody" } })", &opt),
              UF_CONFIG_ERR_SUBST_UNRESOLVED);
    EXPECT_EQ(Effective(h, "sub.plain"), "<absent>");
    UfConfigDestroy(h);
}

TEST_F(Subst, NoCallbackRefusesTheLoad)
{
    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = {};

    opt.mode = UF_CONFIG_LOAD_STRICT;
    EXPECT_EQ(Load(h, R"(return { sub = { plain = "$$who" } })", &opt),
              UF_CONFIG_ERR_SUBST_UNRESOLVED);
    UfConfigDestroy(h);
}

TEST_F(Subst, DeclaredOrderDecidesWhetherSubstitutionSeesTheCase)
{
    Substitutions s;
    s.count     = 2;
    s.names[0]  = "who";
    s.values[0] = "bob";
    s.names[1]  = "WHO";
    s.values[1] = "BOB";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { subfirst = "$$who", firstsub = "$$who" } })", &opt),
              UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.subfirst"), "BOB");
    EXPECT_EQ(Effective(h, "sub.firstsub"), "BOB");
    EXPECT_EQ(s.last_name, "WHO");  /* toupper ran before the substitution */
    UfConfigDestroy(h);
}

TEST_F(Subst, AFieldWithoutVarsubKeepsTheTextLiteral)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "who";
    s.values[0] = "bob";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { plainlit = "$$who" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.plainlit"), "$$who");
    EXPECT_EQ(s.calls, 0);
    UfConfigDestroy(h);
}

TEST_F(Subst, ValidationSeesTheResolvedValue)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "choice";
    s.values[0] = "A";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { fixed = "$$choice" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.fixed"), "A");

    s.values[0] = "C";
    EXPECT_EQ(Load(h, R"(return { sub = { fixed = "$$choice" } })", &opt), UF_CONFIG_ERR_VALIDATION);
    UfConfigDestroy(h);
}

TEST_F(Subst, RecursiveResolvesTheValueItJustProduced)
{
    Substitutions s;
    s.count     = 2;
    s.names[0]  = "a";
    s.values[0] = "$$b";
    s.names[1]  = "b";
    s.values[1] = "done";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, true);

    ASSERT_EQ(Load(h, R"(return { sub = { nested = "$$a" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.nested"), "done");
    UfConfigDestroy(h);
}

TEST_F(Subst, NonRecursiveLeavesTheInnerReferenceAlone)
{
    Substitutions s;
    s.count     = 2;
    s.names[0]  = "a";
    s.values[0] = "$$b";
    s.names[1]  = "b";
    s.values[1] = "done";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, false);

    ASSERT_EQ(Load(h, R"(return { sub = { nested = "$$a" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.nested"), "$$b");
    UfConfigDestroy(h);
}

TEST_F(Subst, ACycleStopsInsteadOfSpinning)
{
    Substitutions s;
    s.count     = 1;
    s.names[0]  = "a";
    s.values[0] = "$$a";

    UfConfig *          h   = Create();
    UfConfigLoadOptions opt = Options(&s, true);

    ASSERT_EQ(Load(h, R"(return { sub = { cycle = "$$a" } })", &opt), UF_CONFIG_OK);
    EXPECT_EQ(Effective(h, "sub.cycle"), "$$a");
    EXPECT_LT(s.calls, 64);
    UfConfigDestroy(h);
}

TEST_F(Subst, TheDigestDoesNotDependOnWhatWasSubstituted)
{
    Substitutions bob;
    bob.count     = 1;
    bob.names[0]  = "who";
    bob.values[0] = "bob";

    Substitutions alice = bob;
    alice.values[0]     = "alice";

    const char *document = R"(return { sub = { plain = "v=$$who" } })";

    UfConfig *          first  = Create();
    UfConfig *          second = Create();
    UfConfigLoadOptions opt_a  = Options(&bob, false);
    UfConfigLoadOptions opt_b  = Options(&alice, false);

    ASSERT_EQ(Load(first, document, &opt_a), UF_CONFIG_OK);
    ASSERT_EQ(Load(second, document, &opt_b), UF_CONFIG_OK);
    EXPECT_EQ(Effective(first, "sub.plain"), "v=bob");
    EXPECT_EQ(Effective(second, "sub.plain"), "v=alice");

    unsigned char da[32] = {0}, db[32] = {0};

    ASSERT_EQ(UfConfigDigest(first, da), UF_CONFIG_OK);
    ASSERT_EQ(UfConfigDigest(second, db), UF_CONFIG_OK);
    EXPECT_EQ(std::memcmp(da, db, sizeof(da)), 0);

    UfConfigDestroy(first);
    UfConfigDestroy(second);
}
