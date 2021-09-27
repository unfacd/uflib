/**
 * Copyright (C) 2015-2021 unfacd works
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

#include <syslog.h>
#include <uflib/db/dp_ops.h>
#include <uflib/db/db_sql.h>

/**
 * Necessary convention must be followed with this style of DB operation:
 * 1) User must allocate a DbOpDescriptor defined as follows:
 * 1.1)Query builder that returns a fully constructed query string
 * 1.2)Values into the query string builder must be passed as intptr_t array
 * 1.3)If you don't want the the result object to be finalised after transformation set finaliser.finalise to NULL.
 *     In this case, you have to to invoke the finaliser yourself, otherwise your are leaking memory
 * @param username
 * @param dbop_descriptor
 * @return None
 */
void __attribute__ ((nonnull(1, 2)))
GetDbResultForQuery(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor)
{
  struct _h_result *result = &dbop_descriptor->result;
  char *sql_query_str  = DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER(dbop_descriptor);

#ifdef __UF_FULLDEBUG
  syslog(LOG_DEBUG, "%s (th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_select(db_backend, sql_query_str, result);

  if (sql_result != H_OK) {
    syslog(LOG_DEBUG, "%s (th_ctx:''): ERROR: COULD NOT EXECUTE QUERY: '%s'", __func__, sql_query_str);

    DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER_FINALISER(dbop_descriptor, sql_query_str);

    dbop_descriptor->dbop_status.status = DB_ERROR;
    return;
  }

  DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER_FINALISER(dbop_descriptor, sql_query_str);

  if (result->nb_rows == 0) {
#ifdef __UF_FULLDEBUG
    syslog(LOG_DEBUG, "%s (th_ctx:'%p'): ERROR: COULD FIND CORRESPONDING DB RECORD", __func__, THREAD_CONTEXT_PTR);
#endif
    h_clean_result(result);

    dbop_descriptor->dbop_status.status = EMPTY_SET;

    return;
  }

  DBOP_DESCRIPTOR_INVOKE_TRANSFORMER_IF_PRESENT(dbop_descriptor);

  DBOP_DESCRIPTOR_INVOKE_RESULT_FINALISER_IF_PRESENT(dbop_descriptor);

}

void __attribute__((nonnull(1)))
GetDbResultForUpdate(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor)
{
  struct _h_result *result = &dbop_descriptor->result;
  char *sql_query_str  = DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER(dbop_descriptor);

#ifdef __UF_FULLDEBUG
  syslog(LOG_DEBUG, "%s (th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_update(db_backend, sql_query_str);

  if (sql_result != H_OK) {
    syslog(LOG_DEBUG, "%s (th_ctx:''): ERROR: COULD NOT EXECUTE QUERY: '%s'", __func__, sql_query_str);

    DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER_FINALISER(dbop_descriptor, sql_query_str);

    dbop_descriptor->dbop_status.status = DB_ERROR;
    return;
  }

  DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER_FINALISER(dbop_descriptor, sql_query_str);

  DBOP_DESCRIPTOR_INVOKE_RESULT_FINALISER_IF_PRESENT(dbop_descriptor);

}