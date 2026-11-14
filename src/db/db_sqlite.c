#include <config_uflib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <uflib/standard_defs.h>
#include <sqlite3.h>
#include <uflib/db/db_sql.h>

struct _h_sqlite {
  sqlite3 * db_handle;
};

static int sBeginTransaction(struct _h_sqlite *sqlite)
{
  if (sqlite3_exec(sqlite->db_handle, "BEGIN", NULL, NULL, NULL) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BEGIN TRANSACTION. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));

  return H_ERROR_QUERY;
}

static int sEndTransaction(struct _h_sqlite *sqlite, bool commit)
{
  const char *sql = commit ? "COMMIT" : "ROLLBACK";

  if (sqlite3_exec(sqlite->db_handle, sql, NULL, NULL, NULL) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT %s TRANSACTION. ERROR: '%s'", __func__, commit ? "COMMIT" : "ROLLBACK",
         sqlite3_errmsg(sqlite->db_handle));

  return H_ERROR_QUERY;
}

static struct _h_data *sColumnData(sqlite3_stmt *stmt, int col)
{
  switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_INTEGER:
      return h_new_data_int(sqlite3_column_int64(stmt, col));
    case SQLITE_FLOAT:
      return h_new_data_double(sqlite3_column_double(stmt, col));
    case SQLITE_BLOB:
      return h_new_data_blob(sqlite3_column_blob(stmt, col), (size_t) sqlite3_column_bytes(stmt, col));
    case SQLITE_TEXT:
      return h_new_data_text((const char *) sqlite3_column_text(stmt, col), (size_t) sqlite3_column_bytes(stmt, col));
    case SQLITE_NULL:
      return h_new_data_null();
    default:
      return NULL;
  }
}

static int sMaterialiseStatement(sqlite3_stmt *stmt, struct _h_result *result)
{
  struct _h_data *data = NULL, *cur_row = NULL;
  int nb_columns = sqlite3_column_count(stmt);
  int col, row, res, step_result;

  result->nb_rows = 0;
  result->nb_columns = (unsigned int) nb_columns;
  result->data = NULL;

  step_result = sqlite3_step(stmt);

  for (row = 0; step_result == SQLITE_ROW; row++) {
    cur_row = NULL;

    for (col = 0; col < nb_columns; col++) {
      data = sColumnData(stmt, col);

      if (data == NULL) {
        syslog(LOG_ERR, "%s: Error marshalling column '%d' of row '%d'", __func__, col, row);
        return H_ERROR_MEMORY;
      }

      res = h_row_add_data(&cur_row, data, col);
      h_clean_data_full(data);

      if (res != H_OK) {
        syslog(LOG_ERR, "%s: Error adding column '%d' of row '%d'", __func__, col, row);
        return res;
      }
    }

    res = h_result_add_row(result, cur_row, row);
    if (res != H_OK) {
      syslog(LOG_ERR, "%s: Error adding row '%d'", __func__, row);
      return res;
    }

    step_result = sqlite3_step(stmt);
  }

  if (step_result != SQLITE_DONE) {
    syslog(LOG_ERR, "%s: Error stepping statement. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite3_db_handle(stmt)));
    return H_ERROR_QUERY;
  }

  return H_OK;
}

struct _h_connection *
h_connect_sqlite(const char * db_path)
{
  struct _h_connection *conn = NULL;

  if (db_path == NULL) {
    syslog(LOG_ERR, "%s: ERROR: NO DB PATH PROVIDED", __func__);
    return NULL;
  }

  conn = calloc(1, sizeof(struct _h_connection));
  if (conn == NULL) {
    syslog(LOG_ERR, "%s: Error allocating memory for conn", __func__);
    return NULL;
  }

  conn->type = HOEL_DB_TYPE_SQLITE;
  conn->connection = calloc(1, sizeof(struct _h_sqlite));
  if (conn->connection == NULL) {
    syslog(LOG_ERR, "%s: Error allocating memory for conn->connection", __func__);
    free(conn);
    return NULL;
  }

  if (sqlite3_open_v2(db_path, &((struct _h_sqlite *) conn->connection)->db_handle,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: ERROR: COULD NOT CONNECT TO DB BACKEND: '%s'. ERROR: '%s'", __func__, db_path,
           sqlite3_errmsg(((struct _h_sqlite *) conn->connection)->db_handle));
    sqlite3_close(((struct _h_sqlite *) conn->connection)->db_handle);
    free(conn->connection);
    free(conn);
    return NULL;
  }

  return conn;
}

void
h_close_sqlite(struct _h_connection * conn)
{
  sqlite3_close(((struct _h_sqlite *) conn->connection)->db_handle);
}

char *
h_escape_string_sqlite(const struct _h_connection * conn, const char * unsafe)
{
  char *tmp = sqlite3_mprintf("%q", unsafe);
  char *escaped = NULL;
  struct _h_sqlite *sqlite = (struct _h_sqlite *) conn->connection;

  if (tmp == NULL) {
    syslog(LOG_ERR, "%s: Error escaping string. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));
    return NULL;
  }

  escaped = strdup(tmp);
  sqlite3_free(tmp);

  if (escaped == NULL) {
    syslog(LOG_ERR, "%s: Error allocating memory for escaped", __func__);
  }

  return escaped;
}

int
h_last_insert_id_sqlite(const struct _h_connection * conn)
{
  return (int) sqlite3_last_insert_rowid(((struct _h_sqlite *) conn->connection)->db_handle);
}

int
h_exec_query_sqlite_ex(const struct _h_connection * conn, const char * query, bool is_atomic)
{
  struct _h_sqlite *sqlite = (struct _h_sqlite *) conn->connection;
  char *errmsg = NULL;

  if (is_atomic && sBeginTransaction(sqlite) != H_OK) {
    return H_ERROR_QUERY;
  }

  if (sqlite3_exec(sqlite->db_handle, query, NULL, NULL, &errmsg) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, query,
           errmsg != NULL ? errmsg : sqlite3_errmsg(sqlite->db_handle));
    sqlite3_free(errmsg);

    if (is_atomic) sEndTransaction(sqlite, false);

    return H_ERROR_QUERY;
  }

  sqlite3_free(errmsg);

  if (is_atomic && sEndTransaction(sqlite, true) != H_OK) {
    return H_ERROR_QUERY;
  }

  return H_OK;
}

int
h_exec_query_sqlite(const struct _h_connection * conn, const char * query)
{
  return h_exec_query_sqlite_ex(conn, query, true);
}

int
h_select_query_sqlite_ex(const struct _h_connection * conn, const char * query, struct _h_result * result, bool is_atomic)
{
  struct _h_sqlite *sqlite = (struct _h_sqlite *) conn->connection;
  struct _h_result collected = {0};
  const char *tail = query;
  sqlite3_stmt *stmt = NULL;

  if (is_atomic && sBeginTransaction(sqlite) != H_OK) {
    return H_ERROR_QUERY;
  }

  if (result != NULL) {
    result->nb_rows = 0;
    result->nb_columns = 0;
    result->data = NULL;
  }

  for (;;) {
    if (sqlite3_prepare_v2(sqlite->db_handle, tail, -1, &stmt, &tail) != SQLITE_OK) {
      syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, query, sqlite3_errmsg(sqlite->db_handle));
      h_clean_result(&collected);
      sqlite3_finalize(stmt);
      if (is_atomic) sEndTransaction(sqlite, false);
      return H_ERROR_QUERY;
    }

    if (stmt == NULL) {
      sqlite3_finalize(stmt);
      break;
    }

    if (result != NULL && sqlite3_column_count(stmt) > 0) {
      struct _h_result stmt_result = {0};
      int res = sMaterialiseStatement(stmt, &stmt_result);

      if (res != H_OK) {
        h_clean_result(&stmt_result);
        h_clean_result(&collected);
        sqlite3_finalize(stmt);
        if (is_atomic) sEndTransaction(sqlite, false);
        return res;
      }

      h_clean_result(&collected);
      collected = stmt_result;
    } else {
      int step_result = sqlite3_step(stmt);

      while (step_result == SQLITE_ROW) {
        step_result = sqlite3_step(stmt);
      }

      if (step_result != SQLITE_DONE) {
        syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, query, sqlite3_errmsg(sqlite->db_handle));
        h_clean_result(&collected);
        sqlite3_finalize(stmt);
        if (is_atomic) sEndTransaction(sqlite, false);
        return H_ERROR_QUERY;
      }
    }

    sqlite3_finalize(stmt);

    if (tail == NULL || *tail == '\0') {
      break;
    }
  }

  if (result != NULL) {
    *result = collected;
  } else {
    h_clean_result(&collected);
  }

  if (is_atomic && sEndTransaction(sqlite, true) != H_OK) {
    return H_ERROR_QUERY;
  }

  return H_OK;
}

int
h_select_query_sqlite(const struct _h_connection * conn, const char * query, struct _h_result * result)
{
  return h_select_query_sqlite_ex(conn, query, result, true);
}

int
h_execute_query_collect_sqlite_ex(const struct _h_connection * conn, const char * query,
                                  CollectionDescriptor * results, size_t * failed_index, bool is_atomic)
{
  struct _h_sqlite *sqlite = (struct _h_sqlite *) conn->connection;
  const char *tail = query;
  sqlite3_stmt *stmt = NULL;
  size_t index = 0;

  if (is_atomic && sBeginTransaction(sqlite) != H_OK) {
    return H_ERROR_QUERY;
  }

  for (;;) {
    if (sqlite3_prepare_v2(sqlite->db_handle, tail, -1, &stmt, &tail) != SQLITE_OK) {
      syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, query, sqlite3_errmsg(sqlite->db_handle));
      sqlite3_finalize(stmt);
      if (is_atomic) {
        sEndTransaction(sqlite, false);
        h_clean_results(results);
      }
      if (failed_index != NULL) *failed_index = index;
      return H_ERROR_QUERY;
    }

    if (stmt == NULL) {
      sqlite3_finalize(stmt);
      break;
    }

    struct _h_result *entry = calloc(1, sizeof(struct _h_result));
    if (entry == NULL) {
      syslog(LOG_ERR, "%s: Error allocating memory for result", __func__);
      sqlite3_finalize(stmt);
      if (is_atomic) {
        sEndTransaction(sqlite, false);
        h_clean_results(results);
      }
      if (failed_index != NULL) *failed_index = index;
      return H_ERROR_MEMORY;
    }

    int res = H_OK;

    if (sqlite3_column_count(stmt) > 0) {
      res = sMaterialiseStatement(stmt, entry);
    } else {
      int step_result = sqlite3_step(stmt);

      while (step_result == SQLITE_ROW) {
        step_result = sqlite3_step(stmt);
      }

      if (step_result != SQLITE_DONE) {
        syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, query, sqlite3_errmsg(sqlite->db_handle));
        res = H_ERROR_QUERY;
      }
    }

    if (res != H_OK) {
      h_clean_result(entry);
      free(entry);
      sqlite3_finalize(stmt);
      if (is_atomic) {
        sEndTransaction(sqlite, false);
        h_clean_results(results);
      }
      if (failed_index != NULL) *failed_index = index;
      return res;
    }

    collection_t **grown = realloc(results->collection, (results->collection_sz + 1) * sizeof(collection_t *));
    if (grown == NULL) {
      syslog(LOG_ERR, "%s: Error allocating memory for the result collection", __func__);
      h_clean_result(entry);
      free(entry);
      sqlite3_finalize(stmt);
      if (is_atomic) {
        sEndTransaction(sqlite, false);
        h_clean_results(results);
      }
      if (failed_index != NULL) *failed_index = index;
      return H_ERROR_MEMORY;
    }

    results->collection = grown;
    results->collection[results->collection_sz] = (collection_t *) entry;
    results->collection_sz++;

    sqlite3_finalize(stmt);

    if (tail == NULL || *tail == '\0') {
      break;
    }

    index++;
  }

  if (is_atomic && sEndTransaction(sqlite, true) != H_OK) {
    return H_ERROR_QUERY;
  }

  return H_OK;
}

int
h_execute_query_collect_sqlite(const struct _h_connection * conn, const char * query,
                               CollectionDescriptor * results, size_t * failed_index)
{
  return h_execute_query_collect_sqlite_ex(conn, query, results, failed_index, true);
}

struct UfDbStatement {
  sqlite3_stmt * stmt;
};

UfDbStatement *
UfDbStatementPrepare(struct _h_connection * conn, const char * sql_statement)
{
  struct _h_sqlite *sqlite = (struct _h_sqlite *) conn->connection;
  sqlite3_stmt *stmt = NULL;
  sqlite3_stmt *stacked = NULL;
  const char *tail = NULL;
  UfDbStatement *statement = NULL;

  if (sqlite3_prepare_v2(sqlite->db_handle, sql_statement, -1, &stmt, &tail) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR: '%s'. ERROR: '%s'", __func__, sql_statement, sqlite3_errmsg(sqlite->db_handle));
    sqlite3_finalize(stmt);
    return NULL;
  }

  if (stmt == NULL) {
    sqlite3_finalize(stmt);
    return NULL;
  }

  if (tail != NULL && *tail != '\0') {
    int tail_result = sqlite3_prepare_v2(sqlite->db_handle, tail, -1, &stacked, NULL);

    if (tail_result != SQLITE_OK || stacked != NULL) {
      syslog(LOG_ERR, "%s: STACKED STATEMENTS NOT SUPPORTED: '%s'", __func__, sql_statement);
      sqlite3_finalize(stacked);
      sqlite3_finalize(stmt);
      return NULL;
    }

    sqlite3_finalize(stacked);
  }

  statement = calloc(1, sizeof(UfDbStatement));
  if (statement == NULL) {
    syslog(LOG_ERR, "%s: Error allocating memory for the statement", __func__);
    sqlite3_finalize(stmt);
    return NULL;
  }

  statement->stmt = stmt;

  return statement;
}

int
UfDbStatementBindInt(UfDbStatement * statement, unsigned int index, int64_t value)
{
  if (sqlite3_bind_int64(statement->stmt, (int) index, value) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BIND INTEGER AT INDEX '%u'", __func__, index);

  return H_ERROR_PARAMS;
}

int
UfDbStatementBindDouble(UfDbStatement * statement, unsigned int index, double value)
{
  if (sqlite3_bind_double(statement->stmt, (int) index, value) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BIND DOUBLE AT INDEX '%u'", __func__, index);

  return H_ERROR_PARAMS;
}

int
UfDbStatementBindText(UfDbStatement * statement, unsigned int index, const char * value, size_t length)
{
  if (sqlite3_bind_text(statement->stmt, (int) index, value, (int) length, SQLITE_TRANSIENT) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BIND TEXT AT INDEX '%u'", __func__, index);

  return H_ERROR_PARAMS;
}

int
UfDbStatementBindBlob(UfDbStatement * statement, unsigned int index, const void * value, size_t length)
{
  if (sqlite3_bind_blob(statement->stmt, (int) index, value, (int) length, SQLITE_TRANSIENT) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BIND BLOB AT INDEX '%u'", __func__, index);

  return H_ERROR_PARAMS;
}

int
UfDbStatementBindNull(UfDbStatement * statement, unsigned int index)
{
  if (sqlite3_bind_null(statement->stmt, (int) index) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT BIND NULL AT INDEX '%u'", __func__, index);

  return H_ERROR_PARAMS;
}

int
UfDbStatementStep(UfDbStatement * statement, bool * has_row)
{
  int step_result = sqlite3_step(statement->stmt);

  if (has_row != NULL) *has_row = false;

  if (step_result == SQLITE_ROW) {
    if (has_row != NULL) *has_row = true;
    return H_OK;
  }

  if (step_result == SQLITE_DONE) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite3_db_handle(statement->stmt)));

  return H_ERROR_QUERY;
}

struct _h_data *
UfDbStatementColumn(UfDbStatement * statement, unsigned int index)
{
  return sColumnData(statement->stmt, (int) index);
}

int
UfDbStatementReset(UfDbStatement * statement)
{
  sqlite3_reset(statement->stmt);

  return H_OK;
}

int
UfDbStatementClearBindings(UfDbStatement * statement)
{
  if (sqlite3_clear_bindings(statement->stmt) == SQLITE_OK) {
    return H_OK;
  }

  syslog(LOG_ERR, "%s: COULD NOT CLEAR BINDINGS", __func__);

  return H_ERROR_QUERY;
}

void
UfDbStatementFinalize(UfDbStatement * statement)
{
  if (statement == NULL) {
    return;
  }

  sqlite3_finalize(statement->stmt);
  statement->stmt = NULL;
  free(statement);
}

static int
sRunMigrationStatements(struct _h_sqlite *sqlite, const char *query, size_t *failed_offset)
{
  const char *cursor;

  if (query == NULL) return H_OK;

  cursor = query;
  for (;;) {
    const char *statement_start;
    sqlite3_stmt *stmt = NULL;
    int step_result;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
    if (*cursor == '\0') break;

    statement_start = cursor;

    if (sqlite3_prepare_v2(sqlite->db_handle, cursor, -1, &stmt, &cursor) != SQLITE_OK) {
      if (failed_offset != NULL) *failed_offset = (size_t)(statement_start - query);
      syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR AT OFFSET '%zu'. ERROR: '%s'", __func__,
             (size_t)(statement_start - query), sqlite3_errmsg(sqlite->db_handle));
      return H_ERROR_QUERY;
    }

    if (stmt == NULL) break;

    step_result = sqlite3_step(stmt);
    while (step_result == SQLITE_ROW) {
      step_result = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    if (step_result != SQLITE_DONE) {
      if (failed_offset != NULL) *failed_offset = (size_t)(statement_start - query);
      syslog(LOG_ERR, "%s: DB BACKEND QUERY ERROR AT OFFSET '%zu'. ERROR: '%s'", __func__,
             (size_t)(statement_start - query), sqlite3_errmsg(sqlite->db_handle));
      return H_ERROR_QUERY;
    }
  }

  return H_OK;
}

static int
sCheckForeignKeys(struct _h_sqlite *sqlite)
{
  sqlite3_stmt *stmt = NULL;
  int status;

  if (sqlite3_prepare_v2(sqlite->db_handle, "PRAGMA foreign_key_check", -1, &stmt, NULL) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: COULD NOT RUN foreign_key_check. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));
    return H_ERROR_QUERY;
  }

  status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    syslog(LOG_ERR, "%s: THE MIGRATION LEAVES FOREIGN KEY VIOLATIONS BEHIND", __func__);
  }
  sqlite3_finalize(stmt);

  return status == SQLITE_DONE ? H_OK : H_ERROR_QUERY;
}

static int
sStampUserVersion(struct _h_sqlite *sqlite, int version)
{
  char statement[64];

  snprintf(statement, sizeof statement, "PRAGMA user_version = %d", version);

  if (sqlite3_exec(sqlite->db_handle, statement, NULL, NULL, NULL) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: COULD NOT SET THE SCHEMA VERSION. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));
    return H_ERROR_QUERY;
  }

  return H_OK;
}

int
h_execute_migration_sqlite(struct _h_connection *conn, const char *query, h_migration_step_fn step_fn, void *context,
                           int target_version, size_t *failed_offset)
{
  struct _h_sqlite *sqlite;
  int rc;

  if (failed_offset != NULL) *failed_offset = 0;
  if (conn == NULL || conn->connection == NULL || target_version < 0) return H_ERROR_PARAMS;

  sqlite = (struct _h_sqlite *) conn->connection;

  if (sqlite3_exec(sqlite->db_handle, "PRAGMA foreign_keys=OFF", NULL, NULL, NULL) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: COULD NOT DISABLE FOREIGN KEYS. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));
    return H_ERROR_QUERY;
  }

  if (sqlite3_exec(sqlite->db_handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    syslog(LOG_ERR, "%s: COULD NOT BEGIN THE MIGRATION TRANSACTION. ERROR: '%s'", __func__,
           sqlite3_errmsg(sqlite->db_handle));
    rc = H_ERROR_QUERY;
  } else {
    rc = sRunMigrationStatements(sqlite, query, failed_offset);

    if (rc == H_OK && step_fn != NULL) rc = step_fn(conn, context);
    if (rc == H_OK) rc = sCheckForeignKeys(sqlite);
    if (rc == H_OK) rc = sStampUserVersion(sqlite, target_version);
    if (rc == H_OK) rc = sEndTransaction(sqlite, true);
    if (rc != H_OK) sEndTransaction(sqlite, false);
  }

  if (sqlite3_exec(sqlite->db_handle, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != SQLITE_OK && rc == H_OK) {
    syslog(LOG_ERR, "%s: COULD NOT RESTORE FOREIGN KEYS. ERROR: '%s'", __func__, sqlite3_errmsg(sqlite->db_handle));
    rc = H_ERROR_QUERY;
  }

  return rc;
}

int
h_last_error_message_sqlite(const struct _h_connection *conn, char *out, size_t cap)
{
  const struct _h_sqlite *sqlite;

  if (conn == NULL || conn->connection == NULL || out == NULL || cap == 0) return H_ERROR_PARAMS;

  sqlite = (const struct _h_sqlite *) conn->connection;
  snprintf(out, cap, "%s", sqlite3_errmsg(sqlite->db_handle));

  return H_OK;
}
