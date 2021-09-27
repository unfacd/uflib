#include <ctype.h>
#include <stdlib.h>

#include <uflib/standard_defs.h>
#include <uflib/utils_str.h>
#include <uflib/tokeniser.h>

#define TOK_SPACE 1
#define TOK_CODE  2

//region
static const char *GetTokenStepThru(const char *Str, int Flags)
{
  const char *ptr;

  switch (*Str)
  {
    case '\0': return(Str); break;

    case '\\':
      if (! (Flags & GETTOKEN_BACKSLASH))
      {
        //if we got a backslash, then skip past it and the character it quotes,
        //unless it's quoting a NULL character (which just ain't allowed)
        if ( *(Str+1) != '\0' ) return(Str+2);
      }
      break;

    case '"':
    case '\'':
      if (Flags & GETTOKEN_HONOR_QUOTES)
      {
        ptr=traverse_quoted(Str);
        ptr++;
        return(ptr);
      }
      else return(Str+1);
      break;
  }

  //default result
  return(Str+1);
}

static char *SetStrLen(char *Str, size_t len)
{
  char *ptr;

//  StrLenCacheDel(Str);
  // Note len+1 to allow for terminating NULL
  if (Str==NULL) ptr=(char *) calloc(1, len + 8);
  else ptr=(char *) realloc(Str, len + 8);

//  if (len > 8) StrLenCacheAdd(ptr, len);
  return(ptr);
}

static char *CopyStrLen(char *Dest, const char *Src, size_t len)
{
  const char *src, *end;
  char *dst;

  Dest=SetStrLen(Dest,len);
  dst=Dest;
  src=Src;
  if (src)
  {
    end=src+len;
    while ((src < end) && (*src != '\0'))
    {
      *dst=*src;
      dst++;
      src++;
    }
  }
  *dst='\0';

  return(Dest);
}

static char *CatStrLen(char *Dest, const char *Src, size_t len)
{
  const char *src, *end;
  char *dst;
  int dstlen;

  dstlen=strlen(Dest);
  Dest=SetStrLen(Dest,dstlen+len);
  dst=Dest+dstlen;
  src=Src;
  end=src+len;
  while ((src < end) && (*src != '\0'))
  {
    *dst=*src;
    dst++;
    src++;
  }
  *dst='\0';

  return(Dest);
}

static char *CopyStr(char *Dest, const char *Src)
{
  if (Dest) *Dest=0;
  return(CatStr(Dest,Src));
}

static void Destroy(void *Obj)
{
  if (Obj) free(Obj);
}

//These functions return str to allow easy use in languages like lua where the string object
//is opaque and we must return a new one like this: StripTrailingWhitespace(CopyStr(NULL,Str));
static char *StripTrailingWhitespace(char *Str)
{
  size_t len;
  char *ptr;

  len=strlen(Str);
  if (len > 0)
  {
    for(ptr=Str+len-1; (ptr >= Str) && isspace(*ptr); ptr--) *ptr='\0';
  }

  return(Str);
}

static char *StripLeadingWhitespace(char *Str)
{
  size_t len;
  char *ptr, *start=NULL;

  if (Str)
  {
    for(ptr=Str; *ptr !='\0'; ptr++)
    {
      if ((! start) && (! isspace(*ptr))) start=ptr;
    }

    if (!start) start=ptr;
    len=ptr-start;
    //+1 to get the '\0' character at the end of the line
    memmove(Str,start,len+1);
  }
  return(Str);
}

__unused static char *StripCRLF(char *Str)
{
  size_t len;
  char *ptr;

  len=strlen(Str);
  if (len > 0)
  {
    for (ptr=Str+len-1; ptr >= Str; ptr--)
    {
      if (strchr("\n\r",*ptr))
      {
        *ptr='\0';
      }
      else break;
    }
  }

  return(Str);
}

static char *StripQuotes(char *Str)
{
  int len;
  char *ptr, StartQuote='\0';

  ptr=Str;
  while (isspace(*ptr)) ptr++;

  if ((*ptr=='"') || (*ptr=='\''))
  {
    StartQuote = *ptr;
    len = StrLen(ptr);
    if ((len > 0) && (StartQuote != '\0') && (ptr[len-1]==StartQuote))
    {
      if (ptr[len-1]==StartQuote) ptr[len-1]='\0';
      memmove(Str,ptr+1,len);
    }
  }
  return(Str);
}

__unused static void **ArrayAdd(void **Array, void *Item)
{
  void *ptr;
  int count=0;

  if (! Array) Array=calloc(10, sizeof(void *));
  else
  {
    for (ptr=*Array; ptr !=NULL; ptr++)
    {
      count++;
    }
  }

  Array=realloc(Array, (count+10) * sizeof(void *));
  Array[count]=Item;
  Array[count+1]=NULL;
  return(Array);
}

static void ArrayDestroy(void **Array, ARRAY_ITEM_DESTROY_FUNC DestroyFunc)
{
  void **ptr;

  for (ptr=Array; *ptr != NULL; ptr++) DestroyFunc(*ptr);
  if (Array != NULL) free(Array);
}
//regionend

//Does the current position match against Pattern
int GetTokenSepMatch(const char *Pattern,const char **start, const char **end, int Flags)
{
  const char *pptr, *eptr;
  int MatchType=0;

//if start and end pointers are same, then we've no string
  if (*start == *end) return false;

  pptr=Pattern;
  eptr=*start;


  while (1)
  {
//check the current 'pattern' char
    switch (*pptr)
    {
      //if we run out of pattern, then we got a match
      case '\0':
        *end = eptr;
        return true;
        break;


        //Quoted char
      case '\\':
        pptr++;
        if (*pptr=='S') MatchType=TOK_SPACE;
        if (*pptr=='X') MatchType=TOK_CODE;
        break;

    }

//traverse the string
    switch (*eptr)
    {
      //if we run out of string, then we got a part match, but its not
      //a full match, so we return fail
      case '\0':
        *start=eptr;
        *end=eptr;
        return(false);
        break;

      case '\\':
        //if we got a quoted character we can't have found
        //the separator, so return false
        if (Flags & GETTOKEN_BACKSLASH)
        {
          if (*eptr != *pptr) return(false);
        }
        else
        {
          eptr++;
          *start=eptr;
          return(false);
        }
        break;

      case '"':
      case '\'':
        if (Flags & GETTOKEN_HONOR_QUOTES) return(false);
        else if (*eptr != *pptr) return(false);
        break;

      case ' ':
      case '	':
      case '\n':
      case '\r':
        if ((MatchType==TOK_SPACE) || (MatchType==TOK_CODE))
        {
          while (isspace(*eptr)) eptr++;
          eptr--;
          MatchType=0;
        }
        else if (*eptr != *pptr) return false;
        break;

      case '(':
      case ')':
      case '=':
      case '!':
      case '<':
      case '>':
        if (MatchType==TOK_CODE) MatchType=0;
        else if (*eptr != *pptr) return false;
        break;

      default:
        if (MatchType != 0) return false;
        if (*eptr != *pptr) return false;
        break;
    }

    pptr++;
    eptr++;
  }

  return false;
}

//Searches through 'String' for a match of a Pattern
int GetTokenFindSeparator(const char *Pattern, const char *String, const char **SepStart, const char **SepEnd, int Flags)
{
  const char *start_ptr=NULL, *end_ptr=NULL, *ptr;

  start_ptr=String;
  while (*start_ptr != '\0')
  {
    if ((*start_ptr=='\\') && (! (Flags & GETTOKEN_BACKSLASH)))
    {
      start_ptr++;
      start_ptr++;
      continue;
    }

    if (GetTokenSepMatch(Pattern,&start_ptr, &end_ptr, Flags))
    {
      *SepStart=start_ptr;
      *SepEnd=end_ptr;
      return true;
    }

    start_ptr=GetTokenStepThru(start_ptr, Flags);
  }

//We found nothing, set sep start to equal end of string
  *SepStart=start_ptr;
  *SepEnd=start_ptr;

  return false;
}

char **BuildMultiSeparators(const char *Pattern)
{
  const char *ptr, *next;
  int count=0;
  char **separators;

  ptr=strchr(Pattern, '|');
  while (ptr)
  {
    count++;
    ptr++;
    ptr=strchr(ptr,'|');
  }

//count + 2 because last item will lack a '|' and  we want a NULL at the end
//of the separator array
  separators=(char **) calloc(count+2,sizeof(char *));
  ptr=Pattern;
  count=0;
  while (ptr && (*ptr !='\0'))
  {
    while (*ptr=='|') ptr++;
    if (*ptr!='\0')
    {
      next=strchr(ptr,'|');
      if (next) separators[count]=CopyStrLen(NULL, ptr, next-ptr);
      else separators[count]=CopyStr(NULL, ptr);
      count++;
      ptr=next;
    }
  }

  return(separators);
}

int GetTokenMultiSepMatch(char **Separators, const char **start_ptr, const char **end_ptr, int Flags)
{
  char **sep_ptr;
  const char *sptr=NULL, *eptr=NULL, *tptr;

  //must do this as GetTokenSepMatch moves these pointers on, and that'll cause problems
  //if one of our separators fails to match part way through
  sptr=*start_ptr;
  eptr=*end_ptr;

  while (*sptr !='\0')
  {
    sep_ptr=Separators;

    while (*sep_ptr !=NULL)
    {
      //we have to protect sptr just like start_ptr, or else GetTokenSepMatch will change it
      tptr=sptr;
      if (GetTokenSepMatch(*sep_ptr, &tptr, &eptr, Flags))
      {
        *start_ptr=tptr;
        *end_ptr=eptr;
        return true;
      }

      sep_ptr++;
    }

    sptr=GetTokenStepThru(sptr, Flags);
  }

  *start_ptr=*end_ptr;

  return false;
}

//Once we've found our token we need to do various cleanups and post processing on it
const char *GetTokenPostProcess(const char *SearchStr, const char *SepStart, const char *SepEnd, char **Token, int Flags)
{
  const char *sptr, *eptr;

  if (! SepStart)
  {
    *Token=CopyStr(*Token, SearchStr);
    return(SearchStr+StrLen(SearchStr));
  }

  sptr=SearchStr;
  if (Flags & GETTOKEN_INCLUDE_SEP)
  {
    if (SepStart==SearchStr) eptr=SepEnd;
    else
    {
      eptr=SepStart;
      SepEnd=SepStart;
    }
  }
  else if (Flags & GETTOKEN_APPEND_SEP) eptr=SepEnd;
  else if (SepStart) eptr=SepStart;
  else eptr=SepEnd;

  if (Flags & GETTOKEN_STRIP_QUOTES)
  {
    if ((*sptr=='"') || (*sptr=='\''))
    {
      //is character before the sep a quote? If so, we copy one less char, and also start one character later
      //else we copy the characters as well
      eptr--;
      if (*sptr==*eptr) sptr++;
      else eptr++;
    }
  }

  if (eptr >= sptr) *Token=CopyStrLen(*Token, sptr, eptr-sptr);
  else *Token=CopyStr(*Token, sptr);

  if (Flags & GETTOKEN_STRIP_SPACE)
  {
    StripTrailingWhitespace(*Token);
    StripLeadingWhitespace(*Token);
  }

//return empty string, but not null
  if ((! SepEnd) || (*SepEnd=='\0'))
  {
    SepEnd=SearchStr+StrLen((char *) SearchStr);
  }

  return(SepEnd);
}

const char *GetTokenSeparators(const char *SearchStr, char **Separators, char **Token, int Flags)
{
  const char *SepStart=NULL, *SepEnd=NULL;

  /* this is a safety measure so that there is always something in Token*/
  if (Token) *Token=CopyStr(*Token,"");
  if ((! Token) || StrEnd(SearchStr)) return(NULL);
  SepStart=SearchStr;
  GetTokenMultiSepMatch(Separators, &SepStart, &SepEnd, Flags);
  return(GetTokenPostProcess(SearchStr, SepStart, SepEnd, Token, Flags));
}

const char *GetToken(const char *SearchStr, const char *Separator, char **Token, int Flags)
{
  const char *SepStart=NULL, *SepEnd=NULL;
  char **separators;
  int len;

  if (! Token) return(NULL);
  if (StrEnd(SearchStr))
  {
    *Token=CopyStr(*Token,"");
    return(NULL);
  }

  if (! StrValid(Separator))
  {
    *Token=CopyStr(*Token,SearchStr);
    return(NULL);
  }

//if we've only got one character than just keep it simple
  if (
          (! (Flags & GETTOKEN_HONOR_QUOTES)) &&
          (*(Separator+1)=='\0')
          )
  {
    SepStart=strchr(SearchStr,*Separator);
    if (! SepStart) SepStart=SearchStr+StrLen(SearchStr);
    if (*SepStart=='\0') SepEnd=SepStart;
    else SepEnd=SepStart+1;
    if (Flags) return(GetTokenPostProcess(SearchStr, SepStart, SepEnd, Token, Flags));
    *Token=CopyStrLen(*Token, SearchStr, SepStart-SearchStr);
    return(SepEnd);
  }
  else
  {
//right, now we're committed to doing a complex search
    *Token=CopyStr(*Token,"");


    if (Flags & GETTOKEN_MULTI_SEPARATORS)
    {
      separators=BuildMultiSeparators(Separator);
      SepStart=SearchStr;
      GetTokenMultiSepMatch(separators, &SepStart, &SepEnd, Flags);
      StringArrayDestroy(separators);
    }
    else GetTokenFindSeparator(Separator, SearchStr, &SepStart, &SepEnd, Flags);
  }

  return(GetTokenPostProcess(SearchStr, SepStart, SepEnd, Token, Flags));
}

int GetTokenParseConfig(const char *Config)
{
  const char *ptr;
  int Flags=0;

  for (ptr=Config; *ptr != '\0'; ptr++)
  {
    switch (*ptr)
    {
      case 'm':
        Flags |= GETTOKEN_MULTI_SEPARATORS;
        break;
      case 'q':
        Flags |= GETTOKEN_HONOR_QUOTES;
        break;
      case 'Q':
        Flags |= GETTOKEN_QUOTES;
        break;
      case 's':
        Flags |= GETTOKEN_INCLUDE_SEPARATORS;
        break;
      case '+':
        Flags |= GETTOKEN_APPEND_SEPARATORS;
        break;
    }
  }

  return(Flags);
}

const char *GetNameValuePair(const char *Input, const char *PairDelim, const char *NameValueDelim, char **Name, char **Value)
{
  const char *ptr, *ptr2;
  char *Token=NULL;

  *Name=CopyStr(*Name,"");
  *Value=CopyStr(*Value,"");
  ptr=GetToken(Input,PairDelim,&Token,GETTOKEN_HONOR_QUOTES);
  if (StrValid(Token))
  {
    if ((Token[0]=='"') || (Token[0]=='\''))
    {
      // StripQuotes(Token);
    }
    ptr2=GetToken(Token,NameValueDelim,Name,GETTOKEN_QUOTES);
//ptr2=GetToken(ptr2,PairDelim,Value,GETTOKEN_HONOR_QUOTES);
    *Value=CopyStr(*Value,ptr2);

    StripQuotes(*Name);
    StripQuotes(*Value);
  }

  DestroyString(Token);
  return(ptr);
}

char *GetNameValue(char *RetStr, const char *Input, const char *PairDelim, const char *NameValueDelim, const char *SearchName)
{
  char *Name=NULL, *Value=NULL;
  const char *ptr;

  RetStr=CopyStr(RetStr, "");
  ptr=GetNameValuePair(Input, PairDelim, NameValueDelim, &Name, &Value);
  while (ptr)
  {
    if (strcmp(Name, SearchName)==0) RetStr=CopyStr(RetStr, Value);
    ptr=GetNameValuePair(ptr, PairDelim, NameValueDelim, &Name, &Value);
  }

  Destroy(Name);
  Destroy(Value);

  return(RetStr);
}