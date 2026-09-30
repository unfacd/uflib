/**
 * @file ufoptional_tests.cpp
 * @brief Adversarial test suite for the UfOptional module.
 *
 * UfOptional is an owning container: almost every bug it can have is an
 * ownership bug, and an ownership bug is invisible to an assertion that only
 * checks a returned value.  So this suite pairs ordinary contract tests with a
 * counting release callback and deliberately hostile call sequences — payloads
 * re-set on top of themselves, mappers that publish and then fail, takes that
 * find nothing, callbacks that report failure after doing work — in order to
 * break the code rather than record what it currently does.
 *
 * Every payload here is a heap object released through Release(), so a missed
 * release, a doubled release or a dangling read shows up either as a counter
 * that disagrees or under AddressSanitizer/LeakSanitizer, which is how this
 * suite is meant to be run.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gtest/gtest.h"

#include <cstddef>

extern "C" {
#include <uflib/optional/ufoptional.h>
}

namespace {

/* ── The payload ───────────────────────────────────────────────────────── */

/* Every payload in this suite is one of these, and every release goes through
 * Release(), so the counters below are the whole ownership record. */
struct Payload
{
  int  value;
  int *release_count; /* bumped on release, or nullptr when nobody is counting */
};

/* Total releases across the suite, for the churn test's balance check. */
int g_releases = 0;

void
Release(void *value, void *context)
{
  (void)context;
  ++g_releases;

  auto *payload = static_cast<Payload *>(value);
  if (payload->release_count != nullptr) {
    ++*payload->release_count;
  }
  delete payload;
}

Payload *
Make(int value, int *release_count = nullptr)
{
  return new Payload{value, release_count};
}

int
ValueOf(const UfOptional *optional)
{
  auto *payload = static_cast<const Payload *>(UfOptionalGet(optional));
  return payload != nullptr ? payload->value : -1;
}

/* ── Callbacks ─────────────────────────────────────────────────────────── */

int g_clone_calls       = 0;
int g_predicate_calls   = 0;
int g_consumer_calls    = 0;
int g_supplier_calls    = 0;
int g_mapper_calls      = 0;
int g_flat_mapper_calls = 0;

void *
ClonePayload(const void *value, void *context)
{
  ++g_clone_calls;
  (void)context;
  return Make(static_cast<const Payload *>(value)->value);
}

/* The one legitimate way for a clone to report that it could not copy. */
void *
CloneRefuse(const void *value, void *context)
{
  ++g_clone_calls;
  (void)value;
  (void)context;
  return nullptr;
}

bool
PredicateEven(const void *value, void *context)
{
  ++g_predicate_calls;
  (void)context;
  return (static_cast<const Payload *>(value)->value % 2) == 0;
}

bool
ConsumerOkay(const void *value, void *context)
{
  ++g_consumer_calls;
  (void)value;
  (void)context;
  return true;
}

/* Does its work and then reports failure -- the shape that tempts an
 * implementation into releasing twice or not at all. */
bool
ConsumerRefuse(const void *value, void *context)
{
  ++g_consumer_calls;
  (void)value;
  (void)context;
  return false;
}

void *
SupplyFromContext(void *context)
{
  ++g_supplier_calls;
  return context;
}

bool
MapperDoubled(const void *value, void *context, void **mapped)
{
  ++g_mapper_calls;
  (void)context;
  *mapped = Make(static_cast<const Payload *>(value)->value * 2);
  return *mapped != nullptr;
}

bool
MapperPublishesNull(const void *value, void *context, void **mapped)
{
  ++g_mapper_calls;
  (void)value;
  (void)context;
  *mapped = nullptr;
  return true;
}

bool
MapperRefusesClean(const void *value, void *context, void **mapped)
{
  ++g_mapper_calls;
  (void)value;
  (void)context;
  (void)mapped;
  return false;
}

/* Publishes a live payload and then reports failure.  Nothing in the caller's
 * code can see that payload afterwards, so if the library does not release it,
 * it is gone.  The context is the release counter, so the published payload
 * reports its own release back to the test that provoked it. */
bool
MapperRefusesAfterPublishing(const void *value, void *context, void **mapped)
{
  ++g_mapper_calls;
  *mapped = Make(static_cast<const Payload *>(value)->value + 1000,
                 static_cast<int *>(context));
  return false;
}

/* The same, counting the released payload through the context -- for the churn
 * test, which has to account for every payload it creates. */
bool
MapperDoubledCounted(const void *value, void *context, void **mapped)
{
  ++g_mapper_calls;
  *mapped = Make(static_cast<const Payload *>(value)->value * 2,
                 static_cast<int *>(context));
  return *mapped != nullptr;
}

bool
FlatMapperIncrement(const void *value, void *context, UfOptional **mapped)
{
  ++g_flat_mapper_calls;
  (void)context;
  *mapped = UfOptionalOf(Make(static_cast<const Payload *>(value)->value + 1), Release, nullptr);
  return *mapped != nullptr;
}

bool
FlatMapperPublishesNull(const void *value, void *context, UfOptional **mapped)
{
  ++g_flat_mapper_calls;
  (void)value;
  (void)context;
  *mapped = nullptr;
  return true;
}

bool
FlatMapperRefusesAfterPublishing(const void *value, void *context, UfOptional **mapped)
{
  ++g_flat_mapper_calls;
  *mapped = UfOptionalOf(Make(static_cast<const Payload *>(value)->value + 2000,
                              static_cast<int *>(context)), Release, nullptr);
  return false;
}

/* ── A release that records what it was handed ─────────────────────────── */

struct ReleaseRecord
{
  void *seen_value;
  void *seen_context;
  int   calls;
};

void
ReleaseRecording(void *value, void *context)
{
  auto *record = static_cast<ReleaseRecord *>(context);
  ++record->calls;
  record->seen_value   = value;
  record->seen_context = context;
  delete static_cast<Payload *>(value);
}

}  // namespace

/* ══ Construction ════════════════════════════════════════════════════════ */

TEST(UfOptionalConstruction, EmptyHoldsNothing)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_TRUE(UfOptionalIsEmpty(opt));
  EXPECT_FALSE(UfOptionalIsPresent(opt));
  EXPECT_EQ(UfOptionalGet(opt), nullptr);

  UfOptionalDestroy(opt);
}

/* An empty container that carries a release callback must not call it: there is
 * no payload, and the callback would be handed NULL. */
TEST(UfOptionalConstruction, EmptyNeverReleases)
{
  int releases = 0;

  UfOptional *opt = UfOptionalEmpty(Release, &releases);
  ASSERT_NE(opt, nullptr);
  UfOptionalDestroy(opt);

  EXPECT_EQ(releases, 0);
}

TEST(UfOptionalConstruction, OfTakesOwnershipOfThePayload)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(42, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_TRUE(UfOptionalIsPresent(opt));
  EXPECT_EQ(ValueOf(opt), 42);
  EXPECT_EQ(releases, 0) << "a stored payload is not released on the way in";

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalConstruction, OfRejectsANullPayload)
{
  EXPECT_EQ(UfOptionalOf(nullptr, Release, nullptr), nullptr);
}

TEST(UfOptionalConstruction, OfNullableCollapsesNullToEmpty)
{
  UfOptional *opt = UfOptionalOfNullable(nullptr, Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_TRUE(UfOptionalIsEmpty(opt));
  EXPECT_EQ(UfOptionalGet(opt), nullptr);

  UfOptionalDestroy(opt);
}

TEST(UfOptionalConstruction, OfNullableKeepsANonNullPayload)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOfNullable(Make(7, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(ValueOf(opt), 7);
  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

/* ══ Take ════════════════════════════════════════════════════════════════ */

TEST(UfOptionalTake, HandsOwnershipToTheCaller)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(7, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  auto *taken = static_cast<Payload *>(UfOptionalTake(opt));
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(taken->value, 7);
  EXPECT_EQ(releases, 0) << "the container must not release what the caller now owns";
  EXPECT_TRUE(UfOptionalIsEmpty(opt));
  EXPECT_EQ(UfOptionalGet(opt), nullptr);

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 0) << "destroying afterwards must not release it a second time";

  delete taken;
}

TEST(UfOptionalTake, OnAnEmptyContainerIsANoOp)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalTake(opt), nullptr);
  EXPECT_TRUE(UfOptionalIsEmpty(opt));

  UfOptionalDestroy(opt);
}

TEST(UfOptionalTake, TwiceYieldsNullTheSecondTime)
{
  UfOptional *opt = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  delete static_cast<Payload *>(UfOptionalTake(opt));

  EXPECT_EQ(UfOptionalTake(opt), nullptr);
  UfOptionalDestroy(opt);
}

/* ══ Set ═════════════════════════════════════════════════════════════════ */

/* Setting the payload the container already holds must not release it: the
 * pointer is stored straight back, so releasing first leaves it dangling. */
TEST(UfOptionalSet, ReSettingTheHeldPayloadKeepsItAlive)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(5, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  void *held = UfOptionalGet(opt);
  ASSERT_NE(held, nullptr);

  EXPECT_EQ(UfOptionalSet(opt, held, Release, nullptr), UFOPTIONAL_OK);

  EXPECT_EQ(releases, 0);
  EXPECT_EQ(UfOptionalGet(opt), held);
  EXPECT_EQ(ValueOf(opt), 5) << "reading through the stored pointer must still be valid";

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalSet, ReplacingReleasesTheOldPayloadExactlyOnce)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(1, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalSet(opt, Make(2, &releases), Release, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(releases, 1);
  EXPECT_EQ(ValueOf(opt), 2);

  EXPECT_EQ(UfOptionalSet(opt, nullptr, Release, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(releases, 2);
  EXPECT_TRUE(UfOptionalIsEmpty(opt));

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 2) << "an emptied container has nothing left to release";
}

TEST(UfOptionalSet, OnAnEmptyContainerJustStores)
{
  int releases = 0;

  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalSet(opt, Make(3, &releases), Release, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(releases, 0);
  EXPECT_EQ(ValueOf(opt), 3);

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

/* ══ Map ═════════════════════════════════════════════════════════════════ */

TEST(UfOptionalMap, LeavesTheSourceUntouched)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(21, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  ASSERT_EQ(UfOptionalMap(opt, MapperDoubled, nullptr, Release, &mapped), UFOPTIONAL_OK);
  ASSERT_NE(mapped, nullptr);

  EXPECT_EQ(ValueOf(opt), 21);
  EXPECT_EQ(ValueOf(mapped), 42);
  EXPECT_NE(UfOptionalGet(opt), UfOptionalGet(mapped)) << "the copy must be its own object";

  UfOptionalDestroy(mapped);
  EXPECT_EQ(releases, 0);
  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalMap, OnAnEmptyContainerYieldsEmptyWithoutCallingTheMapper)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  g_mapper_calls     = 0;
  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalMap(opt, MapperDoubled, nullptr, Release, &mapped), UFOPTIONAL_EMPTY);
  EXPECT_EQ(g_mapper_calls, 0) << "nothing to map, so the mapper must not run";
  ASSERT_NE(mapped, nullptr);
  EXPECT_TRUE(UfOptionalIsEmpty(mapped));

  UfOptionalDestroy(mapped);
  UfOptionalDestroy(opt);
}

TEST(UfOptionalMap, MapperPublishingNullYieldsAnEmptyContainer)
{
  UfOptional *opt = UfOptionalOf(Make(3), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalMap(opt, MapperPublishesNull, nullptr, Release, &mapped), UFOPTIONAL_OK);
  ASSERT_NE(mapped, nullptr);
  EXPECT_TRUE(UfOptionalIsEmpty(mapped));

  UfOptionalDestroy(mapped);
  UfOptionalDestroy(opt);
}

TEST(UfOptionalMap, MapperFailingWithoutPublishingReleasesNothing)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(3, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalMap(opt, MapperRefusesClean, nullptr, Release, &mapped),
            UFOPTIONAL_CALLBACK_FAILED);
  EXPECT_EQ(releases, 0);
  EXPECT_EQ(mapped, nullptr);

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

/* A mapper that publishes a payload and then reports failure leaves that
 * payload reachable from nowhere.  If the library does not release it, nothing
 * else can. */
TEST(UfOptionalMap, MapperPublishingThenFailingIsReleasedOnce)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(3, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalMap(opt, MapperRefusesAfterPublishing, &releases, Release, &mapped),
            UFOPTIONAL_CALLBACK_FAILED);

  EXPECT_EQ(releases, 1) << "the published payload must be released by the library";
  EXPECT_EQ(mapped, nullptr) << "no container is handed back on failure";

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 2);
}

/* The out-parameter must not be disturbed when the call is rejected before it
 * could produce anything: a caller reusing the variable would otherwise lose
 * the handle it already held. */
TEST(UfOptionalMap, InvalidArgumentsDoNotDisturbTheOutParameter)
{
  UfOptional *opt = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *sentinel = opt;
  EXPECT_EQ(UfOptionalMap(opt, nullptr, nullptr, Release, &sentinel), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(sentinel, opt);

  EXPECT_EQ(UfOptionalMap(nullptr, MapperDoubled, nullptr, Release, &sentinel),
            UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(sentinel, opt);

  EXPECT_EQ(UfOptionalMap(opt, MapperDoubled, nullptr, Release, nullptr),
            UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(ValueOf(opt), 1) << "a rejected call must leave the source alone";

  UfOptionalDestroy(opt);
}

/* The context that goes to the mapper is the one later handed to the mapped
 * payload's release callback -- one pointer serving two roles.  Pinned here so
 * that a change to that arrangement is a deliberate one. */
TEST(UfOptionalMap, HandsTheSameContextToTheMapperAndTheMappedRelease)
{
  ReleaseRecord record{};

  UfOptional *opt = UfOptionalOf(Make(3), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  ASSERT_EQ(UfOptionalMap(opt, MapperDoubled, &record, ReleaseRecording, &mapped), UFOPTIONAL_OK);
  ASSERT_NE(mapped, nullptr);
  EXPECT_EQ(record.calls, 0);

  UfOptionalDestroy(mapped);
  EXPECT_EQ(record.calls, 1);
  EXPECT_EQ(record.seen_context, &record);

  UfOptionalDestroy(opt);
}

/* ══ FlatMap ═════════════════════════════════════════════════════════════ */

TEST(UfOptionalFlatMap, UnwrapsTheMapperResult)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(9, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  ASSERT_EQ(UfOptionalFlatMap(opt, FlatMapperIncrement, nullptr, &mapped), UFOPTIONAL_OK);
  ASSERT_NE(mapped, nullptr);
  EXPECT_EQ(ValueOf(mapped), 10);

  UfOptionalDestroy(mapped);
  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalFlatMap, OnEmptyYieldsEmptyWithoutCallingTheMapper)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  g_flat_mapper_calls = 0;
  UfOptional *mapped  = nullptr;
  EXPECT_EQ(UfOptionalFlatMap(opt, FlatMapperIncrement, nullptr, &mapped), UFOPTIONAL_EMPTY);
  EXPECT_EQ(g_flat_mapper_calls, 0);
  ASSERT_NE(mapped, nullptr);
  EXPECT_TRUE(UfOptionalIsEmpty(mapped));

  UfOptionalDestroy(mapped);
  UfOptionalDestroy(opt);
}

/* Reporting success while publishing nothing is a mapper contract violation,
 * not a successful empty result. */
TEST(UfOptionalFlatMap, MapperPublishingNullIsACallbackFailure)
{
  UfOptional *opt = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalFlatMap(opt, FlatMapperPublishesNull, nullptr, &mapped),
            UFOPTIONAL_CALLBACK_FAILED);
  EXPECT_EQ(mapped, nullptr);

  UfOptionalDestroy(opt);
}

TEST(UfOptionalFlatMap, MapperPublishingThenFailingDestroysTheContainer)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(1, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;
  EXPECT_EQ(UfOptionalFlatMap(opt, FlatMapperRefusesAfterPublishing, &releases, &mapped),
            UFOPTIONAL_CALLBACK_FAILED);

  EXPECT_EQ(releases, 1) << "the published container's payload must be released";
  EXPECT_EQ(mapped, nullptr);

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 2);
}

/* ══ Clone ═══════════════════════════════════════════════════════════════ */

TEST(UfOptionalClone, ProducesAnIndependentOwner)
{
  int releases = 0;

  UfOptional *source = UfOptionalOf(Make(21, &releases), Release, nullptr);
  ASSERT_NE(source, nullptr);

  UfOptional *copy = UfOptionalClone(source, ClonePayload, Release, nullptr);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, source);
  EXPECT_EQ(ValueOf(copy), 21);
  EXPECT_NE(UfOptionalGet(copy), UfOptionalGet(source));

  UfOptionalDestroy(copy);
  EXPECT_EQ(releases, 0) << "releasing the copy must not touch the source's payload";
  EXPECT_EQ(ValueOf(source), 21);

  UfOptionalDestroy(source);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalClone, OnAnEmptySourceDoesNotInvokeTheClone)
{
  UfOptional *source = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(source, nullptr);

  g_clone_calls = 0;
  UfOptional *copy = UfOptionalClone(source, ClonePayload, Release, nullptr);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(g_clone_calls, 0);
  EXPECT_TRUE(UfOptionalIsEmpty(copy));

  UfOptionalDestroy(copy);
  UfOptionalDestroy(source);
}

TEST(UfOptionalClone, FailureLeavesTheSourceIntact)
{
  int releases = 0;

  UfOptional *source = UfOptionalOf(Make(9, &releases), Release, nullptr);
  ASSERT_NE(source, nullptr);

  EXPECT_EQ(UfOptionalClone(source, CloneRefuse, Release, nullptr), nullptr);
  EXPECT_EQ(releases, 0);
  EXPECT_EQ(ValueOf(source), 9);

  UfOptionalDestroy(source);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalClone, NullArgumentsAreRejected)
{
  UfOptional *source = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(source, nullptr);

  EXPECT_EQ(UfOptionalClone(nullptr, ClonePayload, Release, nullptr), nullptr);
  EXPECT_EQ(UfOptionalClone(source, nullptr, Release, nullptr), nullptr);

  EXPECT_EQ(ValueOf(source), 1);
  UfOptionalDestroy(source);
}

/* ══ Filter ══════════════════════════════════════════════════════════════ */

TEST(UfOptionalFilter, KeepingLeavesThePayloadOwned)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(4, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(releases, 0);
  EXPECT_TRUE(UfOptionalIsPresent(opt));
  EXPECT_EQ(ValueOf(opt), 4);

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1);
}

TEST(UfOptionalFilter, DiscardingReleasesThePayloadExactlyOnce)
{
  int releases = 0;

  UfOptional *opt = UfOptionalOf(Make(3, &releases), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(releases, 1);
  EXPECT_TRUE(UfOptionalIsEmpty(opt));

  UfOptionalDestroy(opt);
  EXPECT_EQ(releases, 1) << "the discarded payload is not released a second time";
}

TEST(UfOptionalFilter, EvaluatesThePredicateExactlyOnce)
{
  UfOptional *opt = UfOptionalOf(Make(4), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  g_predicate_calls = 0;
  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(g_predicate_calls, 1);

  UfOptionalDestroy(opt);
}

TEST(UfOptionalFilter, OnAnEmptyContainerDoesNotCallThePredicate)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);

  g_predicate_calls = 0;
  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_EMPTY);
  EXPECT_EQ(g_predicate_calls, 0);

  UfOptionalDestroy(opt);
}

/* ══ OrElse family ═══════════════════════════════════════════════════════ */

TEST(UfOptionalOrElse, ReturnsThePayloadOrTheFallback)
{
  int fallback = 99;

  UfOptional *opt = UfOptionalOf(Make(5), Release, nullptr);
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(UfOptionalOrElse(opt, &fallback), UfOptionalGet(opt));
  UfOptionalDestroy(opt);

  UfOptional *empty = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(UfOptionalOrElse(empty, &fallback), &fallback);
  UfOptionalDestroy(empty);
}

TEST(UfOptionalOrElseGet, ConsultsTheSupplierOnlyWhenEmpty)
{
  int fallback = 8;

  UfOptional *opt = UfOptionalOf(Make(2), Release, nullptr);
  ASSERT_NE(opt, nullptr);
  g_supplier_calls = 0;
  EXPECT_NE(UfOptionalOrElseGet(opt, SupplyFromContext, &fallback), &fallback);
  EXPECT_EQ(g_supplier_calls, 0) << "an expensive fallback must not be built when unused";
  UfOptionalDestroy(opt);

  UfOptional *empty = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(empty, nullptr);
  g_supplier_calls = 0;
  EXPECT_EQ(UfOptionalOrElseGet(empty, SupplyFromContext, &fallback), &fallback);
  EXPECT_EQ(g_supplier_calls, 1);
  UfOptionalDestroy(empty);
}

TEST(UfOptionalOrElseGet, NullSupplierYieldsNull)
{
  UfOptional *empty = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(empty, nullptr);

  EXPECT_EQ(UfOptionalOrElseGet(empty, nullptr, nullptr), nullptr);

  UfOptionalDestroy(empty);
}

TEST(UfOptionalOrElseThrow, HandsBackThePayload)
{
  UfOptional *opt = UfOptionalOf(Make(11), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  void *value = nullptr;
  EXPECT_EQ(UfOptionalOrElseThrow(opt, &value), UFOPTIONAL_OK);
  EXPECT_EQ(value, UfOptionalGet(opt));

  UfOptionalDestroy(opt);
}

/* A stale payload left in the out-parameter from an earlier call must not
 * survive a failed one. */
TEST(UfOptionalOrElseThrow, ClearsTheOutParameterBeforeAnyTest)
{
  UfOptional *empty = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(empty, nullptr);

  void *value = empty;
  EXPECT_EQ(UfOptionalOrElseThrow(empty, &value), UFOPTIONAL_NOT_PRESENT);
  EXPECT_EQ(value, nullptr);

  UfOptionalDestroy(empty);
}

TEST(UfOptionalOrElseThrow, NullOutParameterIsRejected)
{
  UfOptional *opt = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(UfOptionalOrElseThrow(opt, nullptr), UFOPTIONAL_INVALID_ARGUMENT);

  UfOptionalDestroy(opt);
}

/* ══ IfPresent ═══════════════════════════════════════════════════════════ */

TEST(UfOptionalIfPresent, RunsTheConsumerOnlyWhenPresent)
{
  UfOptional *opt = UfOptionalOf(Make(3), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  g_consumer_calls = 0;
  EXPECT_EQ(UfOptionalIfPresent(opt, ConsumerOkay, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(g_consumer_calls, 1);

  /* A consumer that reports failure is surfaced as such, and the payload it
   * saw stays owned by the container. */
  EXPECT_EQ(UfOptionalIfPresent(opt, ConsumerRefuse, nullptr), UFOPTIONAL_CALLBACK_FAILED);
  EXPECT_TRUE(UfOptionalIsPresent(opt));
  EXPECT_EQ(ValueOf(opt), 3);

  UfOptionalDestroy(opt);

  UfOptional *empty = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(empty, nullptr);
  g_consumer_calls = 0;
  EXPECT_EQ(UfOptionalIfPresent(empty, ConsumerOkay, nullptr), UFOPTIONAL_EMPTY);
  EXPECT_EQ(g_consumer_calls, 0);
  UfOptionalDestroy(empty);
}

/* ══ last_status ═════════════════════════════════════════════════════════ */

/* It records what the last state-changing call did -- including that it found
 * nothing to do -- and a read never disturbs it. */
TEST(UfOptionalLastStatus, RecordsStateChangingCallsOnly)
{
  UfOptional *opt = UfOptionalEmpty(Release, nullptr);
  ASSERT_NE(opt, nullptr);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_OK) << "a fresh container starts at OK";

  EXPECT_EQ(UfOptionalTake(opt), nullptr);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_EMPTY);

  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_EMPTY);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_EMPTY);

  /* Reads, and calls that only read the container, must leave the record be. */
  (void)UfOptionalIsPresent(opt);
  (void)UfOptionalGet(opt);
  UfOptional *mapped = nullptr;
  (void)UfOptionalMap(opt, MapperRefusesClean, nullptr, Release, &mapped);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_EMPTY);

  /* Mapping an empty container still hands back a container to own. */
  UfOptionalDestroy(mapped);

  EXPECT_EQ(UfOptionalSet(opt, Make(4), Release, nullptr), UFOPTIONAL_OK);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_OK);

  delete static_cast<Payload *>(UfOptionalTake(opt));
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_OK);

  EXPECT_EQ(UfOptionalFilter(opt, PredicateEven, nullptr), UFOPTIONAL_EMPTY);
  EXPECT_EQ(UfOptionalLastStatus(opt), UFOPTIONAL_EMPTY);

  UfOptionalDestroy(opt);
}

/* ══ Release callback ════════════════════════════════════════════════════ */

TEST(UfOptionalRelease, ReceivesTheStoredPayloadAndContext)
{
  ReleaseRecord record{};
  Payload      *payload = Make(1);

  UfOptional *opt = UfOptionalOf(payload, ReleaseRecording, &record);
  ASSERT_NE(opt, nullptr);

  UfOptionalDestroy(opt);

  EXPECT_EQ(record.calls, 1);
  EXPECT_EQ(record.seen_value, payload);
  EXPECT_EQ(record.seen_context, &record);
}

/* With no release callback a container holds a payload it will not release.
 * The payload here belongs to this frame rather than the heap, so an
 * implementation that released unconditionally would be freeing memory it was
 * never given -- and would take the test down with it. */
TEST(UfOptionalRelease, NullCallbackLeavesThePayloadToTheCaller)
{
  Payload borrowed{7, nullptr};

  UfOptional *opt = UfOptionalOf(&borrowed, nullptr, nullptr);
  ASSERT_NE(opt, nullptr);

  EXPECT_EQ(ValueOf(opt), 7);
  EXPECT_EQ(UfOptionalGet(opt), &borrowed);

  UfOptionalDestroy(opt);

  EXPECT_EQ(borrowed.value, 7) << "teardown must not have released a payload it does not own";
}

/* ══ A NULL handle is not a container ════════════════════════════════════ */

TEST(UfOptionalNullHandle, ReadAccessorsReportEmpty)
{
  int fallback = 0;

  EXPECT_FALSE(UfOptionalIsPresent(nullptr));
  EXPECT_TRUE(UfOptionalIsEmpty(nullptr));
  EXPECT_EQ(UfOptionalGet(nullptr), nullptr);
  EXPECT_EQ(UfOptionalTake(nullptr), nullptr);
  EXPECT_EQ(UfOptionalOrElse(nullptr, &fallback), &fallback);
  EXPECT_EQ(UfOptionalOrElseGet(nullptr, nullptr, &fallback), nullptr);
  EXPECT_EQ(UfOptionalLastStatus(nullptr), UFOPTIONAL_INVALID_ARGUMENT);
}

TEST(UfOptionalNullHandle, StatusReturningCallsRejectIt)
{
  void *value = nullptr;

  EXPECT_EQ(UfOptionalOrElseThrow(nullptr, &value), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalIfPresent(nullptr, ConsumerOkay, nullptr), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalFilter(nullptr, PredicateEven, nullptr), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalSet(nullptr, nullptr, nullptr, nullptr), UFOPTIONAL_INVALID_ARGUMENT);
}

TEST(UfOptionalNullHandle, NullCallbacksAreRejected)
{
  UfOptional *opt = UfOptionalOf(Make(1), Release, nullptr);
  ASSERT_NE(opt, nullptr);

  UfOptional *mapped = nullptr;

  EXPECT_EQ(UfOptionalIfPresent(opt, nullptr, nullptr), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalFilter(opt, nullptr, nullptr), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalMap(opt, nullptr, nullptr, Release, &mapped), UFOPTIONAL_INVALID_ARGUMENT);
  EXPECT_EQ(UfOptionalFlatMap(opt, nullptr, nullptr, &mapped), UFOPTIONAL_INVALID_ARGUMENT);

  EXPECT_EQ(ValueOf(opt), 1) << "a rejected call leaves the container untouched";
  UfOptionalDestroy(opt);
}

/* Teardown needs no guard branch. */
TEST(UfOptionalNullHandle, DestroyToleratesNull)
{
  UfOptionalDestroy(nullptr);
}

/* ══ Ownership under churn ═══════════════════════════════════════════════ */

/* Drives every owning path in a loop.  A release that is missed or doubled
 * shows up as a counter that disagrees; LeakSanitizer confirms it. */
TEST(UfOptionalOwnership, ChurnBalancesEveryRelease)
{
  constexpr int kRounds = 1000;

  int releases = 0;
  int taken    = 0;

  for (int i = 0; i < kRounds; ++i) {
    UfOptional *opt = UfOptionalOf(Make(i, &releases), Release, nullptr);
    ASSERT_NE(opt, nullptr);

    UfOptional *mapped = nullptr;
    ASSERT_EQ(UfOptionalMap(opt, MapperDoubledCounted, &releases, Release, &mapped), UFOPTIONAL_OK);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(ValueOf(mapped), i * 2);
    UfOptionalDestroy(mapped);

    /* Every third round the payload leaves with the caller instead. */
    if (i % 3 == 0) {
      delete static_cast<Payload *>(UfOptionalTake(opt));
      ++taken;
    }

    UfOptionalDestroy(opt);
  }

  /* One release per mapped payload, plus one per source payload not taken. */
  EXPECT_EQ(releases, kRounds + (kRounds - taken));
  EXPECT_EQ(taken, 334);
}
