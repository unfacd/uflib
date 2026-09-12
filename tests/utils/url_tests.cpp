/**
 * Copyright (C) 2015-2026 unfacd works
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

/**
 * @file url_tests.cpp
 * @brief Unit tests for TokeniseUrlParams (utils_url.h) — gtest suite covering
 *        basic tokenisation, edge cases, escape sequences, tokens_sz_hint
 *        boundaries, and output-correctness validation.
 */

#include "gtest/gtest.h"
#include <cstring>
#include <cstdio>

extern "C" {
#include <uflib/utils_urls.h>
}

/* ---------------------------------------------------------------------------
 * Helper — set up a UrlParamsDescriptor with @p count pre‑allocated tokens.
 * The tokens and pointer array must remain live for the duration of the test.
 * --------------------------------------------------------------------------- */
static void
SetupDescriptor(UrlParamsDescriptor *desc,
                UrlParamToken      *token_array,
                UrlParamToken     **ptr_array,
                size_t              count)
{
    for (size_t i = 0; i < count; i++) {
        ptr_array[i] = &token_array[i];
    }
    desc->tokens    = ptr_array;
    desc->tokens_sz = 0;
}

/* =========================================================================
 * 1 — Basic tokenisation
 * ========================================================================= */

TEST(utils_url, simple_path_three_segments)
{
    char             buf[] = "a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

TEST(utils_url, single_token_no_slashes)
{
    char             buf[] = "hello";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "hello");
}

TEST(utils_url, leading_slash_is_skipped)
{
    char             buf[] = "/a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

TEST(utils_url, trailing_slash_is_ignored)
{
    char             buf[] = "a/b/c/";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

TEST(utils_url, both_leading_and_trailing_slashes)
{
    char             buf[] = "/a/b/c/";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

/* =========================================================================
 * 2 — Empty and degenerate paths
 * ========================================================================= */

TEST(utils_url, empty_string_yields_one_empty_token)
{
    char             buf[] = "";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "");
}

TEST(utils_url, single_slash_yields_one_empty_token)
{
    char             buf[] = "/";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "");
}

TEST(utils_url, multiple_leading_slashes_only_one_skipped)
{
    // Leading slash is skipped; the second '/' starts an empty token
    // which is then immediately followed by NUL → break.
    char             buf[] = "//";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "");
}

TEST(utils_url, triple_slash_only_yields_one_empty_token)
{
    // Leading '/' skipped; then '/' at pos 1 is delimiter, p+1 is '/' at
    // pos 2, not NUL, so we advance to token[1]; token[1] points to pos 2
    // which is '/'; at that '/' we set NUL, p+1 is NUL → break.
    // Result: two tokens, both empty.
    char             buf[] = "///";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "");
    EXPECT_STREQ(tokens[1].token, "");
}

TEST(utils_url, double_slash_in_middle_yields_empty_token)
{
    char             buf[] = "a//b";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "");
    EXPECT_STREQ(tokens[2].token, "b");
}

TEST(utils_url, multiple_consecutive_slashes_produce_multiple_empty_tokens)
{
    char             buf[] = "a///b";
    UrlParamToken    tokens[6];
    UrlParamToken   *ptrs[6];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 6);
    TokeniseUrlParams(buf, &desc, 6);

    EXPECT_EQ(desc.tokens_sz, 4U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "");
    EXPECT_STREQ(tokens[2].token, "");
    EXPECT_STREQ(tokens[3].token, "b");
}

/* =========================================================================
 * 3 — tokens_sz_hint boundaries
 * ========================================================================= */

TEST(utils_url, hint_one_stops_after_first_delimiter)
{
    // hint=1 — token[0] gets "a" (null-terminated at the first '/'), then
    // counter+1 == hint → break.  tokens_sz = 1.
    char             buf[] = "a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 1);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "a");
}

TEST(utils_url, hint_two_stops_at_second_delimiter)
{
    char             buf[] = "a/b/c/d";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
}

TEST(utils_url, hint_exactly_matches_token_count)
{
    char             buf[] = "a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 3);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

TEST(utils_url, hint_larger_than_actual_tokens)
{
    // Hint is larger than needed — tokens_sz reflects actual count.
    char             buf[] = "a/b";
    UrlParamToken    tokens[10];
    UrlParamToken   *ptrs[10];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 10);
    TokeniseUrlParams(buf, &desc, 10);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
}

/* =========================================================================
 * 4 — Escape sequences ('\/')
 * ========================================================================= */

TEST(utils_url, escaped_slash_not_treated_as_delimiter)
{
    // "a\\/b" — the '\/' is escaped, so the whole thing is one token.
    char             buf[] = "a\\/b";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "a\\/b");
}

TEST(utils_url, escaped_slash_then_real_delimiter)
{
    // "a\\/b/c" — '\/' is escaped (1 token for "a\/b"), then "/c".
    char             buf[] = "a\\/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "a\\/b");
    EXPECT_STREQ(tokens[1].token, "c");
}

TEST(utils_url, escaped_slash_at_beginning)
{
    char             buf[] = "\\/a/b";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "\\/a");
    EXPECT_STREQ(tokens[1].token, "b");
}

TEST(utils_url, multiple_escaped_slashes)
{
    char             buf[] = "a\\/b\\/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "a\\/b\\/c");
}

TEST(utils_url, backslash_not_followed_by_slash_is_not_an_escape)
{
    // "a\\b" — backslash followed by 'b', not '/', so no escape.
    // The '\\' is just an ordinary character.
    char             buf[] = "a\\b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "a\\b");
    EXPECT_STREQ(tokens[1].token, "c");
}

TEST(utils_url, consecutive_escapes_backslash_backslash_slash)
{
    // "\\\\/" — backslash(0), backslash(1), slash(2)
    // The first backslash is NOT an escape (p+1 is another backslash, not '/').
    // The second backslash IS an escape (p+1 is '/').
    // Token 0 = "\\/".  No further tokens.
    char             buf[] = "\\\\/";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "\\\\/");
}

TEST(utils_url, escape_only_entire_path)
{
    char             buf[] = "\\/";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "\\/");
}

/* =========================================================================
 * 5 — Output‑correctness validation
 * ========================================================================= */

TEST(utils_url, tokens_sz_is_one_indexed_count)
{
    // Empty string → 1 token, not 0.
    char             buf[] = "";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
}

TEST(utils_url, token_pointers_point_into_original_buffer)
{
    char             buf[] = "hello/world";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    // Token pointers must lie within the original buffer.
    EXPECT_GE(tokens[0].token, buf);
    EXPECT_LT(tokens[0].token, buf + sizeof(buf));
    EXPECT_GE(tokens[1].token, buf);
    EXPECT_LT(tokens[1].token, buf + sizeof(buf));
}

TEST(utils_url, original_string_is_null_terminated_at_delimiters)
{
    char             buf[] = "a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    // After tokenisation, the '/' delimiters are overwritten with NUL.
    // buf[1] was '/', should now be '\0'.
    EXPECT_EQ(buf[1], '\0');
    EXPECT_EQ(buf[3], '\0');
}

TEST(utils_url, hint_does_not_null_terminate_last_token_for_full_scan)
{
    // When the scan runs to NUL naturally (no trailing slash), the last
    // token is already NUL-terminated by the original string's terminator.
    char             buf[] = "a/b/c";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
}

/* =========================================================================
 * 6 — Documented example (from the function's Doxygen)
 * ========================================================================= */

TEST(utils_url, documented_example)
{
    // "1/2//3/4/5/" → 6 tokens, token[2] is empty (the "//" pair).
    char             buf[] = "1/2//3/4/5/";
    UrlParamToken    tokens[8];
    UrlParamToken   *ptrs[8];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 8);
    TokeniseUrlParams(buf, &desc, 8);

    EXPECT_EQ(desc.tokens_sz, 6U);
    EXPECT_STREQ(tokens[0].token, "1");
    EXPECT_STREQ(tokens[1].token, "2");
    EXPECT_STREQ(tokens[2].token, "");    // empty — the "//" pair
    EXPECT_STREQ(tokens[3].token, "3");
    EXPECT_STREQ(tokens[4].token, "4");
    EXPECT_STREQ(tokens[5].token, "5");
}

/* =========================================================================
 * 7 — Additional edge cases
 * ========================================================================= */

TEST(utils_url, single_character_tokens)
{
    char             buf[] = "a/b/c/d/e/f/g/h/i/j";
    UrlParamToken    tokens[12];
    UrlParamToken   *ptrs[12];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 12);
    TokeniseUrlParams(buf, &desc, 12);

    EXPECT_EQ(desc.tokens_sz, 10U);
    EXPECT_STREQ(tokens[0].token, "a");
    EXPECT_STREQ(tokens[1].token, "b");
    EXPECT_STREQ(tokens[2].token, "c");
    EXPECT_STREQ(tokens[3].token, "d");
    EXPECT_STREQ(tokens[4].token, "e");
    EXPECT_STREQ(tokens[5].token, "f");
    EXPECT_STREQ(tokens[6].token, "g");
    EXPECT_STREQ(tokens[7].token, "h");
    EXPECT_STREQ(tokens[8].token, "i");
    EXPECT_STREQ(tokens[9].token, "j");
}

TEST(utils_url, token_with_embedded_special_characters)
{
    // Characters other than '/' (and the escape sequence) are left alone.
    char             buf[] = "hello world/foo@bar/price$5.99";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 3U);
    EXPECT_STREQ(tokens[0].token, "hello world");
    EXPECT_STREQ(tokens[1].token, "foo@bar");
    EXPECT_STREQ(tokens[2].token, "price$5.99");
}

TEST(utils_url, token_with_query_string_characters)
{
    // '?' and '&' are not delimiters — they pass through as part of a token.
    char             buf[] = "path/to?key=val&other=123";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 2U);
    EXPECT_STREQ(tokens[0].token, "path");
    EXPECT_STREQ(tokens[1].token, "to?key=val&other=123");
}

TEST(utils_url, leading_slash_with_empty_path)
{
    char             buf[] = "/";
    UrlParamToken    tokens[2];
    UrlParamToken   *ptrs[2];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 2);
    TokeniseUrlParams(buf, &desc, 2);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "");
}

TEST(utils_url, trailing_slash_after_single_token)
{
    char             buf[] = "token/";
    UrlParamToken    tokens[4];
    UrlParamToken   *ptrs[4];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 4);
    TokeniseUrlParams(buf, &desc, 4);

    EXPECT_EQ(desc.tokens_sz, 1U);
    EXPECT_STREQ(tokens[0].token, "token");
}

TEST(utils_url, only_slashes_six)
{
    // "//////" (6 slashes) — leading skipped, then alternating delimiter/empty.
    // Trace: leading '/' skipped; p at pos 1='/', sets NUL, advances to
    // token[1] → pos 2='/'; sets NUL, token[2] → pos 3='/'; sets NUL,
    // token[3] → pos 4='/'; sets NUL, token[4] → pos 5='/'; sets NUL,
    // p+1='\0' → break.  4 tokens all empty.
    //
    // Actually, let me trace precisely: str="//////", 6 slashes + NUL.
    // Start: p=str[0]='/', p++ → p=1. param->token=str[1]='/'.
    // Loop: p=1='/'. *p='\0'. c=0, c+1=1 ≠ hint. *(p+1)=str[2]='/' ≠ '\0'.
    //   param=tokens[1]->token=str[2]. p=2.
    // Loop: p=2='/'. *p='\0'. c=1, c+1=2 ≠ hint. *(p+1)=str[3]='/' ≠ '\0'.
    //   param=tokens[2]->token=str[3]. p=3.
    // Loop: p=3='/'. *p='\0'. c=2, c+1=3 ≠ hint. *(p+1)=str[4]='/' ≠ '\0'.
    //   param=tokens[3]->token=str[4]. p=4.
    // Loop: p=4='/'. *p='\0'. c=3, c+1=4 ≠ hint. *(p+1)=str[5]='/' ≠ '\0'.
    //   param=tokens[4]->token=str[5]. p=5.
    // Loop: p=5='/'. *p='\0'. c=4, c+1=5 ≠ hint. *(p+1)=str[6]='\0' → BREAK.
    // tokens_sz = 4+1 = 5.
    // Token[0] = str[1] (now '\0') = ""
    // Token[1] = str[2] (now '\0') = ""
    // Token[2] = str[3] (now '\0') = ""
    // Token[3] = str[4] (now '\0') = ""
    // Token[4] = str[5] (now '\0') = ""
    // All 5 tokens are empty.
    char             buf[] = "//////";
    UrlParamToken    tokens[8];
    UrlParamToken   *ptrs[8];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 8);
    TokeniseUrlParams(buf, &desc, 8);

    EXPECT_EQ(desc.tokens_sz, 5U);
    for (size_t i = 0; i < 5; i++) {
        EXPECT_STREQ(tokens[i].token, "") << "token[" << i << "] should be empty";
    }
}

TEST(utils_url, empty_token_middle_preserves_probe_chain)
{
    // Verify the documented invariant: "1/2//3/4/5/" has token[2] empty,
    // and token[3] correctly points to "3".
    char             buf[] = "1/2//3/4/5/";
    UrlParamToken    tokens[8];
    UrlParamToken   *ptrs[8];
    UrlParamsDescriptor desc;

    SetupDescriptor(&desc, tokens, ptrs, 8);
    TokeniseUrlParams(buf, &desc, 8);

    ASSERT_GE(desc.tokens_sz, 4U);
    EXPECT_STREQ(tokens[2].token, "");   // empty token at index 2
    EXPECT_STREQ(tokens[3].token, "3");  // probe chain continued correctly
}
