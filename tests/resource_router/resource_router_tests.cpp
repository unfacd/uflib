/**
 * @file resource_router_tests.cpp
 * @brief Unit and concurrency tests for ResourceRouter.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/resource_router/resource_router.h>
}

namespace {

BufferDescriptor *onGet(ResourceHandlerContext *) { return nullptr; }
BufferDescriptor *onDel(ResourceHandlerContext *) { return nullptr; }
BufferDescriptor *onPut(ResourceHandlerContext *) { return nullptr; }

constexpr unsigned kAllMethods = RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_DEL | RESOURCE_ROUTE_METHOD_PUT;

ResourceRouteHandler makeHandler(unsigned methods)
{
  ResourceRouteHandler handler{};

  handler.on_get  = (methods & RESOURCE_ROUTE_METHOD_GET) ? onGet : nullptr;
  handler.on_del  = (methods & RESOURCE_ROUTE_METHOD_DEL) ? onDel : nullptr;
  handler.on_put  = (methods & RESOURCE_ROUTE_METHOD_PUT) ? onPut : nullptr;
  handler.methods = methods;
  return handler;
}

class RouterTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    router = ResourceRouteRegistryCreate();
    ASSERT_NE(router, nullptr);
    BufferDescriptorInit(&buffer, 256);
  }

  void TearDown() override
  {
    BufferDescriptorRelease(&buffer);
    ResourceRouteRegistryDestroy(router);
  }

  std::string describe()
  {
    buffer.size    = 0;
    buffer.data[0] = '\0';
    EXPECT_NE(ResourceRouteRegistryDescribe(router, &buffer), nullptr);
    return std::string(buffer.data ? buffer.data : "");
  }

  ResourceRouteRegistry *router{nullptr};
  BufferDescriptor        buffer{};
};

} // namespace

TEST(ResourceRouterLifecycle, CreateAndDestroy)
{
  ResourceRouteRegistry *router = ResourceRouteRegistryCreate();
  ASSERT_NE(router, nullptr);
  ResourceRouteRegistryDestroy(router);
  ResourceRouteRegistryDestroy(nullptr);
}

TEST(ResourceRouterLifecycle, DescribeEmptyRegistry)
{
  ResourceRouteRegistry *router = ResourceRouteRegistryCreate();
  BufferDescriptor       buffer;

  ASSERT_NE(router, nullptr);
  BufferDescriptorInit(&buffer, 64);

  EXPECT_EQ(ResourceRouteRegistryDescribe(router, &buffer), &buffer);
  EXPECT_STREQ(buffer.data, "{}");

  BufferDescriptorRelease(&buffer);
  ResourceRouteRegistryDestroy(router);
}

TEST(ResourceRouterLifecycle, DescribeRejectsNullArguments)
{
  ResourceRouteRegistry *router = ResourceRouteRegistryCreate();
  BufferDescriptor       buffer;

  BufferDescriptorInit(&buffer, 64);
  EXPECT_EQ(ResourceRouteRegistryDescribe(nullptr, &buffer), nullptr);
  EXPECT_EQ(ResourceRouteRegistryDescribe(router, nullptr), nullptr);

  BufferDescriptorRelease(&buffer);
  ResourceRouteRegistryDestroy(router);
}

TEST_F(RouterTest, RegisterRejectsNullArguments)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);

  EXPECT_NE(ResourceRouteRegister(nullptr, "a", nullptr, &handler, 0), 0);
  EXPECT_NE(ResourceRouteRegister(router, nullptr, nullptr, &handler, 0), 0);
  EXPECT_NE(ResourceRouteRegister(router, "a", nullptr, nullptr, 0), 0);
}

TEST_F(RouterTest, RegisterRejectsMalformedPaths)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const char *bad[] = { "", "/", "//", "///", "a//b", "a/", "/a/", "a//", "/a//b" };

  for (const char *path : bad) {
    EXPECT_NE(ResourceRouteRegister(router, path, nullptr, &handler, 0), 0) << "accepted '" << path << "'";
  }
  EXPECT_EQ(describe(), "{}");
}

TEST_F(RouterTest, RegisterRejectsZeroMethods)
{
  ResourceRouteHandler handler = makeHandler(0);

  EXPECT_NE(ResourceRouteRegister(router, "a", nullptr, &handler, 0), 0);
  EXPECT_EQ(describe(), "{}");
}

TEST_F(RouterTest, RegisterRejectsUnknownMethodBit)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET | 0x40u);

  EXPECT_NE(ResourceRouteRegister(router, "a", nullptr, &handler, 0), 0);
}

TEST_F(RouterTest, RegisterRejectsUnknownFlagBit)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);

  EXPECT_NE(ResourceRouteRegister(router, "a", nullptr, &handler, 0x80u), 0);
}

TEST_F(RouterTest, RegisterRejectsDuplicate)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);

  ASSERT_EQ(ResourceRouteRegister(router, "a/b", "first", &handler, 0), 0);
  EXPECT_NE(ResourceRouteRegister(router, "a/b", "second", &handler, 0), 0);
  EXPECT_NE(ResourceRouteRegister(router, "/a/b", "third", &handler, 0), 0);
}

TEST_F(RouterTest, MatchExactEveryMethod)
{
  ResourceRouteHandler handler = makeHandler(kAllMethods);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "desc", &handler, 0), 0);

  for (unsigned method : { RESOURCE_ROUTE_METHOD_GET, RESOURCE_ROUTE_METHOD_DEL, RESOURCE_ROUTE_METHOD_PUT }) {
    matched   = nullptr;
    remainder = reinterpret_cast<const char *>(1);
    EXPECT_EQ(ResourceRouteMatch(router, method, "suspend", &matched, &remainder), RESOURCE_ROUTE_MATCH);
    ASSERT_NE(matched, nullptr);
    EXPECT_EQ(matched->methods, kAllMethods);
    EXPECT_EQ(remainder, nullptr);
  }
}

TEST_F(RouterTest, MatchMethodNotAllowed)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", nullptr, &handler, 0), 0);

  matched = reinterpret_cast<const ResourceRouteHandler *>(1);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_DEL, "suspend", &matched, &remainder),
            RESOURCE_ROUTE_METHOD_NOT_ALLOWED);
  EXPECT_EQ(matched, nullptr);
  EXPECT_EQ(remainder, nullptr);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_PUT, "suspend", &matched, &remainder),
            RESOURCE_ROUTE_METHOD_NOT_ALLOWED);
}

TEST_F(RouterTest, MatchUnknownPath)
{
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspend", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "/", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
}

TEST_F(RouterTest, MatchRejectsInvalidMethodAndArguments)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "a", nullptr, &handler, 0), 0);

  EXPECT_EQ(ResourceRouteMatch(router, 0, "a", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, 0x40u, "a", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_DEL, "a", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(nullptr, RESOURCE_ROUTE_METHOD_GET, "a", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, nullptr, &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a", nullptr, nullptr), RESOURCE_ROUTE_MATCH);
}

TEST_F(RouterTest, NestedRoutesMatchRegardlessOfRegistrationOrder)
{
  ResourceRouteHandler parent   = makeHandler(kAllMethods);
  ResourceRouteHandler child    = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "a/b", nullptr, &child, 0), 0);
  ASSERT_EQ(ResourceRouteRegister(router, "a", nullptr, &parent, 0), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a", &matched, &remainder), RESOURCE_ROUTE_MATCH);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->methods, kAllMethods);
  EXPECT_EQ(remainder, nullptr);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a/b", &matched, &remainder), RESOURCE_ROUTE_MATCH);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->methods, RESOURCE_ROUTE_METHOD_GET);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_DEL, "a/b", &matched, &remainder),
            RESOURCE_ROUTE_METHOD_NOT_ALLOWED);
}

TEST_F(RouterTest, GroupNodeWithoutHandlerIsNotFound)
{
  ResourceRouteHandler child = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "a/b/c", nullptr, &child, 0), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a/b", &matched, &remainder), RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a/b/c", &matched, &remainder), RESOURCE_ROUTE_MATCH);
}

TEST_F(RouterTest, TrailingSlashAndExtraSlashesInMatchAreTolerated)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "a/b", nullptr, &handler, 0), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "/a/b", &matched, &remainder), RESOURCE_ROUTE_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a/b/", &matched, &remainder), RESOURCE_ROUTE_MATCH);
}

TEST_F(RouterTest, StemPassesBareAndTail)
{
  ResourceRouteHandler handler = makeHandler(kAllMethods);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "s", &handler, RESOURCE_ROUTE_STEMMED), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspend", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  ASSERT_NE(remainder, nullptr);
  EXPECT_STREQ(remainder, "");

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspend/thread1", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  ASSERT_NE(remainder, nullptr);
  EXPECT_STREQ(remainder, "thread1");

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspend/thread1/x/y", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  ASSERT_NE(remainder, nullptr);
  EXPECT_STREQ(remainder, "thread1/x/y");
}

TEST_F(RouterTest, StemDoesNotMatchLongerSegment)
{
  ResourceRouteHandler handler = makeHandler(kAllMethods);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "s", &handler, RESOURCE_ROUTE_STEMMED), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspendzzz", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "other/suspend", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
}

TEST_F(RouterTest, StemGatesMethodBeforePassingTail)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "s", &handler, RESOURCE_ROUTE_STEMMED), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_PUT, "suspend/thread1", &matched, &remainder),
            RESOURCE_ROUTE_METHOD_NOT_ALLOWED);
}

TEST_F(RouterTest, StemShadowsDescendants)
{
  ResourceRouteHandler stem      = makeHandler(kAllMethods);
  ResourceRouteHandler child     = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "suspend/thread1", nullptr, &child, 0), 0);
  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "s", &stem, RESOURCE_ROUTE_STEMMED), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "suspend/thread1", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->methods, kAllMethods);
  ASSERT_NE(remainder, nullptr);
  EXPECT_STREQ(remainder, "thread1");
}

TEST_F(RouterTest, HandlerIsCopiedByValue)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  handler.user_data = reinterpret_cast<void *>(0x1234);
  ASSERT_EQ(ResourceRouteRegister(router, "a", nullptr, &handler, 0), 0);

  handler.on_get    = nullptr;
  handler.methods   = RESOURCE_ROUTE_METHOD_PUT;
  handler.user_data = nullptr;

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a", &matched, &remainder), RESOURCE_ROUTE_MATCH);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->on_get, onGet);
  EXPECT_EQ(matched->methods, RESOURCE_ROUTE_METHOD_GET);
  EXPECT_EQ(matched->user_data, reinterpret_cast<void *>(0x1234));
}

TEST_F(RouterTest, DescriptionIsCopied)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  char                 description[16] = "original";

  ASSERT_EQ(ResourceRouteRegister(router, "a", description, &handler, 0), 0);
  std::strcpy(description, "mutated");

  EXPECT_NE(describe().find("\"description\":\"original\""), std::string::npos);
}

TEST_F(RouterTest, DescribeShowsRegisteredParentWithChildren)
{
  ResourceRouteHandler parent = makeHandler(kAllMethods);
  ResourceRouteHandler child  = makeHandler(RESOURCE_ROUTE_METHOD_GET);

  ASSERT_EQ(ResourceRouteRegister(router, "suspend", "suspend work", &parent, 0), 0);
  ASSERT_EQ(ResourceRouteRegister(router, "suspend/thread1", "one thread", &child, 0), 0);

  std::string json = describe();
  EXPECT_EQ(json.find("\"children\""), std::string::npos);
  EXPECT_NE(json.find("\"thread1\""), std::string::npos);
  EXPECT_NE(json.find("\"description\":\"one thread\""), std::string::npos);
}

TEST_F(RouterTest, DescribeShowsStemmedAndMethodSet)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_PUT);

  ASSERT_EQ(ResourceRouteRegister(router, "a", nullptr, &handler, RESOURCE_ROUTE_STEMMED), 0);

  std::string json = describe();
  EXPECT_NE(json.find("\"methods\":[\"GET\",\"PUT\"]"), std::string::npos);
  EXPECT_EQ(json.find("\"stemmed\""), std::string::npos);
  EXPECT_NE(json.find("\"description\":\"\""), std::string::npos);
}

TEST_F(RouterTest, DescribeEscapesControlCharacters)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);

  ASSERT_EQ(ResourceRouteRegister(router, "a", "quote\"slash\\tab\tend", &handler, 0), 0);

  std::string json = describe();
  EXPECT_NE(json.find("quote\\\"slash\\\\tab\\tend"), std::string::npos);
  EXPECT_EQ(json.find('\t'), std::string::npos);
}

TEST_F(RouterTest, RegisterTableSucceeds)
{
  ResourceRouteHandler suspend = makeHandler(kAllMethods);
  ResourceRouteHandler resume  = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "delegator/suspend", "suspend", &suspend, 0 },
    { "delegator/resume",  "resume",  &resume,  0 },
    { "delegator",         "root",    &suspend, 0 },
  };
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegisterTable(router, table, 3), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "delegator", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  EXPECT_EQ(remainder, nullptr);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "delegator/suspend", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  EXPECT_EQ(remainder, nullptr);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->methods, kAllMethods);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "delegator/unknown", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
}

TEST_F(RouterTest, RegisterTableStemmedParentShadowsRows)
{
  ResourceRouteHandler root    = makeHandler(kAllMethods);
  ResourceRouteHandler suspend = makeHandler(kAllMethods);
  const ResourceRouteEntry table[] = {
    { "delegator/suspend", "suspend", &suspend, 0 },
    { "delegator",         "root",    &root,    RESOURCE_ROUTE_STEMMED },
  };
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegisterTable(router, table, 2), 0);

  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "delegator/suspend", &matched, &remainder),
            RESOURCE_ROUTE_MATCH);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->methods, kAllMethods);
  ASSERT_NE(remainder, nullptr);
  EXPECT_STREQ(remainder, "suspend");
}

TEST_F(RouterTest, RegisterTableRejectsMalformedRowAtomically)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "alpha",     nullptr, &handler, 0 },
    { "bad//path", nullptr, &handler, 0 },
  };
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  EXPECT_EQ(ResourceRouteRegisterTable(router, table, 2), 2);
  EXPECT_EQ(describe(), "{}");
  EXPECT_EQ(ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "alpha", &matched, &remainder),
            RESOURCE_ROUTE_NO_MATCH);
}

TEST_F(RouterTest, RegisterTableRejectsRowZeroWithNonZeroReturn)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "", nullptr, &handler, 0 },
  };

  EXPECT_EQ(ResourceRouteRegisterTable(router, table, 1), 1);
}

TEST_F(RouterTest, RegisterTableRejectsDuplicateWithinTable)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "beta",  nullptr, &handler, 0 },
    { "/beta", nullptr, &handler, 0 },
  };

  EXPECT_EQ(ResourceRouteRegisterTable(router, table, 2), 2);
  EXPECT_EQ(describe(), "{}");
}

TEST_F(RouterTest, RegisterTableRejectsDuplicateAgainstRegistry)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "beta", nullptr, &handler, 0 },
  };

  ASSERT_EQ(ResourceRouteRegister(router, "beta", nullptr, &handler, 0), 0);
  EXPECT_EQ(ResourceRouteRegisterTable(router, table, 1), 1);
}

TEST_F(RouterTest, RegisterTableEdgeCases)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteEntry table[] = {
    { "a", nullptr, &handler, 0 },
  };

  EXPECT_EQ(ResourceRouteRegisterTable(router, table, 0), 0);
  EXPECT_EQ(ResourceRouteRegisterTable(router, nullptr, 1), 1);
}

TEST_F(RouterTest, ConcurrentReadsOnFrozenRegistry)
{
  constexpr int kThreads = 8;
  constexpr int kIterations = 200000;

  ResourceRouteHandler handler = makeHandler(kAllMethods);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  for (int i = 0; i < 32; i++) {
    std::string path = "cmd" + std::to_string(i);
    ASSERT_EQ(ResourceRouteRegister(router, path.c_str(), nullptr, &handler,
                                    (i % 4 == 0) ? RESOURCE_ROUTE_STEMMED : 0), 0);
  }

  std::atomic<unsigned long> errors{0};
  std::vector<std::thread>   workers;

  for (int t = 0; t < kThreads; t++) {
    workers.emplace_back([&, t]() {
      for (int i = 0; i < kIterations; i++) {
        int           idx     = (i + t) % 32;
        bool          stemmed = (idx % 4 == 0);
        std::string   path    = "cmd" + std::to_string(idx);
        const char   *rem     = nullptr;
        const ResourceRouteHandler *hit = nullptr;

        if (ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, path.c_str(), &hit, &rem) != RESOURCE_ROUTE_MATCH) {
          errors.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (hit == nullptr) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (stemmed && rem == nullptr) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (!stemmed && rem != nullptr) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }

  EXPECT_EQ(errors.load(), 0UL);
}

TEST_F(RouterTest, MatchDoesNotDisturbRegistryState)
{
  ResourceRouteHandler handler = makeHandler(RESOURCE_ROUTE_METHOD_GET);
  const ResourceRouteHandler *matched = nullptr;
  const char                 *remainder = nullptr;

  ASSERT_EQ(ResourceRouteRegister(router, "a", "desc", &handler, 0), 0);
  std::string before = describe();

  for (int i = 0; i < 1000; i++) {
    (void)ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "a", &matched, &remainder);
    (void)ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_DEL, "a", &matched, &remainder);
    (void)ResourceRouteMatch(router, RESOURCE_ROUTE_METHOD_GET, "zzz", &matched, &remainder);
  }

  EXPECT_EQ(describe(), before);
}
