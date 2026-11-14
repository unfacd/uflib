#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" {
#include <uflib/db/db_sql.h>
}

namespace {

class MigrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = ::testing::TempDir() + "ufsrv_db_migration_" + std::to_string(getpid()) + "_" +
            std::to_string(counter_++) + ".sqlite";
    std::remove(path_.c_str());

    db_ = h_connect_sqlite(path_.c_str());
    ASSERT_NE(db_, nullptr);
  }

  void TearDown() override {
    if (db_ != nullptr) {
      h_close_db(db_);
      h_clean_connection(db_);
      db_ = nullptr;
    }
    std::remove(path_.c_str());
  }

  int ScalarInt(const char *sql) {
    struct _h_result result = {0};
    int value = -1;

    EXPECT_EQ(h_select_query_sqlite(db_, sql, &result), H_OK);
    if (result.data != nullptr && result.nb_rows > 0 && result.data[0][0].type == HOEL_COL_TYPE_INT) {
      value = (int)((struct _h_type_int *)result.data[0][0].t_data)->value;
    }
    if (result.data != nullptr) h_clean_result(&result);

    return value;
  }

  bool TableExists(const char *name) {
    char sql[256];

    snprintf(sql, sizeof sql, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'", name);

    return ScalarInt(sql) == 1;
  }

  std::string LastError() {
    char buffer[512] = {0};

    if (h_last_error_message(db_, buffer, sizeof buffer) != H_OK) return "<unavailable>";

    return buffer;
  }

  std::string path_;
  struct _h_connection *db_ = nullptr;
  static int counter_;
};

int MigrationTest::counter_ = 0;

int InsertMigratedRow(struct _h_connection *conn, void *context) {
  auto *marker = static_cast<int *>(context);

  *marker += 1;

  return h_execute_query_ex(conn, "INSERT INTO migrated(id, v) VALUES(1, 'from-callback')", nullptr, H_OPTION_EXEC,
                            false) == H_OK
             ? H_OK
             : H_ERROR_QUERY;
}

int FailTheStep(struct _h_connection *conn, void *context) {
  (void)conn;
  (void)context;

  return H_ERROR_QUERY;
}

int CreateTableInTheCallback(struct _h_connection *conn, void *context) {
  (void)context;

  return h_execute_query_ex(conn, "CREATE TABLE from_callback(id INTEGER PRIMARY KEY)", nullptr, H_OPTION_EXEC,
                            false) == H_OK
             ? H_OK
             : H_ERROR_QUERY;
}

}  // namespace

TEST_F(MigrationTest, AppliesTheSqlAndStampsTheVersion) {
  ASSERT_EQ(h_execute_migration(db_, "CREATE TABLE migrated(id INTEGER PRIMARY KEY, v TEXT);", nullptr, nullptr, 2,
                                nullptr),
            H_OK);
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 2);
  EXPECT_TRUE(TableExists("migrated"));
}

TEST_F(MigrationTest, StampsEachStepAtItsOwnVersion) {
  ASSERT_EQ(h_execute_migration(db_, "CREATE TABLE one(id INTEGER)", nullptr, nullptr, 1, nullptr), H_OK);
  ASSERT_EQ(ScalarInt("PRAGMA user_version"), 1);

  ASSERT_EQ(h_execute_migration(db_, "CREATE TABLE two(id INTEGER)", nullptr, nullptr, 5, nullptr), H_OK);
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 5);
  EXPECT_TRUE(TableExists("one"));
  EXPECT_TRUE(TableExists("two"));
}

TEST_F(MigrationTest, RollsBackEveryStatementWhenOneFails) {
  const char *sql = "CREATE TABLE first(id INTEGER);\nCREATE TABLE second(id INTEGER;\n";

  EXPECT_EQ(h_execute_migration(db_, sql, nullptr, nullptr, 3, nullptr), H_ERROR_QUERY);
  EXPECT_FALSE(TableExists("first"));
  EXPECT_FALSE(TableExists("second"));
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 0);
}

TEST_F(MigrationTest, ReportsTheOffsetOfTheFailingStatement) {
  const char *sql     = "CREATE TABLE first(id INTEGER);\nCREATE TABLE second(id INTEGER;\n";
  size_t      offset  = 0;
  const char *failing = strstr(sql, "CREATE TABLE second");

  ASSERT_NE(h_execute_migration(db_, sql, nullptr, nullptr, 3, &offset), H_OK);
  EXPECT_EQ(offset, (size_t)(failing - sql));
}

TEST_F(MigrationTest, ReportsTheSqliteErrorMessageOfTheFailingStatement) {
  ASSERT_NE(h_execute_migration(db_, "CREATE TABLE broken(", nullptr, nullptr, 1, nullptr), H_OK);
  EXPECT_NE(LastError().find("error"), std::string::npos) << LastError();
}

TEST_F(MigrationTest, RollsBackTheSqlWhenTheStepCallbackFails) {
  EXPECT_EQ(h_execute_migration(db_, "CREATE TABLE migrated(id INTEGER PRIMARY KEY, v TEXT)", FailTheStep, nullptr, 1,
                                nullptr),
            H_ERROR_QUERY);
  EXPECT_FALSE(TableExists("migrated"));
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 0);
}

TEST_F(MigrationTest, RunsTheStepCallbackInsideTheTransaction) {
  int marker = 0;

  ASSERT_EQ(h_execute_migration(db_, "CREATE TABLE migrated(id INTEGER PRIMARY KEY, v TEXT)", InsertMigratedRow,
                                &marker, 4, nullptr),
            H_OK);
  EXPECT_EQ(marker, 1);

  struct _h_result result = {0};

  ASSERT_EQ(h_select_query_sqlite(db_, "SELECT v FROM migrated WHERE id = 1", &result), H_OK);
  ASSERT_EQ(result.nb_rows, 1u);
  EXPECT_NE(result.data[0][0].t_data, nullptr);
  h_clean_result(&result);
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 4);
}

TEST_F(MigrationTest, AcceptsAStepWithNoSql) {
  ASSERT_EQ(h_execute_migration(db_, nullptr, CreateTableInTheCallback, nullptr, 6, nullptr), H_OK);
  EXPECT_TRUE(TableExists("from_callback"));
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 6);
}

TEST_F(MigrationTest, RollsBackWhenTheForeignKeysLeftBehindAreBroken) {
  const char *sql =
      "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
      "CREATE TABLE child(id INTEGER PRIMARY KEY, p INTEGER REFERENCES parent(id));"
      "INSERT INTO child(id, p) VALUES(1, 999);";
  size_t offset = 1;

  EXPECT_EQ(h_execute_migration(db_, sql, nullptr, nullptr, 2, &offset), H_ERROR_QUERY);
  EXPECT_EQ(offset, 0u) << "the statement must run with foreign keys off; only the check may fail";
  EXPECT_FALSE(TableExists("parent"));
  EXPECT_FALSE(TableExists("child"));
  EXPECT_EQ(ScalarInt("PRAGMA user_version"), 0);
}

TEST_F(MigrationTest, RestoresForeignKeysAfterSuccessAndAfterFailure) {
  ASSERT_EQ(h_execute_migration(db_, "CREATE TABLE ok(id INTEGER)", nullptr, nullptr, 1, nullptr), H_OK);
  EXPECT_EQ(ScalarInt("PRAGMA foreign_keys"), 1);

  ASSERT_NE(h_execute_migration(db_, "CREATE TABLE broken(", nullptr, nullptr, 2, nullptr), H_OK);
  EXPECT_EQ(ScalarInt("PRAGMA foreign_keys"), 1);

  ASSERT_NE(h_execute_migration(db_, "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
                                     "CREATE TABLE child(id INTEGER PRIMARY KEY, p INTEGER REFERENCES parent(id));"
                                     "INSERT INTO child(id, p) VALUES(1, 999);",
                                nullptr, nullptr, 3, nullptr),
            H_OK);
  EXPECT_EQ(ScalarInt("PRAGMA foreign_keys"), 1);
}

TEST_F(MigrationTest, RejectsANegativeVersion) {
  EXPECT_EQ(h_execute_migration(db_, "CREATE TABLE refused(id INTEGER)", nullptr, nullptr, -1, nullptr),
            H_ERROR_PARAMS);
  EXPECT_FALSE(TableExists("refused"));
}

TEST_F(MigrationTest, RejectsAMissingConnection) {
  EXPECT_EQ(h_execute_migration(nullptr, "CREATE TABLE refused(id INTEGER)", nullptr, nullptr, 1, nullptr),
            H_ERROR_PARAMS);
}

TEST_F(MigrationTest, LastErrorMessageRejectsMisuse) {
  char buffer[64] = {0};

  EXPECT_EQ(h_last_error_message(nullptr, buffer, sizeof buffer), H_ERROR_PARAMS);
  EXPECT_EQ(h_last_error_message(db_, nullptr, sizeof buffer), H_ERROR_PARAMS);
  EXPECT_EQ(h_last_error_message(db_, buffer, 0), H_ERROR_PARAMS);
}
