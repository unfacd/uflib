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

#include "gtest/gtest.h"

extern "C" {
#include "uflib/ufsrvuid.h"
}

TEST(ufsrvuid, ok) {
  UfsrvUid ufsrvuid = {0};
  UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &ufsrvuid);
  ASSERT_EQ(UfsrvUidGetSequenceId(&ufsrvuid), 1);
//  ASSERT_STREQ("file", source);
}

//TEST(testy, not_ok) {
//  ASSERT_EQ(customer_check(0), 0);
//}
