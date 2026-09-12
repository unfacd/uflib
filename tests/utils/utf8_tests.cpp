/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */


#include "gtest/gtest.h"
#include <stdint.h>

extern "C" {
#include "uflib/utils_str.h"
}

TEST(utf8, ok) {
const char *utf8_mixed = "Hello 世界 🌍";  // Mixed ASCII, CJK, emoji

const char *invalid_utf8 = "Hello \x80 World";  // Invalid continuation byte
const uint8_t buffer[] = {0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC}; //Japan

size_t bytes_read;
bool is_valid;
size_t chars = DefensiveStrlenUtf8(utf8_mixed, 100, &bytes_read, &is_valid);
//printf("Characters: %zu, Bytes: %zu, Valid: %s\n", chars, bytes_read, is_valid ? "yes" : "no");

is_valid = IsUtf8Valid(invalid_utf8, 50);
EXPECT_FALSE(is_valid);

size_t buf_chars = DefensiveStrlenUtf8Binary(buffer, sizeof(buffer), 10);
}