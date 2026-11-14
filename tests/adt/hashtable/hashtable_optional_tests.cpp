#include <gtest/gtest.h>
#include <cerrno>
#include <cstddef>
#include <thread>

extern "C" {
#include <uflib/adt/adt_hashtable.h>
#include <uflib/optional/ufoptional.h>
void *__real_malloc(size_t size);
}

static thread_local bool fail_next_malloc;

extern "C" void *__wrap_malloc(size_t size)
{
  if (fail_next_malloc) {
    fail_next_malloc = false;
    errno = ENOMEM;
    return nullptr;
  }
  return __real_malloc(size);
}

namespace {
struct Item {
  const char *key;
  int references;
};

void Retain(RecyclerClientData *data, int count)
{
  static_cast<Item *>(static_cast<void *>(data))->references += count;
}

class HashTableOptional : public ::testing::Test {
protected:
  HashTable table{};
  Item item{"present", 0};

  void SetUp() override
  {
    ASSERT_EQ(HashTableLockingInstantiate(&table, offsetof(Item, key), KEY_SIZE_ZERO,
              HASH_ITEM_IS_PTR_TYPE, "optional-lookup", HASHTABLE_DEFAULT_EXTRACTOR), &table);
  }

  void TearDown() override
  {
    fail_next_malloc = false;
    HashTableDestruct(&table, false);
  }

  void Insert() { ASSERT_EQ(AddToHash(&table, &item), &item); }
};

TEST_F(HashTableOptional, PresentAcquiresExactlyOneReferenceAndTransfersPinToCaller)
{
  Insert();
  errno = EACCES;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", Retain, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(errno, 0);
  EXPECT_TRUE(UfOptionalIsPresent(result));
  EXPECT_EQ(UfOptionalTake(result), &item);
  EXPECT_TRUE(UfOptionalIsEmpty(result));
  UfOptionalDestroy(result);
  EXPECT_EQ(item.references, 1);
  item.references--;
}

TEST_F(HashTableOptional, WrapperDestructionDoesNotReleaseCallerOwnedReference)
{
  Insert();
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", Retain, nullptr);
  ASSERT_NE(result, nullptr);
  UfOptionalDestroy(result);
  EXPECT_EQ(item.references, 1);
  item.references--;
}

TEST_F(HashTableOptional, LegacyLookupUsesOptionalFailurePathWithoutLeakingPin)
{
  Insert();
  fail_next_malloc = true;
  void *result = HashLookupWithReference(&table, "present", Retain);
  EXPECT_FALSE(fail_next_malloc);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(item.references, 0);
  fail_next_malloc = false;
}

TEST_F(HashTableOptional, LegacyLookupKeepsPointerOrNullContract)
{
  Insert();
  EXPECT_EQ(HashLookupWithReference(&table, "present", Retain), &item);
  EXPECT_EQ(item.references, 1);
  EXPECT_EQ(HashLookupWithReference(&table, "absent", Retain), nullptr);
  EXPECT_EQ(item.references, 1);
  EXPECT_EQ(HashLookupWithReference(nullptr, "present", Retain), nullptr);
  item.references--;
}

TEST_F(HashTableOptional, MissingKeyReturnsAllocatedEmptyWithoutReference)
{
  Insert();
  errno = EACCES;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "absent", Retain, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(errno, 0);
  EXPECT_TRUE(UfOptionalIsEmpty(result));
  EXPECT_EQ(item.references, 0);
  UfOptionalDestroy(result);
}

TEST_F(HashTableOptional, EmptyTableIsAbsence)
{
  UfOptional *result = HashLookupWithReferenceOptional(&table, "absent", Retain, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(UfOptionalIsEmpty(result));
  EXPECT_EQ(errno, 0);
  UfOptionalDestroy(result);
}

TEST_F(HashTableOptional, InvalidInputsAreErrors)
{
  EXPECT_EQ(HashLookupWithReferenceOptional(nullptr, "present", Retain, nullptr), nullptr);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(HashLookupWithReferenceOptional(&table, nullptr, Retain, nullptr), nullptr);
  EXPECT_EQ(errno, EINVAL);
}

TEST_F(HashTableOptional, ContentionIsErrorForPresentAndMissingKeysThenRetryWorks)
{
  Insert();
  ASSERT_EQ(pthread_rwlock_wrlock(&table.hashtable_rwlock), 0);
  UfOptional *present = nullptr, *missing = nullptr;
  int present_error = 0, missing_error = 0;
  std::thread reader([&] {
    errno = EACCES;
    present = HashLookupWithReferenceOptional(&table, "present", Retain, nullptr);
    present_error = errno;
    errno = EACCES;
    missing = HashLookupWithReferenceOptional(&table, "absent", Retain, nullptr);
    missing_error = errno;
  });
  reader.join();
  EXPECT_EQ(pthread_rwlock_unlock(&table.hashtable_rwlock), 0);
  EXPECT_EQ(present, nullptr);
  EXPECT_EQ(missing, nullptr);
  EXPECT_EQ(present_error, EBUSY);
  EXPECT_EQ(missing_error, EBUSY);
  EXPECT_EQ(item.references, 0);
  UfOptionalDestroy(present);
  UfOptionalDestroy(missing);
  UfOptional *retry = HashLookupWithReferenceOptional(&table, "present", Retain, nullptr);
  ASSERT_NE(retry, nullptr);
  EXPECT_EQ(UfOptionalGet(retry), &item);
  EXPECT_EQ(item.references, 1);
  UfOptionalDestroy(retry);
  item.references--;
}

TEST_F(HashTableOptional, AllocationFailureCannotAcquireOrLeakReference)
{
  Insert();
  fail_next_malloc = true;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", Retain, nullptr);
  EXPECT_FALSE(fail_next_malloc);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(item.references, 0);
  UfOptionalDestroy(result);
  ASSERT_EQ(pthread_rwlock_trywrlock(&table.hashtable_rwlock), 0);
  EXPECT_EQ(pthread_rwlock_unlock(&table.hashtable_rwlock), 0);
}

TEST_F(HashTableOptional, AllocationFailureIsErrorEvenForAbsence)
{
  fail_next_malloc = true;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "absent", Retain, nullptr);
  EXPECT_FALSE(fail_next_malloc);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(errno, ENOMEM);
  UfOptionalDestroy(result);
}

TEST_F(HashTableOptional, NullableReferenceCallbackPreservesBorrowedLookupMode)
{
  Insert();
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", nullptr, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(UfOptionalGet(result), &item);
  EXPECT_EQ(item.references, 0);
  UfOptionalDestroy(result);
}

static thread_local HashTable *callback_table;
static thread_local int callback_write_lock;
void RetainUnderReadLock(RecyclerClientData *data, int count)
{
  callback_write_lock = pthread_rwlock_trywrlock(&callback_table->hashtable_rwlock);
  if (callback_write_lock == 0) pthread_rwlock_unlock(&callback_table->hashtable_rwlock);
  Retain(data, count);
  errno = EIO;
}

TEST_F(HashTableOptional, RetainsInsideLookupLockAndReleasesLockBeforeReturning)
{
  Insert();
  callback_table = &table;
  callback_write_lock = 0;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", RetainUnderReadLock, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(errno, 0);
  EXPECT_EQ(callback_write_lock, EBUSY);
  EXPECT_EQ(item.references, 1);
  ASSERT_EQ(pthread_rwlock_trywrlock(&table.hashtable_rwlock), 0);
  EXPECT_EQ(pthread_rwlock_unlock(&table.hashtable_rwlock), 0);
  UfOptionalDestroy(result);
  item.references--;
}

TEST_F(HashTableOptional, RepeatedMissesAndHitsLeaveTableUsable)
{
  Insert();
  for (int n = 0; n < 1000; ++n) {
    UfOptional *result = HashLookupWithReferenceOptional(&table, n % 2 ? "present" : "absent", Retain, nullptr);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(UfOptionalIsPresent(result), n % 2 != 0);
    if (UfOptionalIsPresent(result)) item.references--;
    UfOptionalDestroy(result);
  }
  EXPECT_EQ(item.references, 0);
  EXPECT_EQ(HashLookup(&table, const_cast<char *>("present"), true), &item);
}
TEST_F(HashTableOptional, SuppliedOptionalIsReusedWithoutAllocation)
{
  Insert();
  UfOptional *provided = UfOptionalEmpty(nullptr, nullptr);
  ASSERT_NE(provided, nullptr);
  fail_next_malloc = true;
  UfOptional *result = HashLookupWithReferenceOptional(&table, "present", Retain, provided);
  bool allocation_was_not_attempted = fail_next_malloc;
  fail_next_malloc = false;
  EXPECT_TRUE(allocation_was_not_attempted);
  ASSERT_EQ(result, provided);
  EXPECT_EQ(UfOptionalTake(provided), &item);
  EXPECT_EQ(item.references, 1);
  item.references--;
  EXPECT_EQ(HashLookupWithReferenceOptional(&table, "absent", Retain, provided), provided);
  EXPECT_TRUE(UfOptionalIsEmpty(provided));
  EXPECT_EQ(errno, 0);
  UfOptionalDestroy(provided);
}

TEST_F(HashTableOptional, SuppliedOptionalSurvivesLockErrorAndCanBeRetried)
{
  Insert();
  UfOptional *provided = UfOptionalEmpty(nullptr, nullptr);
  ASSERT_NE(provided, nullptr);
  ASSERT_EQ(pthread_rwlock_wrlock(&table.hashtable_rwlock), 0);
  UfOptional *result = provided;
  int error = 0;
  std::thread reader([&] {
    result = HashLookupWithReferenceOptional(&table, "present", Retain, provided);
    error = errno;
  });
  reader.join();
  EXPECT_EQ(pthread_rwlock_unlock(&table.hashtable_rwlock), 0);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(error, EBUSY);
  EXPECT_TRUE(UfOptionalIsEmpty(provided));
  EXPECT_EQ(item.references, 0);
  EXPECT_EQ(HashLookupWithReferenceOptional(&table, "present", Retain, provided), provided);
  EXPECT_EQ(UfOptionalTake(provided), &item);
  EXPECT_EQ(item.references, 1);
  item.references--;
  UfOptionalDestroy(provided);
}

TEST_F(HashTableOptional, OccupiedOptionalIsRejectedWithoutDiscardingItsPayload)
{
  Insert();
  int previous = 0;
  UfOptional *provided = UfOptionalOf(&previous, nullptr, nullptr);
  ASSERT_NE(provided, nullptr);
  EXPECT_EQ(HashLookupWithReferenceOptional(&table, "present", Retain, provided), nullptr);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(UfOptionalGet(provided), &previous);
  EXPECT_EQ(item.references, 0);
  UfOptionalDestroy(provided);
}

}
