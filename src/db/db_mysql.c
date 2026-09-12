/**
 * 
 * 
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#ifndef __USE_XOPEN
# define __USE_XOPEN //for strptime()
#endif

#include <time.h>
#include <syslog.h>
//#include <mariadb/my_global.h>//this redefine "likley/unlikely macros, so define above standard_defs to eliminate warning of redefs
#include <uflib/standard_defs.h>

#include <mariadb/mysql.h>
#include <uflib/db/db_sql.h>

/**
 * MariaDB handle
 */
struct _h_mariadb {
  char * host;
  char * user;
  char * passwd;
  char * db;
  unsigned int port;
  char * unix_socket;
  unsigned long flags;
  MYSQL * db_handle;
  //pthread_mutex_t lock;
};

void InitMysql(void)
{
	if (mysql_library_init(0, NULL, NULL)) {
		syslog(LOG_ERR, "%s: ERROR: COULD NOT INIT MYSQL SUBSYSTEM: ABORTING", __func__);
		exit(-1);
	}

	if (!mysql_thread_safe()) {
		syslog(LOG_ERR, "%s: ERROR: COULD NOT INIT MYSQL SUBSYSTEM: NOT THREAD SAFE: ABORTING", __func__);
		exit(-1);
	}

	syslog(LOG_INFO, "%s: SUCCESS: MYSQL SUBSYSTEM INITIATED...", __func__);

}

/**
 *  executed in a ufsrv thread context, so issuing mysql lib call unsigned long mysql_thread_id(MYSQL *mysql) returns
 *  client thread associated with this ufsrv thread. or "SELECT CONNECTION_ID();"
 *//*
struct _h_connection *InitialiseDbBackend (void)
{
	extern ufsrv *const masterptr;
	struct _h_connection *db_ptr;
//ANNOTATE_IGNORE_READS_BEGIN();
	//__vdrd_AnnotateIgnoreReadsBegin();
	db_ptr=h_connect_mariadb(masterptr->db_backend.address, masterptr->db_backend.username, masterptr->db_backend.password, CONFIG_DBBACKEND_DBNAME, masterptr->db_backend.port, NULL);
//ANNOTATE_IGNORE_READS_END();
	//__vdrd_AnnotateIgnoreReadsEnd();
	if (db_ptr)
	{
		SqlServerDisplayConnectedUsers (db_ptr);
	}
	return db_ptr;

}*/

 /**
  * @brief Diagnostic display of currently connected db clients.
  * @param db_ptr Preallocated db connection object
  */
void SqlServerDisplayConnectedUsers(struct _h_connection *db_ptr)
{
	struct _h_result result;
	struct _h_data * data;

	const char *query_str="SELECT SUBSTRING_INDEX(host, ':', 1) AS host_short, GROUP_CONCAT(DISTINCT USER) AS users,COUNT(*) "
							"FROM information_schema.processlist "
							"GROUP  BY host_short ORDER  BY COUNT(*), host_short;";


	if (h_query_select(db_ptr, query_str, &result) == H_OK) {
		 int col, row;
		  char buf[64];
		  char blob[MBUF];
		  int i;
		  syslog(LOG_DEBUG, "%s: mysql thread_id: '%lu'. SQL SERVER QUERY RESULT: rows: %d, col: %d", __func__, mysql_thread_id(MYSQL_HANDLE(db_ptr)), result.nb_rows, result.nb_columns);

		  for (row = 0; row<result.nb_rows; row++) {
			  for (i=0; i<((struct _h_type_blob *)result.data[row][1].t_data)->length; i++) {
				  blob[i]=*((char*)(((struct _h_type_blob *)result.data[row][1].t_data)->value+i));
//				 printf("%c", *((char*)(((struct _h_type_blob *)result.data[row][col].t_data)->value+i)));
			   }
			  blob[i] = '\0';

        syslog(LOG_DEBUG, "%s: 'host->%s', 'user_name->%s', 'instances->%li'", __func__, ((struct _h_type_text *)result.data[row][0].t_data)->value, blob, ((struct _h_type_int *)result.data[row][2].t_data)->value);
		  }

		h_clean_result(&result);
	}

}

/**
 * @brief Default on_sleep provider: sleep `delay_ms` milliseconds.
 *
 * The backoff algorithm yields delays in milliseconds (see the EXPOBACKOFF_*_MS
 * defaults), so this sleeps in milliseconds via nanosleep(2), retrying on EINTR.
 * Bound as the default in h_connect_mariadb; consumers may override
 * conn->backoff_descriptor.on_sleep.
 */
static int
sDbBackoffSleepMs(long delay_ms)
{
  struct timespec ts = { delay_ms / 1000L, (delay_ms % 1000L) * 1000000L };

  while (nanosleep(&ts, &ts) != 0) {
    if (errno != EINTR) {
      return -1;
    }
  }

  return 0;
}

/**
 * h_connect_mariadb
 * Opens a database connection to a mariadb server
 * return pointer to a struct _h_connection * on sucess, NULL on error
 */
struct _h_connection *h_connect_mariadb(const char * host, const char * user, const char * passwd, const char * db, const unsigned int port, const char * unix_socket)
{
  struct _h_connection *conn = NULL;

  if (host != NULL && db != NULL) {
    conn = malloc(sizeof(struct _h_connection));
    if (conn == NULL) {
      //y_log_message(Y_LOG_LEVEL_ERROR, "Hoel - Error allocating memory for conn");
      return NULL;
    }
    
    conn->type = HOEL_DB_TYPE_MARIADB;
    conn->connection = malloc(sizeof(struct _h_mariadb));
    if (conn->connection == NULL) {
      syslog(LOG_ERR, "%s: Error allocating memory for conn->connection", __func__);
    	free(conn);
    	return NULL;
    }

#if 0
    //AA+ called in main per ufsrv instance
    if (mysql_library_init(0, NULL, NULL)) {
      //y_log_message(Y_LOG_LEVEL_ERROR, "mysql_library_init error, aborting");
      return NULL;
    }
#endif

    //AA+ added thread
    if (mysql_thread_init() != 0) {//zero for success
    	syslog(LOG_ERR, "%s: ERROR: COULD NOT THREAD-INIT MYSQL..", __func__);
      free(conn);
    	return NULL;
    }

    ((struct _h_mariadb *)conn->connection)->db_handle = mysql_init(NULL);
    if (((struct _h_mariadb *)conn->connection)->db_handle == NULL) {
      syslog(LOG_ERR, "%s: ERROR: COULD NOT INIT MYSQL HANDLE OBJECT...", __func__);
      free(conn);
      mysql_thread_end();
      return NULL;
    }

    if (mysql_real_connect(((struct _h_mariadb *)conn->connection)->db_handle, host, user, passwd, db, port, unix_socket, CLIENT_COMPRESS) == NULL) {
      syslog (LOG_ERR, "%s: ERROR: COULD NOT CONNECT TO DB BACKEND: '%s'. ERROR: '%s'",  __func__, db, mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));

      mysql_close(((struct _h_mariadb *)conn->connection)->db_handle);
      free(conn);
      mysql_thread_end(); //AA+

      return NULL;
    }

    // Set MYSQL_OPT_RECONNECT to true to reconnect automatically when connection is closed by the server (to avoid CR_SERVER_GONE_ERROR)
    //Also, check option 'wait_timeout' (using 'show variables;' SQL command): The number of seconds the server waits for activity on a noninteractive connection before closing it.
    int result = mysql_optionsv(((struct _h_mariadb *)conn->connection)->db_handle, MYSQL_OPT_RECONNECT, (void *)"1");

    if (result == 0 ) {
      /* Initialise the retry/backoff descriptor used by h_execute_query's
       * connection-loss retry loop.  Without this, retry_params.backoff_provider
       * and on_sleep are uninitialised and the loop would call a garbage pointer. */
      BackoffAlgorithm_InitializeParamsExponential(
          &conn->backoff_descriptor.retry_params,
          EXPOBACKOFF_RETRY_BACKOFF_BASE_MS,
          EXPOBACKOFF_RETRY_MAX_BACKOFF_DELAY_MS,
          EXPOBACKOFF_RETRY_MAX_ATTEMPTS);
      conn->backoff_descriptor.on_sleep = sDbBackoffSleepMs;

      return conn;
    }

    syslog(LOG_ERR, "%s: ERROR: COULD NOT SET MYSQL_OPT_RECONNECT OPTION for db: '%s'", __func__, db);
    mysql_close(((struct _h_mariadb *)conn->connection)->db_handle);
    free(conn);
    mysql_thread_end();
  }

  return NULL;
}

//AA+
/**
 * @brief Reconnect a previously connected mysql client. The semantics are different from initial connection establishment
 * at server startup time.
 * @param conn_db Previously allocated db holder with allocation for mysql. All descriptors must be provided, such as host name, port etc...
 * @param is_reinitialise if set, reset and reinitialise mysql context variables, including connection handle.
 * @return
 */
struct _h_connection *h_reconnect_mariadb_initialisable(struct _h_connection *conn_db, bool is_reinitialise)
{
  struct _h_mariadb *conn = (struct _h_mariadb *)conn_db->connection;

  if (is_reinitialise) {
    mysql_thread_end();
    mysql_close(conn->db_handle);
  }

  if (mysql_thread_init() != 0) {
    syslog(LOG_ERR, "%s: ERROR: COULD NOT THREAD-INIT MYSQL..", __func__);
    return NULL;
  }

  conn->db_handle = mysql_init(NULL);
  if (conn->db_handle == NULL) {
    syslog(LOG_ERR, "%s: ERROR: COULD NOT INIT MYSQL HANDLE OBJECT...", __func__);
    mysql_thread_end();
    return NULL;
  }

  if (mysql_real_connect(conn->db_handle, conn->host, conn->user, conn->passwd, conn->db, conn->port, conn->unix_socket, CLIENT_COMPRESS) == NULL) {
    syslog (LOG_ERR, "%s: ERROR: COULD NOT CONNECT TO DB BACKEND: '%s'. ERROR: '%s'",  __func__, conn->db, mysql_error(conn->db_handle));

    mysql_close(conn->db_handle);
    mysql_thread_end();

    return NULL;
  }

  // Set MYSQL_OPT_RECONNECT to true to reconnect automatically when connection is closed by the server (to avoid CR_SERVER_GONE_ERROR)
  //Also, check option 'wait_timeout' (using 'show variables like 'wait_timeout';' 'SET wait_timeout = xxx_in_seconds;" SQL command): The number of seconds the server waits for activity on a noninteractive connection before closing it.
  int result = mysql_optionsv(conn->db_handle, MYSQL_OPT_RECONNECT, (void *)"1");

  if (result == 0 ) {
    syslog(LOG_ERR, "%s: SUCCESS: RECONNECTED WITH DB BACKEND: '%s'", __func__, conn->db);
    return conn_db;
  }

  syslog(LOG_ERR, "%s: ERROR: COULD NOT SET MYSQL_OPT_RECONNECT OPTION for db: '%s'", __func__, conn->db);
  mysql_close(conn->db_handle);
  mysql_thread_end();

  return NULL;
}

/**
 * @brief Reconnect an existing connection without reinitialising context
 * @param conn_db[inout] Preinitialised connection context
 * @return 0 on sucess
 */
unsigned int h_reconnect_mariadb(struct _h_connection *conn_db)
{
  struct _h_mariadb *conn = (struct _h_mariadb *)conn_db->connection;
  return mariadb_reconnect(conn->db_handle);
}

/**
 * close connection to database
 */
void h_close_mariadb(struct _h_connection * conn)
{
  mysql_close(((struct _h_mariadb *)conn->connection)->db_handle);

  mysql_thread_end(); //AA+

  mysql_library_end();
 // pthread_mutex_destroy(&((struct _h_mariadb *)conn->connection)->lock);
}

/**
 * escape a string
 * returned value must be free'd after use
 */
char * h_escape_string_mariadb(const struct _h_connection * conn, const char * unsafe) {
  char * escaped = malloc(2 * strlen(unsafe) + sizeof(char));
  if (escaped == NULL) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Hoel - Error allocating memory for escaped");
    return NULL;
  }
  mysql_real_escape_string(((struct _h_mariadb *)conn->connection)->db_handle, escaped, unsafe, strlen(unsafe));
  return escaped;
}

/**
 * escape a string
 * returned value must be free'd after use
 */
char * h_escape_binary_string_mariadb(const struct _h_connection * conn, const char * unsafe, size_t unsafe_sz) {
  char * escaped = malloc(2 * unsafe_sz + sizeof(char));
  if (escaped == NULL) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Hoel - Error allocating memory for escaped");
    return NULL;
  }
  mysql_real_escape_string(((struct _h_mariadb *)conn->connection)->db_handle, escaped, unsafe, unsafe_sz);
  return escaped;
}

/**
 * Return the id of the last inserted value
 */
int h_last_insert_id_mariadb(const struct _h_connection * conn) {
  int id = mysql_insert_id(((struct _h_mariadb *)conn->connection)->db_handle);
  if (id <= 0) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Error executing mysql_insert_id");
    //y_log_message(Y_LOG_LEVEL_DEBUG, "Error message: \"%s\"", mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
  }
  return id;
}

/**
 * @brief True if `errorno` denotes a lost database connection during a query.
 *
 * Each of these should surface as H_ERROR_CONNECTION so that h_execute_query's
 * retry/backoff loop engages:
 *   2013 CR_SERVER_LOST           — lost *during* a query (plaintext)
 *   2026 CR_SSL_CONNECTION_ERROR  — TLS connection loss (FIN or RST)
 *   2006 CR_SERVER_GONE_ERROR     — server gone away (auto-reconnect failed)
 */
static bool
sIsConnectionLossErrno(unsigned int errorno)
{
  return errorno == 2013 || errorno == 2026 || errorno == 2006;
}

/**
 * h_execute_query_mariadb
 * Execute a query on a mariadb connection, set the result structure with the returned values
 * Should not be executed by the user because all parameters are supposed to be correct
 * if result is NULL, the query is executed but no value will be returned
 * return H_OK on success
 */
int h_execute_query_mariadb(struct _h_connection * conn, const char * query, struct _h_result * h_result) {
  MYSQL_RES * result;
  unsigned int num_fields, col, row;
  MYSQL_ROW m_row;
  MYSQL_FIELD * fields;
  struct _h_data * data, * cur_row = NULL;
  unsigned long * lengths;
  int res;
  
//  if (pthread_mutex_lock(&(((struct _h_mariadb *)conn->connection)->lock))) {
  //  return H_ERROR_QUERY;
 // }
  if (mysql_query(((struct _h_mariadb *)conn->connection)->db_handle, query)) {
    unsigned int query_errorno = mysql_errno(((struct _h_mariadb *)conn->connection)->db_handle);
    syslog(LOG_DEBUG, "%s {errno: '%u'}: DB BACKEND QUERY ERROR: '%s'", __func__, query_errorno, mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
    return sIsConnectionLossErrno(query_errorno) ? H_ERROR_CONNECTION : H_ERROR_QUERY;
  }
  
  if (h_result != NULL) {
    result = mysql_store_result(((struct _h_mariadb *)conn->connection)->db_handle);
    
    if (result == NULL) {
      unsigned int query_errorno = mysql_errno(((struct _h_mariadb *)conn->connection)->db_handle);
      syslog(LOG_DEBUG, "%s {pid:''}: DB BACKEND STORE RESULT ERROR: '%s'", __func__, mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
     // pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
      return sIsConnectionLossErrno(query_errorno) ? H_ERROR_CONNECTION : H_ERROR_QUERY;
    }
    
    num_fields = mysql_num_fields(result);
    fields = mysql_fetch_fields(result);
    
    h_result->nb_rows = 0;
    h_result->nb_columns = num_fields;
    h_result->data = NULL;
    for (row = 0; (m_row = mysql_fetch_row(result)) != NULL; row++) {
      cur_row = NULL;
      lengths = mysql_fetch_lengths(result);
      for (col=0; col<num_fields; col++) {
        data = h_get_mariadb_value(m_row[col], lengths[col], fields[col].type);
        res = h_row_add_data(&cur_row, data, col);
        h_clean_data_full(data);
        if (res != H_OK) {
          mysql_free_result(result);
          //pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
          return res;
        }
      }
      res = h_result_add_row(h_result, cur_row, row);
      if (res != H_OK) {
        mysql_free_result(result);
        //pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
        return res;
      }
    }
    mysql_free_result(result);
  }
  
  //pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
  return H_OK;
}

#if 0
/**
 * h_execute_query_json_mariadb
 * Execute a query on a mariadb connection, set the returned values in the json result
 * Should not be executed by the user because all parameters are supposed to be correct
 * return H_OK on success
 */
int h_execute_query_json_mariadb(const struct _h_connection * conn, const char * query, json_t ** j_result) {
  MYSQL_RES * result;
  uint num_fields, col, row;
  MYSQL_ROW m_row;
  MYSQL_FIELD * fields;
  unsigned long * lengths;
  json_t * j_data;
  struct _h_data * h_data;
  char date_stamp[20];
  
  if (pthread_mutex_lock(&(((struct _h_mariadb *)conn->connection)->lock))) {
    return H_ERROR_QUERY;
  }

  if (j_result == NULL) {
    pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
    return H_ERROR_PARAMS;
  }
  
  *j_result = json_array();
  if (*j_result == NULL) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Hoel - Error allocating memory for *j_result");
    pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
    return H_ERROR_MEMORY;
  }

  if (mysql_query(((struct _h_mariadb *)conn->connection)->db_handle, query)) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Error executing sql query");
    //y_log_message(Y_LOG_LEVEL_DEBUG, "Error message: \"%s\"", mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
    //y_log_message(Y_LOG_LEVEL_DEBUG, "Query: \"%s\"", query);
    pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
    return H_ERROR_QUERY;
  }
  
  result = mysql_store_result(((struct _h_mariadb *)conn->connection)->db_handle);
  
  if (result == NULL) {
    //y_log_message(Y_LOG_LEVEL_ERROR, "Error executing mysql_store_result");
    //y_log_message(Y_LOG_LEVEL_DEBUG, "Error message: \"%s\"", mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
    pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
    return H_ERROR_QUERY;
  }
  
  num_fields = mysql_num_fields(result);
  fields = mysql_fetch_fields(result);
  
  for (row = 0; (m_row = mysql_fetch_row(result)) != NULL; row++) {
    j_data = json_object();
    if (j_data == NULL) {
      //y_log_message(Y_LOG_LEVEL_ERROR, "Hoel - Error allocating memory for j_data");
      json_decref(*j_result);
      pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
      return H_ERROR_MEMORY;
    }
    lengths = mysql_fetch_lengths(result);
    for (col=0; col<num_fields; col++) {
      h_data = h_get_mariadb_value(m_row[col], lengths[col], fields[col].type);
      switch (h_data->type) {
        case HOEL_COL_TYPE_INT:
          json_object_set_new(j_data, fields[col].name, json_integer(((struct _h_type_int *)h_data->t_data)->value));
          break;
        case HOEL_COL_TYPE_DOUBLE:
          json_object_set_new(j_data, fields[col].name, json_real(((struct _h_type_double *)h_data->t_data)->value));
          break;
        case HOEL_COL_TYPE_TEXT:
          json_object_set_new(j_data, fields[col].name, json_string(((struct _h_type_text *)h_data->t_data)->value));
          break;
        case HOEL_COL_TYPE_DATE:
          strftime (date_stamp, sizeof(date_stamp), "%FT%TZ", &((struct _h_type_datetime *)h_data->t_data)->value);
          json_object_set_new(j_data, fields[col].name, json_string(date_stamp));
          break;
        case HOEL_COL_TYPE_BLOB:
          json_object_set_new(j_data, fields[col].name, json_stringn(((struct _h_type_blob *)h_data->t_data)->value, ((struct _h_type_blob *)h_data->t_data)->length));
          break;
        case HOEL_COL_TYPE_NULL:
          json_object_set_new(j_data, fields[col].name, json_null());
          break;
      }
      h_clean_data_full(h_data);
    }
    json_array_append_new(*j_result, j_data);
    j_data = NULL;
  }
  mysql_free_result(result);
  pthread_mutex_unlock(&(((struct _h_mariadb *)conn->connection)->lock));
  
  return H_OK;
}
#endif

/**
 * h_get_mariadb_value
 * convert value into a struct _h_data * depening on the m_type given
 * returned value must be free'd with h_clean_data_full after use
 */
struct _h_data * h_get_mariadb_value(const char * value, const unsigned long length, const int m_type) {
  struct _h_data * data = NULL;
  u_int64_t i_value;
  double d_value;
  struct tm tm_value;
  char * endptr;
  
  if (value != NULL)
  {
    switch (m_type)
    {
      case FIELD_TYPE_DECIMAL:
      case FIELD_TYPE_NEWDECIMAL:
      case FIELD_TYPE_TINY:
      case FIELD_TYPE_SHORT:
      case FIELD_TYPE_LONG:
      case FIELD_TYPE_LONGLONG:
      case FIELD_TYPE_INT24:
      case FIELD_TYPE_YEAR:
        i_value = strtoul(value, &endptr, 10);
        if (endptr != value) {
          data = h_new_data_int(i_value);
        } else {
          data = h_new_data_null();
        }
        break;
      case FIELD_TYPE_BIT:
        i_value = strtol(value, &endptr, 2);
        if (endptr != value) {
          data = h_new_data_int(i_value);
        } else {
          data = h_new_data_null();
        }
        break;
      case FIELD_TYPE_FLOAT:
      case FIELD_TYPE_DOUBLE:
        d_value = strtod(value, &endptr);
        if (endptr != value) {
          data = h_new_data_double(d_value);
        } else {
          data = h_new_data_null();
        }
        break;
      case FIELD_TYPE_NULL:
        data = h_new_data_null();
        break;
      case FIELD_TYPE_DATE:
        if (strptime(value, "%F", &tm_value) == NULL) {
          data = h_new_data_null();
        } else {
          data = h_new_data_datetime(&tm_value);
        }
        break;
      case FIELD_TYPE_TIME:
        if (strptime(value, "%T", &tm_value) == NULL) {
          data = h_new_data_null();
        } else {
          data = h_new_data_datetime(&tm_value);
        }
        break;
      case FIELD_TYPE_TIMESTAMP:
      case FIELD_TYPE_DATETIME:
      case FIELD_TYPE_NEWDATE:
        if (strptime(value, "%F %T", &tm_value) == NULL) {
          data = h_new_data_null();
        } else {
          data = h_new_data_datetime(&tm_value);
        }
        break;
      case FIELD_TYPE_TINY_BLOB:
      case FIELD_TYPE_MEDIUM_BLOB:
      case FIELD_TYPE_LONG_BLOB:
      case FIELD_TYPE_BLOB:
        if (length > 0) {
          data = h_new_data_blob(value, length);
        } else {
          data = h_new_data_null();
        }
        break;
      case FIELD_TYPE_VAR_STRING:
      case FIELD_TYPE_ENUM:
      case FIELD_TYPE_SET:
      case FIELD_TYPE_GEOMETRY:
      default:
        data = h_new_data_text(value, length);
        break;
    }
  } else {
    data = h_new_data_null();
  }
  return data;
}

//AA+
MYSQL_STMT *
h_statement_prepare_mariadb(struct _h_connection *conn, const char *sql_statement, size_t sql_statement_sz)
{
  MYSQL_STMT *stmt = mysql_stmt_init(((struct _h_mariadb *)conn->connection)->db_handle);
  if (!stmt) {
    syslog(LOG_ERR, "%s: mysql_stmt_init() failed: %s", __func__, mysql_error(((struct _h_mariadb *)conn->connection)->db_handle));
    return NULL;
  }

  if (mysql_stmt_prepare(stmt, sql_statement, sql_statement_sz)) {
    syslog(LOG_ERR, "%s: mysql_stmt_prepare() failed: %s", __func__, mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return NULL;
  }

  return stmt;
}
//
