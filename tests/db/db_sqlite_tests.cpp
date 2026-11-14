extern "C" {
#include <uflib/db/db_sql.h>
}

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

class DbSqliteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = ::testing::TempDir() + "ufsrv_db_sqlite_" + std::to_string(getpid()) + "_" +
            std::to_string(counter_++) + ".sqlite";
    std::remove(path_.c_str());

    db_ = h_connect_sqlite(path_.c_str());
    ASSERT_NE(db_, nullptr);
    ASSERT_EQ(h_execute_query(db_, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT, i INTEGER, d REAL, b BLOB)",
                              nullptr, H_OPTION_EXEC), H_OK);
  }

  void TearDown() override {
    if (db_ != nullptr) {
      h_close_db(db_);
      h_clean_connection(db_);
      db_ = nullptr;
    }
    std::remove(path_.c_str());
  }

  uint64_t QueryInt(const char *sql) {
    struct _h_result result = {0};
    uint64_t value = 0;

    EXPECT_EQ(h_query_select(db_, sql, &result), H_OK);
    if (result.data != nullptr && result.nb_rows > 0) {
      value = ((struct _h_type_int *) result.data[0][0].t_data)->value;
    }
    if (result.data != nullptr) {
      h_clean_result(&result);
    }

    return value;
  }

  struct _h_connection *db_ = nullptr;
  std::string path_;

 private:
  static int counter_;
};

int DbSqliteTest::counter_ = 0;

TEST(DbSqliteConnect, RejectsNullPath) {
  EXPECT_EQ(h_connect_sqlite(nullptr), nullptr);
}

TEST(DbSqliteConnect, FailsOnUnwritableDirectory) {
  EXPECT_EQ(h_connect_sqlite("/nonexistent-ufsrv-dir/sub/db.sqlite"), nullptr);
}

TEST(DbSqliteConnect, CreatesTheDatabaseFile) {
  const std::string path = ::testing::TempDir() + "ufsrv_db_sqlite_create.sqlite";
  std::remove(path.c_str());

  struct _h_connection *db = h_connect_sqlite(path.c_str());
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(db->type, HOEL_DB_TYPE_SQLITE);

  FILE *fp = fopen(path.c_str(), "rb");
  EXPECT_NE(fp, nullptr);
  if (fp != nullptr) fclose(fp);

  h_close_db(db);
  h_clean_connection(db);
  std::remove(path.c_str());
}

TEST_F(DbSqliteTest, EscapeRejectsNullInput) {
  EXPECT_EQ(h_escape_string(db_, nullptr), nullptr);
}

TEST_F(DbSqliteTest, EscapeDoublesSingleQuotes) {
  char *escaped = h_escape_string(db_, "it's");

  ASSERT_NE(escaped, nullptr);
  EXPECT_STREQ(escaped, "it''s");
  free(escaped);
}

TEST_F(DbSqliteTest, EscapeLeavesFormatSpecifiersLiteral) {
  const char *hostile = "%s%n%q%%%d";
  char *escaped = h_escape_string(db_, hostile);

  ASSERT_NE(escaped, nullptr);
  EXPECT_STREQ(escaped, hostile);
  free(escaped);
}

TEST_F(DbSqliteTest, EscapedInjectionAttemptIsStoredAsData) {
  const char *hostile = "x'); DROP TABLE t; --";
  char *escaped = h_escape_string(db_, hostile);
  ASSERT_NE(escaped, nullptr);

  std::string sql = std::string("INSERT INTO t (s) VALUES ('") + escaped + "')";
  free(escaped);

  EXPECT_EQ(h_query_insert(db_, sql.c_str()), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s FROM t", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  auto *text = (struct _h_type_text *) result.data[0][0].t_data;
  EXPECT_EQ(std::string(text->value, text->length), std::string(hostile));
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, EmptyStringRoundTrips) {
  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (s) VALUES ('')"), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s FROM t", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_EQ(result.data[0][0].type, HOEL_COL_TYPE_TEXT);
  auto *text = (struct _h_type_text *) result.data[0][0].t_data;
  EXPECT_EQ(text->length, 0u);
  ASSERT_NE(text->value, nullptr);
  EXPECT_EQ(text->value[0], '\0');
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, NullAndEmptyAreDistinct) {
  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (s) VALUES (NULL)"), H_OK);
  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (s) VALUES ('')"), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s, s IS NULL FROM t ORDER BY id", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 2u);
  EXPECT_EQ(result.data[0][0].type, HOEL_COL_TYPE_NULL);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][1].t_data)->value, 1u);
  EXPECT_EQ(result.data[1][0].type, HOEL_COL_TYPE_TEXT);
  EXPECT_EQ(((struct _h_type_int *) result.data[1][1].t_data)->value, 0u);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, BlobWithEmbeddedNulRoundTrips) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (b) VALUES (x'00ff0010')", nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT b FROM t", &result), H_OK);
  ASSERT_EQ(result.data[0][0].type, HOEL_COL_TYPE_BLOB);
  auto *blob = (struct _h_type_blob *) result.data[0][0].t_data;
  ASSERT_EQ(blob->length, 4u);
  const unsigned char expected[] = {0x00, 0xff, 0x00, 0x10};
  EXPECT_EQ(std::memcmp(blob->value, expected, 4), 0);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, EmptyQueryIsANoOp) {
  EXPECT_EQ(h_execute_query(db_, "", nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  EXPECT_EQ(h_execute_query(db_, "", &result, H_OPTION_SELECT), H_OK);
  EXPECT_EQ(result.nb_rows, 0u);
}

TEST_F(DbSqliteTest, CommentsOnlyIsANoOp) {
  EXPECT_EQ(h_execute_query(db_, "-- nothing here\n/* nor here */", nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  EXPECT_EQ(h_execute_query(db_, "-- nothing here", &result, H_OPTION_SELECT), H_OK);
  EXPECT_EQ(result.nb_rows, 0u);
}

TEST_F(DbSqliteTest, SelectRejectsStatementWithoutSelectKeyword) {
  struct _h_result result = {0};
  EXPECT_EQ(h_query_select(db_, "DELETE FROM t", &result), H_ERROR_PARAMS);
}

TEST_F(DbSqliteTest, MultiStatementExecRunsEveryStatement) {
  EXPECT_EQ(h_query_insert(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); INSERT INTO t (i) VALUES (3)"), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 3u);
}

TEST_F(DbSqliteTest, MultiStatementThousandStatements) {
  std::string sql;
  for (int i = 0; i < 1000; i++) {
    sql += "INSERT INTO t (i) VALUES (" + std::to_string(i) + ");";
  }

  EXPECT_EQ(h_query_insert(db_, sql.c_str()), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1000u);
}

TEST_F(DbSqliteTest, MultiStatementSelectReturnsLastRowProducingStatement) {
  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT 1 AS a; SELECT 42 AS b", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_EQ(result.nb_columns, 1u);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][0].t_data)->value, 42u);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, MultiStatementSelectRunsDdlFirst) {
  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "CREATE TABLE u (id INTEGER); INSERT INTO u VALUES (5); SELECT id FROM u", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][0].t_data)->value, 5u);
  h_clean_result(&result);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM u"), 1u);
}

TEST_F(DbSqliteTest, MultiStatementSelectLastStatementMayReturnNoRows) {
  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT 1 AS a; SELECT id FROM t WHERE 0", &result), H_OK);
  EXPECT_EQ(result.nb_rows, 0u);
}

TEST_F(DbSqliteTest, MultiStatementMalformedTailFails) {
  struct _h_result result = {0};
  EXPECT_EQ(h_query_select(db_, "SELECT 1 AS a; garbage", &result), H_ERROR_QUERY);
}

TEST_F(DbSqliteTest, ConnectionUsableAfterFailedStatement) {
  EXPECT_EQ(h_execute_query(db_, "SELECT * FROM no_such_table", nullptr, H_OPTION_EXEC), H_ERROR_QUERY);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);
}

TEST_F(DbSqliteTest, LastInsertIdIsNullBeforeAnyInsert) {
  struct _h_data *data = h_query_last_insert_id(db_);

  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->type, HOEL_COL_TYPE_NULL);
  h_clean_data_full(data);
}

TEST_F(DbSqliteTest, LastInsertIdTracksRowid) {
  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (i) VALUES (1)"), H_OK);
  struct _h_data *first = h_query_last_insert_id(db_);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(first->type, HOEL_COL_TYPE_INT);
  EXPECT_EQ(((struct _h_type_int *) first->t_data)->value, 1u);
  h_clean_data_full(first);

  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (id, i) VALUES (100, 2)"), H_OK);
  struct _h_data *second = h_query_last_insert_id(db_);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(second->type, HOEL_COL_TYPE_INT);
  EXPECT_EQ(((struct _h_type_int *) second->t_data)->value, 100u);
  h_clean_data_full(second);
}

TEST_F(DbSqliteTest, TwoConnectionsSeeEachOthersWrites) {
  struct _h_connection *other = h_connect_sqlite(path_.c_str());
  ASSERT_NE(other, nullptr);

  ASSERT_EQ(h_query_insert(db_, "INSERT INTO t (i) VALUES (77)"), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(other, "SELECT i FROM t", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][0].t_data)->value, 77u);
  h_clean_result(&result);

  h_close_db(other);
  h_clean_connection(other);
}

TEST_F(DbSqliteTest, MissingTableIsAnError) {
  struct _h_result result = {0};
  EXPECT_EQ(h_query_select(db_, "SELECT * FROM no_such_table", &result), H_ERROR_QUERY);
}

TEST_F(DbSqliteTest, SelectWithNullResultStillExecutes) {
  EXPECT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (9)", nullptr, H_OPTION_SELECT), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);
}

TEST_F(DbSqliteTest, MultiStatementBatchIsAtomicByDefault) {
  EXPECT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); garbage", nullptr, H_OPTION_EXEC),
            H_ERROR_QUERY);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);
}

TEST_F(DbSqliteTest, MultiStatementBatchOptsOutOfAtomicity) {
  EXPECT_EQ(h_execute_query_ex(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); garbage", nullptr, H_OPTION_EXEC, false),
            H_ERROR_QUERY);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 2u);
}

TEST_F(DbSqliteTest, AtomicSelectRollsBackItsOwnDdl) {
  struct _h_result result = {0};

  EXPECT_EQ(h_execute_query(db_, "CREATE TABLE z (id INTEGER); INSERT INTO z VALUES (1); garbage", &result, H_OPTION_SELECT),
            H_ERROR_QUERY);

  struct _h_result probe = {0};
  EXPECT_EQ(h_query_select(db_, "SELECT count(*) FROM z", &probe), H_ERROR_QUERY);
  if (probe.data != nullptr) h_clean_result(&probe);
}

TEST_F(DbSqliteTest, CollectNotAtomicKeepsPartialResults) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  EXPECT_EQ(h_execute_query_collect_ex(db_, "SELECT 1 AS a; SELECT * FROM no_such_table; SELECT 3 AS c", &results, &failed, false),
            H_ERROR_QUERY);
  EXPECT_EQ(failed, 1u);
  ASSERT_EQ(results.collection_sz, 1u);
  EXPECT_EQ(((struct _h_type_int *) AS_H_RESULT(results.collection[0])->data[0][0].t_data)->value, 1u);
  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectOptsOutAndKeepsInsert) {
  CollectionDescriptor results = {0};

  EXPECT_EQ(h_execute_query_collect_ex(db_, "INSERT INTO t (i) VALUES (7); garbage", &results, nullptr, false), H_ERROR_QUERY);
  EXPECT_EQ(results.collection_sz, 1u);
  h_clean_results(&results);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);
}

TEST_F(DbSqliteTest, AtomicCollectRollsBackItsInserts) {
  CollectionDescriptor results = {0};

  EXPECT_EQ(h_execute_query_collect(db_, "INSERT INTO t (i) VALUES (7); garbage", &results, nullptr), H_ERROR_QUERY);
  EXPECT_EQ(results.collection_sz, 0u);
  h_clean_results(&results);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);
}

TEST_F(DbSqliteTest, DefaultBatchIsAtomicForPlainExec) {
  EXPECT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); INSERT INTO t (i) VALUES (3)",
                            nullptr, H_OPTION_EXEC), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 3u);
}

TEST_F(DbSqliteTest, NegativeIntegerRoundTrips) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (-1)", nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT i FROM t", &result), H_OK);
  auto *value = (struct _h_type_int *) result.data[0][0].t_data;
  EXPECT_EQ(value->value, -1);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, SignedIntegerBoundariesRoundTrip) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (-9223372036854775808)", nullptr, H_OPTION_EXEC), H_OK);
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (9223372036854775807)", nullptr, H_OPTION_EXEC), H_OK);
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (-1)", nullptr, H_OPTION_EXEC), H_OK);
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (0)", nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT i FROM t ORDER BY id", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 4u);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][0].t_data)->value, INT64_MIN);
  EXPECT_EQ(((struct _h_type_int *) result.data[1][0].t_data)->value, INT64_MAX);
  EXPECT_EQ(((struct _h_type_int *) result.data[2][0].t_data)->value, -1);
  EXPECT_EQ(((struct _h_type_int *) result.data[3][0].t_data)->value, 0);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, SignedIntegerAggregatesAreSigned) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (-5); INSERT INTO t (i) VALUES (-7); INSERT INTO t (i) VALUES (3)",
                            nullptr, H_OPTION_EXEC), H_OK);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT sum(i), min(i), max(i) FROM t", &result), H_OK);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][0].t_data)->value, -9);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][1].t_data)->value, -7);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][2].t_data)->value, 3);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, SignedIntegerSurvivesCollect) {
  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, "INSERT INTO t (i) VALUES (-42); SELECT i FROM t", &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 2u);
  EXPECT_EQ(((struct _h_type_int *) AS_H_RESULT(results.collection[1])->data[0][0].t_data)->value, -42);
  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectReturnsOneResultPerStatement) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  ASSERT_EQ(h_execute_query_collect(db_, "INSERT INTO t (i) VALUES (1); SELECT 1 AS a; SELECT 2 AS b", &results, &failed), H_OK);
  ASSERT_EQ(results.collection_sz, 3u);
  EXPECT_EQ(failed, 0u);

  struct _h_result *insert = AS_H_RESULT(results.collection[0]);
  EXPECT_EQ(insert->nb_rows, 0u);
  EXPECT_EQ(insert->nb_columns, 0u);

  struct _h_result *first = AS_H_RESULT(results.collection[1]);
  ASSERT_EQ(first->nb_rows, 1u);
  EXPECT_EQ(((struct _h_type_int *) first->data[0][0].t_data)->value, 1u);

  struct _h_result *second = AS_H_RESULT(results.collection[2]);
  ASSERT_EQ(second->nb_rows, 1u);
  EXPECT_EQ(((struct _h_type_int *) second->data[0][0].t_data)->value, 2u);

  h_clean_results(&results);
  EXPECT_EQ(results.collection, nullptr);
  EXPECT_EQ(results.collection_sz, 0u);
}

TEST_F(DbSqliteTest, CollectEmptyQueryYieldsNoResults) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  EXPECT_EQ(h_execute_query_collect(db_, "", &results, &failed), H_OK);
  EXPECT_EQ(results.collection_sz, 0u);
  EXPECT_EQ(failed, 0u);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectCommentsOnlyYieldsNoResults) {
  CollectionDescriptor results = {0};

  EXPECT_EQ(h_execute_query_collect(db_, "-- nothing\n/* nor here */", &results, nullptr), H_OK);
  EXPECT_EQ(results.collection_sz, 0u);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectReportsFailedStatementIndex) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  EXPECT_EQ(h_execute_query_collect(db_, "SELECT 1 AS a; SELECT * FROM no_such_table; SELECT 3 AS c", &results, &failed),
            H_ERROR_QUERY);
  EXPECT_EQ(failed, 1u);
  EXPECT_EQ(results.collection_sz, 0u);
  EXPECT_EQ(results.collection, nullptr);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectLeavesNothingBehindAfterFailure) {
  CollectionDescriptor results = {0};

  EXPECT_EQ(h_execute_query_collect(db_, "INSERT INTO t (i) VALUES (1); garbage", &results, nullptr), H_ERROR_QUERY);
  EXPECT_EQ(results.collection_sz, 0u);

  h_clean_results(&results);
  EXPECT_EQ(results.collection, nullptr);
  EXPECT_EQ(results.collection_sz, 0u);
}

TEST_F(DbSqliteTest, CollectRejectsNullResults) {
  size_t failed = 0;

  EXPECT_EQ(h_execute_query_collect(db_, "SELECT 1", nullptr, &failed), H_ERROR_PARAMS);
}

TEST_F(DbSqliteTest, CollectDdlAndDmlCarryNoColumns) {
  CollectionDescriptor results = {0};

  ASSERT_EQ(h_execute_query_collect(db_, "CREATE TABLE v (id INTEGER); INSERT INTO v VALUES (1); UPDATE v SET id = 2; DELETE FROM v",
                                    &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 4u);

  for (size_t i = 0; i < results.collection_sz; i++) {
    struct _h_result *entry = AS_H_RESULT(results.collection[i]);
    EXPECT_EQ(entry->nb_columns, 0u);
    EXPECT_EQ(entry->nb_rows, 0u);
    EXPECT_EQ(entry->data, nullptr);
  }

  h_clean_results(&results);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM v"), 0u);
}

TEST_F(DbSqliteTest, SelectLeavesResultEmptyOnFailure) {
  struct _h_result result = {0};

  EXPECT_EQ(h_query_select(db_, "SELECT 1 AS a; garbage", &result), H_ERROR_QUERY);
  EXPECT_EQ(result.nb_rows, 0u);
  EXPECT_EQ(result.nb_columns, 0u);
  EXPECT_EQ(result.data, nullptr);
}

TEST_F(DbSqliteTest, CollectReturnsEveryRowOfEverySelect) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); INSERT INTO t (i) VALUES (3)", nullptr, H_OPTION_EXEC), H_OK);

  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, "SELECT i FROM t ORDER BY i; SELECT i FROM t ORDER BY i DESC", &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 2u);

  struct _h_result *ascending = AS_H_RESULT(results.collection[0]);
  ASSERT_EQ(ascending->nb_rows, 3u);
  for (unsigned int row = 0; row < 3; row++) {
    EXPECT_EQ(((struct _h_type_int *) ascending->data[row][0].t_data)->value, static_cast<uint64_t>(row + 1));
  }

  struct _h_result *descending = AS_H_RESULT(results.collection[1]);
  ASSERT_EQ(descending->nb_rows, 3u);
  for (unsigned int row = 0; row < 3; row++) {
    EXPECT_EQ(((struct _h_type_int *) descending->data[row][0].t_data)->value, static_cast<uint64_t>(3 - row));
  }

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectKeepsColumnCountPerEntry) {
  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, "SELECT 1 AS a; SELECT 1 AS a, 2 AS b, 3 AS c; SELECT 4 AS d", &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 3u);

  EXPECT_EQ(AS_H_RESULT(results.collection[0])->nb_columns, 1u);
  EXPECT_EQ(AS_H_RESULT(results.collection[1])->nb_columns, 3u);
  EXPECT_EQ(AS_H_RESULT(results.collection[2])->nb_columns, 1u);

  struct _h_result *wide = AS_H_RESULT(results.collection[1]);
  ASSERT_EQ(wide->nb_rows, 1u);
  EXPECT_EQ(((struct _h_type_int *) wide->data[0][0].t_data)->value, 1u);
  EXPECT_EQ(((struct _h_type_int *) wide->data[0][1].t_data)->value, 2u);
  EXPECT_EQ(((struct _h_type_int *) wide->data[0][2].t_data)->value, 3u);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectMixesRowCountsAcrossStatements) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (10); INSERT INTO t (i) VALUES (20)", nullptr, H_OPTION_EXEC), H_OK);

  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, "SELECT i FROM t ORDER BY i; SELECT 99 AS one; SELECT i FROM t WHERE 0", &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 3u);

  EXPECT_EQ(AS_H_RESULT(results.collection[0])->nb_rows, 2u);
  EXPECT_EQ(AS_H_RESULT(results.collection[1])->nb_rows, 1u);
  EXPECT_EQ(AS_H_RESULT(results.collection[2])->nb_rows, 0u);
  EXPECT_EQ(AS_H_RESULT(results.collection[2])->nb_columns, 1u);
  EXPECT_EQ(AS_H_RESULT(results.collection[2])->data, nullptr);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CleanResultsHandlesMixedEntriesAndIsIdempotent) {
  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, "INSERT INTO t (i) VALUES (1); SELECT i FROM t; SELECT 'x' AS s", &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 3u);
  EXPECT_EQ(AS_H_RESULT(results.collection[0])->data, nullptr);

  h_clean_results(&results);
  EXPECT_EQ(results.collection, nullptr);
  EXPECT_EQ(results.collection_sz, 0u);

  h_clean_results(&results);
  EXPECT_EQ(results.collection, nullptr);
  EXPECT_EQ(results.collection_sz, 0u);
}

TEST_F(DbSqliteTest, CollectFailsOnFirstStatement) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  EXPECT_EQ(h_execute_query_collect(db_, "SELECT * FROM no_such_table; SELECT 1", &results, &failed), H_ERROR_QUERY);
  EXPECT_EQ(failed, 0u);
  EXPECT_EQ(results.collection_sz, 0u);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectFailsOnLastStatement) {
  CollectionDescriptor results = {0};
  size_t failed = 99;

  EXPECT_EQ(h_execute_query_collect(db_, "SELECT 1 AS a; SELECT 2 AS b; garbage", &results, &failed), H_ERROR_QUERY);
  EXPECT_EQ(failed, 2u);
  EXPECT_EQ(results.collection_sz, 0u);

  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectRejectsNullQuery) {
  CollectionDescriptor results = {0};
  EXPECT_EQ(h_execute_query_collect(db_, nullptr, &results, nullptr), H_ERROR_PARAMS);
}

TEST_F(DbSqliteTest, CollectToleratesNullConnection) {
  CollectionDescriptor results = {0};
  EXPECT_EQ(h_execute_query_collect(nullptr, "SELECT 1", &results, nullptr), H_ERROR_PARAMS);
}

TEST_F(DbSqliteTest, CollectStoresInjectionAttemptInQuotedValueOnly) {
  char *escaped = h_escape_string(db_, "'; DROP TABLE t; --");
  ASSERT_NE(escaped, nullptr);
  std::string sql = std::string("INSERT INTO t (s) VALUES ('") + escaped + "'); SELECT count(*) FROM t";
  free(escaped);

  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, sql.c_str(), &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 2u);
  EXPECT_EQ(((struct _h_type_int *) AS_H_RESULT(results.collection[1])->data[0][0].t_data)->value, 1u);
  h_clean_results(&results);

  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);
}

TEST_F(DbSqliteTest, CollectRoundTripsUtf8AndLongText) {
  const std::string unicode = "δокер 漢字 emoji \xF0\x9F\x98\x80";
  const std::string long_text(4096, 'z');
  char *escaped_unicode = h_escape_string(db_, unicode.c_str());
  char *escaped_long = h_escape_string(db_, long_text.c_str());
  ASSERT_NE(escaped_unicode, nullptr);
  ASSERT_NE(escaped_long, nullptr);

  std::string sql = std::string("INSERT INTO t (s) VALUES ('") + escaped_unicode + "'); INSERT INTO t (s) VALUES ('" +
                    escaped_long + "'); SELECT s FROM t ORDER BY id";
  free(escaped_unicode);
  free(escaped_long);

  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, sql.c_str(), &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 3u);

  struct _h_result *rows = AS_H_RESULT(results.collection[2]);
  ASSERT_EQ(rows->nb_rows, 2u);
  auto *u = (struct _h_type_text *) rows->data[0][0].t_data;
  auto *l = (struct _h_type_text *) rows->data[1][0].t_data;
  EXPECT_EQ(std::string(u->value, u->length), unicode);
  EXPECT_EQ(std::string(l->value, l->length), long_text);
  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectHandlesThousandStatementsWithResults) {
  std::string sql;
  for (int i = 0; i < 500; i++) {
    sql += "INSERT INTO t (i) VALUES (" + std::to_string(i) + ");";
  }
  sql += "SELECT count(*) FROM t; SELECT max(i) FROM t";

  CollectionDescriptor results = {0};
  ASSERT_EQ(h_execute_query_collect(db_, sql.c_str(), &results, nullptr), H_OK);
  ASSERT_EQ(results.collection_sz, 502u);
  EXPECT_EQ(((struct _h_type_int *) AS_H_RESULT(results.collection[500])->data[0][0].t_data)->value, 500u);
  EXPECT_EQ(((struct _h_type_int *) AS_H_RESULT(results.collection[501])->data[0][0].t_data)->value, 499u);
  h_clean_results(&results);
}

TEST_F(DbSqliteTest, CollectRejectsMalformedQueryWithoutCrashing) {
  CollectionDescriptor results = {0};

  EXPECT_EQ(h_execute_query_collect(db_, "SELEC 1", &results, nullptr), H_ERROR_QUERY);
  EXPECT_EQ(results.collection_sz, 0u);
  h_clean_results(&results);
}

TEST_F(DbSqliteTest, MultiRowSelectSurvivesTrailingJunkInSingleResultForm) {
  struct _h_result result = {0};

  EXPECT_EQ(h_query_select(db_, "SELECT 1 AS a; SELECT 2 AS b; garbage", &result), H_ERROR_QUERY);
  EXPECT_EQ(result.nb_rows, 0u);
  EXPECT_EQ(result.data, nullptr);
}

TEST_F(DbSqliteTest, ExplicitTransactionControlRequiresNonAtomicBatch) {
  EXPECT_EQ(h_execute_query(db_, "BEGIN; INSERT INTO t (i) VALUES (1); COMMIT", nullptr, H_OPTION_EXEC), H_ERROR_QUERY);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);

  EXPECT_EQ(h_execute_query_ex(db_, "BEGIN; INSERT INTO t (i) VALUES (1); COMMIT", nullptr, H_OPTION_EXEC, false), H_OK);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);
}

TEST_F(DbSqliteTest, BoundParametersRoundTripEveryType) {
  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (s, i, d, b) VALUES (?, ?, ?, ?)");
  ASSERT_NE(insert, nullptr);
  ASSERT_EQ(UfDbStatementBindText(insert, 1, "text", 4), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 2, -7), H_OK);
  ASSERT_EQ(UfDbStatementBindDouble(insert, 3, 2.5), H_OK);
  const unsigned char blob[] = {0x00, 0xff, 0x10};
  ASSERT_EQ(UfDbStatementBindBlob(insert, 4, blob, sizeof blob), H_OK);

  bool has_row = true;
  ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);
  EXPECT_FALSE(has_row);
  UfDbStatementFinalize(insert);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s, i, d, b FROM t", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_STREQ(((struct _h_type_text *) result.data[0][0].t_data)->value, "text");
  EXPECT_EQ(((struct _h_type_int *) result.data[0][1].t_data)->value, -7);
  EXPECT_DOUBLE_EQ(((struct _h_type_double *) result.data[0][2].t_data)->value, 2.5);
  EXPECT_EQ(((struct _h_type_blob *) result.data[0][3].t_data)->length, sizeof blob);
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, BoundTextCannotExtendTheStatement) {
  const char *hostile = "x'); DROP TABLE t; --";
  const size_t hostile_length = strlen(hostile);

  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (s) VALUES (?)");
  ASSERT_NE(insert, nullptr);
  ASSERT_EQ(UfDbStatementBindText(insert, 1, hostile, hostile_length), H_OK);

  bool has_row = true;
  ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);
  UfDbStatementFinalize(insert);

  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 1u);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s FROM t", &result), H_OK);
  auto *text = (struct _h_type_text *) result.data[0][0].t_data;
  EXPECT_EQ(std::string(text->value, text->length), std::string(hostile));
  h_clean_result(&result);
}

TEST_F(DbSqliteTest, StatementRefusesASecondStatement) {
  EXPECT_EQ(UfDbStatementPrepare(db_, "SELECT 1; DROP TABLE t"), nullptr);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);

  EXPECT_EQ(UfDbStatementPrepare(db_, "SELECT 1; garbage"), nullptr);

  UfDbStatement *plain = UfDbStatementPrepare(db_, "SELECT 1;   ");
  ASSERT_NE(plain, nullptr);
  UfDbStatementFinalize(plain);
}

TEST_F(DbSqliteTest, StatementReuseWithResetAndClearBindings) {
  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (i) VALUES (?)");
  ASSERT_NE(insert, nullptr);

  for (int value = 1; value <= 3; value++) {
    ASSERT_EQ(UfDbStatementClearBindings(insert), H_OK);
    ASSERT_EQ(UfDbStatementBindInt(insert, 1, value), H_OK);

    bool has_row = true;
    ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);
    ASSERT_EQ(UfDbStatementReset(insert), H_OK);
  }

  UfDbStatementFinalize(insert);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 3u);
}

TEST_F(DbSqliteTest, StatementStepsThroughEveryRow) {
  ASSERT_EQ(h_execute_query(db_, "INSERT INTO t (i) VALUES (1); INSERT INTO t (i) VALUES (2); INSERT INTO t (i) VALUES (3)",
                            nullptr, H_OPTION_EXEC), H_OK);

  UfDbStatement *select = UfDbStatementPrepare(db_, "SELECT i FROM t ORDER BY i");
  ASSERT_NE(select, nullptr);

  int64_t expected = 1;
  bool has_row = false;
  while (true) {
    ASSERT_EQ(UfDbStatementStep(select, &has_row), H_OK);
    if (!has_row) break;

    struct _h_data *data = UfDbStatementColumn(select, 0);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->type, HOEL_COL_TYPE_INT);
    EXPECT_EQ(((struct _h_type_int *) data->t_data)->value, expected++);
    h_clean_data_full(data);
  }

  EXPECT_EQ(expected, 4);
  UfDbStatementFinalize(select);
}

TEST_F(DbSqliteTest, StatementBindRejectsOutOfRangeIndex) {
  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (i) VALUES (?)");
  ASSERT_NE(insert, nullptr);

  EXPECT_EQ(UfDbStatementBindInt(insert, 0, 1), H_ERROR_PARAMS);
  EXPECT_EQ(UfDbStatementBindInt(insert, 2, 1), H_ERROR_PARAMS);
  EXPECT_EQ(UfDbStatementBindText(insert, 2, "x", 1), H_ERROR_PARAMS);

  UfDbStatementFinalize(insert);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 0u);
}

TEST_F(DbSqliteTest, StatementStepReportsConstraintFailureAndRecovers) {
  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (id, i) VALUES (?, ?)");
  ASSERT_NE(insert, nullptr);

  ASSERT_EQ(UfDbStatementBindInt(insert, 1, 1), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 2, 10), H_OK);
  bool has_row = true;
  ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);

  ASSERT_EQ(UfDbStatementReset(insert), H_OK);
  ASSERT_EQ(UfDbStatementClearBindings(insert), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 1, 1), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 2, 20), H_OK);
  EXPECT_EQ(UfDbStatementStep(insert, &has_row), H_ERROR_QUERY);

  ASSERT_EQ(UfDbStatementReset(insert), H_OK);
  ASSERT_EQ(UfDbStatementClearBindings(insert), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 1, 2), H_OK);
  ASSERT_EQ(UfDbStatementBindInt(insert, 2, 30), H_OK);
  EXPECT_EQ(UfDbStatementStep(insert, &has_row), H_OK);

  UfDbStatementFinalize(insert);
  EXPECT_EQ(QueryInt("SELECT count(*) FROM t"), 2u);
}

TEST_F(DbSqliteTest, StatementPrepareRejectsGarbageAndNullFinalize) {
  EXPECT_EQ(UfDbStatementPrepare(db_, "SELEC 1"), nullptr);
  EXPECT_EQ(UfDbStatementPrepare(db_, ""), nullptr);

  UfDbStatementFinalize(nullptr);
}

TEST_F(DbSqliteTest, StatementBindTextPreservesEmbeddedNulLength) {
  const char value[] = {'a', '\0', 'b'};

  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (s) VALUES (?)");
  ASSERT_NE(insert, nullptr);
  ASSERT_EQ(UfDbStatementBindText(insert, 1, value, sizeof value), H_OK);

  bool has_row = true;
  ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);
  UfDbStatementFinalize(insert);

  UfDbStatement *select = UfDbStatementPrepare(db_, "SELECT length(s), s FROM t");
  ASSERT_NE(select, nullptr);
  ASSERT_EQ(UfDbStatementStep(select, &has_row), H_OK);
  ASSERT_TRUE(has_row);

  struct _h_data *length = UfDbStatementColumn(select, 0);
  struct _h_data *text = UfDbStatementColumn(select, 1);
  ASSERT_NE(length, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(((struct _h_type_int *) length->t_data)->value, 1);
  EXPECT_EQ(((struct _h_type_text *) text->t_data)->length, 3u);
  EXPECT_EQ(std::string(((struct _h_type_text *) text->t_data)->value, 3), std::string(value, 3));
  h_clean_data_full(length);
  h_clean_data_full(text);

  UfDbStatementFinalize(select);
}

TEST_F(DbSqliteTest, StatementBindNullStoresNull) {
  UfDbStatement *insert = UfDbStatementPrepare(db_, "INSERT INTO t (s) VALUES (?)");
  ASSERT_NE(insert, nullptr);
  ASSERT_EQ(UfDbStatementBindNull(insert, 1), H_OK);

  bool has_row = true;
  ASSERT_EQ(UfDbStatementStep(insert, &has_row), H_OK);
  UfDbStatementFinalize(insert);

  struct _h_result result = {0};
  ASSERT_EQ(h_query_select(db_, "SELECT s, s IS NULL FROM t", &result), H_OK);
  EXPECT_EQ(result.data[0][0].type, HOEL_COL_TYPE_NULL);
  EXPECT_EQ(((struct _h_type_int *) result.data[0][1].t_data)->value, 1);
  h_clean_result(&result);
}

}
