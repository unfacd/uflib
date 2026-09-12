/**
 * @file utils_collection_str_tests.cpp
 * @brief gtest coverage for the string-specialised CollectionDescriptor helpers
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/utils_collection_str.h>
}

/* Mutable char arrays so they decay to void * without const-casting. */
static char sAlpha[] = "Alpha";
static char sBeta[]  = "Beta";
static char sGamma[] = "Gamma";

TEST(CollectionDescriptorString, CountAndAt)
{
    void *slots[] = { sAlpha, sBeta, sGamma };
    CollectionDescriptor cd = { slots, 3, 0, nullptr };

    EXPECT_EQ(CollectionDescriptorStringCount(&cd), 3u);
    EXPECT_STREQ(CollectionDescriptorStringAt(&cd, 0), "Alpha");
    EXPECT_STREQ(CollectionDescriptorStringAt(&cd, 2), "Gamma");
    EXPECT_EQ(CollectionDescriptorStringAt(&cd, 3), nullptr);   // out of range
    EXPECT_EQ(CollectionDescriptorStringAt(&cd, 999), nullptr);
}

TEST(CollectionDescriptorString, NullHandling)
{
    EXPECT_EQ(CollectionDescriptorStringCount(nullptr), 0u);
    EXPECT_EQ(CollectionDescriptorStringAt(nullptr, 0), nullptr);

    CollectionDescriptor empty = { nullptr, 0, 0, nullptr };
    EXPECT_EQ(CollectionDescriptorStringAt(&empty, 0), nullptr);
}

TEST(CollectionStringIterator, IteratesInOrder)
{
    void *slots[] = { sAlpha, sBeta, sGamma };
    CollectionDescriptor cd = { slots, 3, 0, nullptr };

    CollectionStringIterator it = CollectionStringIteratorMake(&cd);

    const char *s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Alpha");
    s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Beta");
    s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Gamma");
    EXPECT_EQ(CollectionStringIteratorNext(&it), nullptr);  // exhausted
}

TEST(CollectionStringIterator, SkipsNullSlots)
{
    void *slots[] = { sAlpha, nullptr, sGamma, nullptr };
    CollectionDescriptor cd = { slots, 4, 0, nullptr };

    CollectionStringIterator it = CollectionStringIteratorMake(&cd);

    const char *s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Alpha");
    s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Gamma");
    EXPECT_EQ(CollectionStringIteratorNext(&it), nullptr);
}

TEST(CollectionStringIterator, ResetRewinds)
{
    void *slots[] = { sAlpha, sBeta };
    CollectionDescriptor cd = { slots, 2, 0, nullptr };

    CollectionStringIterator it = CollectionStringIteratorMake(&cd);

    ASSERT_NE(CollectionStringIteratorNext(&it), nullptr);
    ASSERT_NE(CollectionStringIteratorNext(&it), nullptr);
    EXPECT_EQ(CollectionStringIteratorNext(&it), nullptr);

    CollectionStringIteratorReset(&it);
    const char *s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "Alpha");  // rewound to the first element
}

TEST(CollectionTokenise, BasicSingleChar)
{
    char buf[] = "one,two,three";
    char *t = CollectionTokenise(buf, ',');
    EXPECT_STREQ(t, "one");
    t = CollectionTokenise(nullptr, ',');
    EXPECT_STREQ(t, "two");
    t = CollectionTokenise(nullptr, ',');
    EXPECT_STREQ(t, "three");
    t = CollectionTokenise(nullptr, ',');
    EXPECT_EQ(t, nullptr);
}

TEST(CollectionTokenise, CollapsesConsecutiveDelimiters)
{
    char buf[] = "a,,b,,";
    char *t = CollectionTokenise(buf, ',');
    EXPECT_STREQ(t, "a");
    t = CollectionTokenise(nullptr, ',');
    EXPECT_STREQ(t, "b");
    t = CollectionTokenise(nullptr, ',');
    EXPECT_EQ(t, nullptr);
}

TEST(CollectionFromString, SpaceTokenisedOneSlab)
{
    CollectionDescriptor *cd = CollectionFromStringSpaceTokenised("alpha beta gamma");
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 3u);
    EXPECT_EQ(cd->collection_base_offset, 0u);

    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 0), "alpha");
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 1), "beta");
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 2), "gamma");

    free(cd);  // one free releases the whole slab
}

TEST(CollectionFromString, ColonTokenisedCollapsesEmpty)
{
    CollectionDescriptor *cd = CollectionFromStringColonTokenised("a:b::c");
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 3u);  // empty token collapsed
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 0), "a");
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 1), "b");
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 2), "c");
    free(cd);
}

TEST(CollectionFromString, CommaTokenised)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("x,y,z");
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 3u);
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 0), "x");
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 2), "z");
    free(cd);
}

TEST(CollectionFromString, EnumBased)
{
    CollectionDescriptor *cd = CollectionFromStringTokenised("p q r", STR_TOKEN_SPACE);
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 3u);
    EXPECT_STREQ(CollectionDescriptorStringAt(cd, 1), "q");
    free(cd);
}

TEST(CollectionFromString, NullAndAllDelimiters)
{
    EXPECT_EQ(CollectionFromStringSpaceTokenised(nullptr), nullptr);

    CollectionDescriptor *cd = CollectionFromStringSpaceTokenised("   ");
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 0u);
    EXPECT_EQ(cd->collection, nullptr);
    free(cd);
}

TEST(CollectionFromString, IteratorIntegration)
{
    CollectionDescriptor *cd = CollectionFromStringColonTokenised("one:two:three");
    ASSERT_NE(cd, nullptr);

    CollectionStringIterator it = CollectionStringIteratorMake(cd);
    const char *s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "one");
    s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "two");
    s = CollectionStringIteratorNext(&it);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "three");
    EXPECT_EQ(CollectionStringIteratorNext(&it), nullptr);

    free(cd);
}

TEST(CollectionFromStringDescribe, Json)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("a,b,c");
    ASSERT_NE(cd, nullptr);

    char *json = CollectionFromStringDescribe(cd, DESCRIBE_FORMAT_JSON);
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(json, "[\"a\",\"b\",\"c\"]");
    free(json);
    free(cd);
}

TEST(CollectionFromStringDescribe, Yaml)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("a,b,c");
    ASSERT_NE(cd, nullptr);

    char *yaml = CollectionFromStringDescribe(cd, DESCRIBE_FORMAT_YAML);
    ASSERT_NE(yaml, nullptr);
    EXPECT_STREQ(yaml, "- a\n- b\n- c\n");
    free(yaml);
    free(cd);
}

TEST(CollectionFromStringDescribe, Ini)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("a,b,c");
    ASSERT_NE(cd, nullptr);

    char *ini = CollectionFromStringDescribe(cd, DESCRIBE_FORMAT_INI);
    ASSERT_NE(ini, nullptr);
    EXPECT_STREQ(ini, "0=a\n1=b\n2=c\n");
    free(ini);
    free(cd);
}

TEST(CollectionFromStringDescribe, ConvenienceFrontends)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("x,y");
    ASSERT_NE(cd, nullptr);

    char *json = CollectionFromStringDescribeAsJson(cd);
    char *yaml = CollectionFromStringDescribeAsYaml(cd);
    char *ini  = CollectionFromStringDescribeAsIni(cd);
    EXPECT_STREQ(json, "[\"x\",\"y\"]");
    EXPECT_STREQ(yaml, "- x\n- y\n");
    EXPECT_STREQ(ini,  "0=x\n1=y\n");
    free(json);
    free(yaml);
    free(ini);
    free(cd);
}

TEST(CollectionFromStringDescribe, JsonEscapesQuotes)
{
    CollectionDescriptor *cd = CollectionFromStringCommaTokenised("a\"b,c");
    ASSERT_NE(cd, nullptr);

    char *json = CollectionFromStringDescribeAsJson(cd);
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(json, R"(["a\"b","c"])");
    free(json);
    free(cd);
}

TEST(CollectionFromStringDescribe, NullAndEmpty)
{
    EXPECT_EQ(CollectionFromStringDescribe(nullptr, DESCRIBE_FORMAT_JSON), nullptr);

    CollectionDescriptor *cd = CollectionFromStringSpaceTokenised("   ");
    ASSERT_NE(cd, nullptr);

    char *json = CollectionFromStringDescribeAsJson(cd);
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(json, "[]");
    free(json);
    free(cd);
}

/* ── Callback iteration ─────────────────────────────────────────────────── */

static size_t      sOpCount = 0;
static size_t      sOpIndices[8];
static const char *sOpNodes[8];

static void
sOpVisit(size_t index, const char *node)
{
    if (sOpCount < 8) {
        sOpIndices[sOpCount] = index;
        sOpNodes[sOpCount]   = node;
    }
    sOpCount++;
}

TEST(CollectionStringIteratorWithOperator, VisitsEachNode)
{
    void *slots[] = { sAlpha, sBeta, sGamma };
    CollectionDescriptor cd = { slots, 3, 0, nullptr };

    sOpCount = 0;
    CollectionStringIteratorWithOperator(&cd, sOpVisit);

    EXPECT_EQ(sOpCount, 3u);
    EXPECT_EQ(sOpIndices[0], 0u); EXPECT_STREQ(sOpNodes[0], "Alpha");
    EXPECT_EQ(sOpIndices[1], 1u); EXPECT_STREQ(sOpNodes[1], "Beta");
    EXPECT_EQ(sOpIndices[2], 2u); EXPECT_STREQ(sOpNodes[2], "Gamma");
}

TEST(CollectionStringIteratorWithOperator, SkipsNullSlots)
{
    void *slots[] = { sAlpha, nullptr, sGamma, nullptr };
    CollectionDescriptor cd = { slots, 4, 0, nullptr };

    sOpCount = 0;
    CollectionStringIteratorWithOperator(&cd, sOpVisit);

    EXPECT_EQ(sOpCount, 2u);
    EXPECT_EQ(sOpIndices[0], 0u); EXPECT_STREQ(sOpNodes[0], "Alpha");
    /* "Gamma" sits at slot 2 — the NULL at slot 1 is skipped, not counted. */
    EXPECT_EQ(sOpIndices[1], 2u); EXPECT_STREQ(sOpNodes[1], "Gamma");
}
