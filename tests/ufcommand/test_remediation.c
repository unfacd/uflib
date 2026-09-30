#include "ufcommand/ufcommand.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UfCommandStatus ok_handler(UfCommandContext *ctx, const char *name, const UfCommandArg *args, size_t argc, void *ud) {
    (void)ctx; (void)name; (void)args; (void)argc;
    int *calls = ud; if (calls) ++*calls; return UFCOMMAND_OK;
}
static UfCommandStatus err_handler(UfCommandContext *ctx, const char *name, const UfCommandArg *args, size_t argc, void *ud) {
    (void)ctx; (void)name; (void)args; (void)argc; (void)ud; return UFCOMMAND_HANDLER_ERROR;
}
static UfCommandStatus mutation_handler(UfCommandContext *ctx, const char *name, const UfCommandArg *args, size_t argc, void *ud) {
    (void)ctx; (void)name; (void)args; (void)argc;
    UfCommandRegistry *r = ud;
    return UfCommandRegistryRemove(r, "status");
}
static void expect(UfCommandStatus got, UfCommandStatus want) { if (got != want) { fprintf(stderr, "want %s got %s\n", UfCommandStatusName(want), UfCommandStatusName(got)); abort(); } }
static void write_file(const char *path, const char *text) { FILE *f=fopen(path,"wb"); assert(f); assert(fputs(text,f)>=0); assert(fclose(f)==0); }

int main(void) {
    UfCommandRegistry *r=NULL; UfCommandParser *p=NULL; UfCommandResult *res=NULL;
    expect(UfCommandRegistryCreate(NULL,&r),UFCOMMAND_OK);
    expect(UfCommandParserCreate(NULL,&p),UFCOMMAND_OK);
    expect(UfCommandResultCreate(&res),UFCOMMAND_OK);
    int calls=0;
    UfCommandDefinition checkout={"checkout","",ok_handler,&calls,1,1};
    UfCommandDefinition echo={"echo","",ok_handler,&calls,0,SIZE_MAX};
    UfCommandDefinition status={"status","",ok_handler,&calls,0,0};
    expect(UfCommandRegistryAdd(r,&checkout),UFCOMMAND_OK);
    expect(UfCommandRegistryAdd(r,&echo),UFCOMMAND_OK);
    expect(UfCommandRegistryAdd(r,&status),UFCOMMAND_OK);

    /* Alias chain owns every intermediate token buffer; cycle is identity based. */
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"a","b"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"b","checkout main"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"a",res),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"a"),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"a","b"}),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"b"),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"b","a"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"a",res),UFCOMMAND_ALIAS_CYCLE);

    /* Configurable depth is explicit rather than silent truncation. */
    UfCommandParser *p2=NULL; UfCommandParserConfig pc={64,1024,'"','\\',2}; expect(UfCommandParserCreate(&pc,&p2),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"a"),UFCOMMAND_OK); expect(UfCommandRegistryRemoveAlias(r,"b"),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"a","b"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"b","c"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"c","checkout main"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p2,r,NULL,"a",res),UFCOMMAND_ALIAS_DEPTH_EXCEEDED);
    UfCommandParserDestroy(p2);
    expect(UfCommandRegistryRemoveAlias(r,"a"),UFCOMMAND_OK); expect(UfCommandRegistryRemoveAlias(r,"b"),UFCOMMAND_OK); expect(UfCommandRegistryRemoveAlias(r,"c"),UFCOMMAND_OK);

    /* A positional placeholder reached in the SECOND or later expansion round used
     * to free the token vector the caller still held, making the chain a double free.
     * Only a sanitizer build sees this, and only if a chain carrying $n is exercised --
     * every fixture above expands straight to a command, so the old corpus missed it.
     * $* in the intermediate link is what carries the arguments through to the
     * placeholder, which is the shape real chains use. */
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-a","chain-b $*"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-b","checkout $1"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"chain-a main",res),UFCOMMAND_OK);
    assert(UfCommandResultGetArgCount(res)==1 && strcmp(UfCommandResultGetArgs(res)[0].value,"main")==0);
    expect(UfCommandParserExecute(p,r,NULL,"chain-a",res),UFCOMMAND_PARSE_ERROR);
    expect(UfCommandRegistryRemoveAlias(r,"chain-b"),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-b","echo $1 $2"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"chain-a p q",res),UFCOMMAND_OK);
    assert(UfCommandResultGetArgCount(res)==2);
    expect(UfCommandRegistryRemoveAlias(r,"chain-b"),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-b","nosuch $1"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"chain-a p",res),UFCOMMAND_NOT_FOUND);

    /* The same failure one round deeper, and a bare chain that cannot satisfy $1. */
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-x","chain-y $*"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-y","chain-z $*"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"chain-z","checkout $1"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"chain-x a b",res),UFCOMMAND_OK);
    assert(UfCommandResultGetArgCount(res)==1 && strcmp(UfCommandResultGetArgs(res)[0].value,"a")==0);
    expect(UfCommandParserExecute(p,r,NULL,"chain-x",res),UFCOMMAND_PARSE_ERROR);
    expect(UfCommandRegistryRemoveAlias(r,"chain-x"),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"chain-y"),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"chain-z"),UFCOMMAND_OK);
    expect(UfCommandRegistryRemoveAlias(r,"chain-a"),UFCOMMAND_OK);

    /* Add stores the canonical form of a command name, so every entry point that
     * takes a caller-supplied name has to resolve it the same way. */
    expect(UfCommandRegistryAdd(r,&(UfCommandDefinition){"canon  ical","",ok_handler,&calls,0,0}),UFCOMMAND_OK);
    UfCommandDefinition found;
    expect(UfCommandRegistryFind(r,"canon ical",&found),UFCOMMAND_OK);
    expect(UfCommandRegistryFind(r,"canon  ical",&found),UFCOMMAND_OK);
    expect(UfCommandRegistryFind(r,"  canon ical  ",&found),UFCOMMAND_OK);
    expect(UfCommandRegistryFind(r,"canon",&found),UFCOMMAND_NOT_FOUND);
    expect(UfCommandParserExecute(p,r,NULL,"canon    ical",res),UFCOMMAND_OK);
    expect(UfCommandRegistryRemove(r,"canon ical"),UFCOMMAND_OK);
    expect(UfCommandRegistryRemove(r,"canon ical"),UFCOMMAND_NOT_FOUND);

    /* $10 is not a documented placeholder; it remains literal. */
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"literal","echo $10"}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,NULL,"literal",res),UFCOMMAND_OK);
    assert(UfCommandResultGetArgCount(res)==1 && strcmp(UfCommandResultGetArgs(res)[0].value,"$10")==0);
    expect(UfCommandRegistryRemoveAlias(r,"literal"),UFCOMMAND_OK);

    /* Command-path matching has no 4096-byte join truncation and no four-token cap. */
    char long_name[6000]; size_t pos=0; const char *parts[]={"aaaa","bbbb","cccc","dddd","eeee"};
    for(size_t i=0;i<5;++i){if(i)long_name[pos++]=' '; size_t n=strlen(parts[i]); memcpy(long_name+pos,parts[i],n);pos+=n;} long_name[pos]='\0';
    expect(UfCommandRegistryAdd(r,&(UfCommandDefinition){long_name,"",ok_handler,&calls,1,1}),UFCOMMAND_OK);
    char input[6100]; snprintf(input,sizeof(input),"%s arg",long_name);
    expect(UfCommandParserExecute(p,r,NULL,input,res),UFCOMMAND_OK);
    assert(UfCommandResultGetArgCount(res)==1);

    /* Alias/command namespace conflict and NULL removal arguments are explicit. */
    expect(UfCommandRegistryAddAlias(r,&(UfCommandAliasDefinition){"status2","status"}),UFCOMMAND_OK);
    expect(UfCommandRegistryAdd(r,&(UfCommandDefinition){"status2","",ok_handler,&calls,0,0}),UFCOMMAND_CONFLICT);
    expect(UfCommandRegistryRemove(r,NULL),UFCOMMAND_INVALID_ARGUMENT);
    expect(UfCommandRegistryRemoveAlias(r,NULL),UFCOMMAND_INVALID_ARGUMENT);
    expect(UfCommandRegistryRemoveBinding(r,UFCOMMAND_MOD_NONE,NULL),UFCOMMAND_INVALID_ARGUMENT);

    /* Context seam is usable and handler status is distinct from dispatch status. */
    UfCommandContext *ctx=NULL; int marker=7; expect(UfCommandContextCreate(&(UfCommandContextConfig){&marker},&ctx),UFCOMMAND_OK); assert(UfCommandContextGetUserData(ctx)==&marker);
    expect(UfCommandRegistryAdd(r,&(UfCommandDefinition){"fail","",err_handler,NULL,0,0}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,ctx,"fail",res),UFCOMMAND_HANDLER_ERROR);
    assert(UfCommandResultGetDispatchStatus(res)==UFCOMMAND_OK); assert(UfCommandResultGetHandlerStatus(res)==UFCOMMAND_HANDLER_ERROR); assert(UfCommandResultGetStatus(res)==UFCOMMAND_HANDLER_ERROR);
    expect(UfCommandRegistryAdd(r,&(UfCommandDefinition){"mutate","",mutation_handler,r,0,0}),UFCOMMAND_OK);
    expect(UfCommandParserExecute(p,r,ctx,"mutate",res),UFCOMMAND_CONFLICT);
    UfCommandContextDestroy(ctx);

    /* Binding lookup has a distinct NO_BINDING result. */
    expect(UfCommandParserExecuteBinding(p,r,NULL,UFCOMMAND_MOD_CTRL,"Z",res),UFCOMMAND_NO_BINDING);

    /* Version validation and truncated hex escape are parser errors, not successful loads. */
    write_file("ufcommand_bad_version.lua","return { version = 99, aliases = {}, bindings = {} }\n");
    expect(UfCommandRegistryLoadUserConfig(r,"ufcommand_bad_version.lua"),UFCOMMAND_UNSUPPORTED_VERSION);
    remove("ufcommand_bad_version.lua");
    write_file("ufcommand_bad_hex.lua", "return { version = 1, aliases = { [\"x\"] = \"echo \\xA\" }, bindings = {} }\n");
    expect(UfCommandRegistryLoadUserConfig(r,"ufcommand_bad_hex.lua"),UFCOMMAND_CONFIG_ERROR);
    remove("ufcommand_bad_hex.lua");

    /* Transactionality: a later conflict must not leave the earlier alias applied. */
    expect(UfCommandRegistryAddBinding(r,&(UfCommandBinding){UFCOMMAND_MOD_CTRL,"Q","status"}),UFCOMMAND_OK);
    write_file("ufcommand_transaction.lua","return { version = 1, aliases = { [\"staged\"] = \"status\" }, bindings = { { modifiers = { \"CTRL\" }, key = \"Q\", command = \"echo\" } } }\n");
    expect(UfCommandRegistryLoadUserConfig(r,"ufcommand_transaction.lua"),UFCOMMAND_DUPLICATE);
    remove("ufcommand_transaction.lua");
    expect(UfCommandParserExecute(p,r,NULL,"staged",res),UFCOMMAND_NOT_FOUND);

    const char *save="ufcommand_atomic.lua";
    write_file(save,"ORIGINAL\n");
    expect(UfCommandRegistrySaveUserConfig(r,save),UFCOMMAND_OK);
    FILE *sf=fopen(save,"rb"); assert(sf); fclose(sf);
    remove(save);

    UfCommandResultDestroy(res); UfCommandParserDestroy(p); UfCommandRegistryDestroy(r);
    puts("ufcommand remediation tests: PASS"); return 0;
}
