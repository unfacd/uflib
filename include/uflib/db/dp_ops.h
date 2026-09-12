/**
 * Copyright (C) 2015-2025 unfacd works
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

#ifndef UFLIB_DB_DP_OPS_H
#define UFLIB_DB_DP_OPS_H

#include <uflib/uflib_defs.h>

#include <uflib/db/db_op_descriptor_type.h>

PUBLIC_API void GetDbResultForQuery(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor);
PUBLIC_API void GetDbResultForUpdate(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor);
PUBLIC_API void GetDbResultForInsert(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor);
PUBLIC_API void GetDbResultForDelete(DbBackend *db_backend, DbOpDescriptor *dbop_descriptor);

#endif //UFSRV_DP_OPS_H
