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

#ifndef UFSRV_DB_OP_TYPE_H
#define UFSRV_DB_OP_TYPE_H

#include <stdint.h>
#include <uflib/db/db_sql.h>

typedef struct _h_result DbOpResult ;
typedef  void DbOpResultRecord;
typedef void ClientContextData;

#define DBOP_QUERY_PROVIDER_VALUE(x)  ((intptr_t)(x))
#define ASSIGN_DBOP_RESULT(x) ((DbOpResult *)(x))
#define ASSIGN_DBOP_RESULT_RECORD(x) ((DbOpResultRecord *)(x))

#define DBOP_DESCRIPTOR_IS_PRESENT(x) (likely(x) != NULL)
#define DBOP_DESCRIPTOR_TRANSFORMER_IS_PRESENT(x) ((x)->transformer.transform != NULL)

#define DBOP_DESCRIPTOR_RESULT_FINALISER_IS_PRESENT(x) ((x)->finaliser.finalise != NULL)
#define DBOP_DESCRIPTOR_INVOKE_RESULT_FINALISER(x) ((x)->finaliser.finalise(&((x)->result)))
#define DBOP_DESCRIPTOR_INVOKE_RESULT_FINALISER_IF_PRESENT(x) if DBOP_DESCRIPTOR_RESULT_FINALISER_IS_PRESENT((x)) DBOP_DESCRIPTOR_INVOKE_RESULT_FINALISER((x))

#define DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER(x) ((x)->query_provider.provide((x)->query_provider.values))
#define DBOP_DESCRIPTOR_QUERY_PROVIDER_FINALISER_IS_PRESENT(x) ((x)->query_provider.finalise != NULL)
#define DBOP_DESCRIPTOR_INVOKE_QUERY_PROVIDER_FINALISER(x, y) if DBOP_DESCRIPTOR_QUERY_PROVIDER_FINALISER_IS_PRESENT((x)) (x)->query_provider.finalise((y))

#define DBOP_DESCRIPTOR_INVOKE_TRANSFORMER(x) ((x)->transformer.transform((x)))
#define DBOP_DESCRIPTOR_INVOKE_TRANSFORMER_IF_PRESENT(x) if (DBOP_DESCRIPTOR_TRANSFORMER_IS_PRESENT(x)) DBOP_DESCRIPTOR_INVOKE_TRANSFORMER((x))

typedef enum DBOPStatus {
  SUCCESS,
  TRANSFORMER_ERROR,
  DB_ERROR,
  EMPTY_SET
} DBOPStatus;

typedef struct DbOpDescriptor {
  DbOpResult result;
  ClientContextData *ctx_data;

 struct {
   int (*transform)(struct DbOpDescriptor *);
 } transformer;

 struct {
   int (*finalise)(DbOpResult *);
 } finaliser;

 struct {
   char *(*provide)(intptr_t *);
   void (*finalise)(char *);
   intptr_t *values;
 } query_provider;

 struct {
   DBOPStatus status;
 } dbop_status;
} DbOpDescriptor;

#endif //UFSRV_DB_OP_TYPE_H
