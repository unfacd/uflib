/**
 * Copyright (C) 2015-2021 unfacd works
 * Copyright (c) 2015 Colum Paget <colums.projects@googlemail.com>
 * https://github.com/ColumPaget/libUseful
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
 *
*/

#ifndef UFSRV_TOKENISER_H
#define UFSRV_TOKENISER_H

#include <stdarg.h>
#include <string.h>

typedef void (*ARRAY_ITEM_DESTROY_FUNC)(void *);

/*
These functions break a string up into tokens. GetToken works like:
ptr=GetToken(Str, "::",&Token,0);
while (ptr)
{
printf("%s\n",Token);
ptr=GetToken(ptr, "::",&Token,0);
}
This imagines a string broken up with "::" separators, like "this::that::theother", illustrating that tokens can be more than one character long. You can also use the 'GETTOKEN_MULTI_SEPARATORS' flag to pass multiple separators to GetToken, like this:
ptr=GetToken(Str, ",|;|\n",&Token,GETTOKEN_MULTI_SEPARATORS);
while (ptr)
{
printf("%s\n",Token);
ptr=GetToken(ptr, ",|;|\n",&Token,GETTOKEN_MULTI_SEPARATORS);
}
This imagines that Str is broken up by three types of separator, commas, semicolons and newlines. The '|' pipe symbol is a divider used to indicate different spearators, as again separators can be more than one character long.
GetToken also accepts some special separator types. "\\S" means 'any whitespace' and "\\X" means 'code separators', which is any whitespace plus "(", ")", "=", "!", "<" and ">", which is intended for tokenizing simple conditional expressions.
For example:
ptr=GetToken(Str, "\\S",&Token,0);
while (ptr)
{
printf("%s\n",Token);
ptr=GetToken(ptr, "\\S",&Token,0);
}
Will break a string up by whitespace. (it has to be "\\S" unfortunately because C will treat a single '\' as a quote, and so \\S becomes \S on compilation.
GetToken also understands quotes in the target string. This is activated by passing the "GETTOKEN_QUOTES", like this:
ptr=GetToken(Str, "\\S",&Token,GETTOKEN_QUOTES);
while (ptr)
{
printf("%s\n",Token);
ptr=GetToken(ptr, "\\S",&Token,GETTOKEN_QUOTES);
}
This will break a string up by whitespace, but any substrings that contain whitespace within quotes will not be broken up. So a string like:
one two three four "five six seven" eight nine
will break up into
one
two
three
four
five six seven
eight
nine
Notice that 'GETTOKEN_QUOTES' also strips quotes from tokens. If you don't want the quotes stripped off, use GETTOKEN_HONOR_QUOTES instead.
The GETTOKEN_INCLUDE_SEPARATORS flag causes separators to be passed as tokens, so
ptr=GetToken(Str, ";", &Token, GETTOKEN_INCLUDE_SEPARATORS);
would break the string "this ; that" up into:
this
;
that
Alternatively the GETTOKEN_APPEND_SEPARATORS Flag adds a separator to the end of a token, so now we'd get
this;
that
GETTOKEN_STRIP_SPACE will cause whitespace to be stripped from the start and end of tokens, even if the separator character is not whitespace.
see examples/Tokenizer.c for examples
*/

#define DestroyString(s) (Destroy(s))

//returns true if string is NULL or empty
#define StrEnd(str) ( (str &&  (*(const char *) str != '\0')) ? false : true )

//return length of a string. Doesn't crash if string is NULL, just returns 0
#define StrLen(str) ( (str) ? strlen(str) : 0 )

//if a string is not null, and not empty (contains chars) then return true. Doesn't call 'strlen' or iterate through entire string
//so is more efficient then using StrLen for the same purpose
#define StrValid(str) ( (str && (*(const char *) str != '\0')) ? true : false )

//returns true if string is NULL or empty
#define StrEnd(str) ( (str &&  (*(const char *) str != '\0')) ? false : true )

//copy a number of strings into Dest. Like this:
//   Dest=MCopyStr(Dest, Str1, Str2, Str3, NULL);
#define MCopyStr(Dest, ...) InternalMCopyStr(Dest, __VA_ARGS__, NULL)

//list MCopyStr but concatanantes strings onto Dest rather than copying into it
#define MCatStr(Dest, ...) InternalMCatStr(Dest, __VA_ARGS__, NULL)

//return a copy of a string
#define CloneStr(Str) (CopyStr(NULL,Str))

//Concat 'Src' onto 'Dest'
//yes, we need the strlen even though it means traversing the string twice.
//We need to know how much room 'realloc' needs
#define CatStr(Dest, Src) (CatStrLen(Dest,Src,StrLen(Src)))

//Quote some standard chars in a string with '\'.
#define EnquoteStr(Dest, Src) (QuoteCharsInStr((Dest), (Src), "'\"\r\n"))

#define StringArrayDestroy(a) ArrayDestroy((void **) (a), (Destroy))

//Flags for GetToken

#define GETTOKEN_MULTI_SEPARATORS 1 //multiple seperators divided by '|'
#define GETTOKEN_MULTI_SEP 1
#define GETTOKEN_HONOR_QUOTES 2  //honor quotes but don't strip them
#define GETTOKEN_STRIP_QUOTES 4  //strip quotes (but otherwise ignore)
#define GETTOKEN_QUOTES 6  //honor and strip quotes
#define GETTOKEN_INCLUDE_SEPARATORS 8  //include separators as tokens
#define GETTOKEN_INCLUDE_SEP 8
#define GETTOKEN_APPEND_SEPARATORS 16 //append separators to previous token
#define GETTOKEN_APPEND_SEP 16
#define GETTOKEN_BACKSLASH  32  //treat backslashes as normal characters, rather than a form of quoting
#define GETTOKEN_STRIP_SPACE 64 //strip whitespace from start and end of token

const char *GetToken(const char *SearchStr, const char *Delim, char **Token, int Flags);
const char *GetNameValuePair(const char *Input, const char *PairDelim, const char *NameValueDelim, char **Name, char **Value);

int GetTokenParseConfig(const char *Config);

#endif //UFSRV_TOKENISER_H
