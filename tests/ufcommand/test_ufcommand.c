#include <uflib/ufcommand/ufcommand.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { int calls; char last[256]; } State;
static UfCommandStatus handler(UfCommandContext *ctx,const char *name,const UfCommandArg *args,size_t n,void *ud){(void)ctx;(void)name;State*s=ud;s->calls++;s->last[0]='\0';for(size_t i=0;i<n;i++){if(i)strcat(s->last,"|");strcat(s->last,args[i].value);}return UFCOMMAND_OK;}
static void check(UfCommandStatus got,UfCommandStatus want){if(got!=want){fprintf(stderr,"expected %s got %s\n",UfCommandStatusName(want),UfCommandStatusName(got));exit(1);}}
static void test_golden(UfCommandParser*p,UfCommandRegistry*r,const char*path,State*s){FILE*f=fopen(path,"r");assert(f);char line[1024],expect[512];while(fgets(line,sizeof(line),f)){if(line[0]=='#'||line[0]=='\n')continue;char*sep=strchr(line,'\t');assert(sep);*sep='\0';strcpy(expect,sep+1);expect[strcspn(expect,"\r\n")]='\0';UfCommandResult *res=NULL;check(UfCommandResultCreate(&res),UFCOMMAND_OK);UfCommandStatus st=UfCommandParserExecute(p,r,NULL,line,res);char actual[512];snprintf(actual,sizeof(actual),"%s;%s;%zu;%s",UfCommandStatusName(st),UfCommandResultGetResolvedCommand(res)?UfCommandResultGetResolvedCommand(res):"-",UfCommandResultGetArgCount(res),s->last);if(strcmp(actual,expect)!=0){fprintf(stderr,"golden mismatch\ninput=%s\nwant=%s\ngot =%s\n",line,expect,actual);exit(1);}UfCommandResultDestroy(res);}fclose(f);}
int main(int argc,char**argv){UfCommandRegistry*r=NULL;UfCommandParser*p=NULL;State s={0};check(UfCommandRegistryCreate(NULL,&r),UFCOMMAND_OK);check(UfCommandParserCreate(NULL,&p),UFCOMMAND_OK);
UfCommandDefinition defs[]={{"status","",handler,&s,0,SIZE_MAX},{"remote add","",handler,&s,2,2},{"checkout","",handler,&s,1,1},{"echo","",handler,&s,0,SIZE_MAX},{"build","",handler,&s,0,SIZE_MAX}};for(size_t i=0;i<sizeof(defs)/sizeof(defs[0]);i++)check(UfCommandRegistryAdd(r,&defs[i]),UFCOMMAND_OK);
UfCommandAliasDefinition as[]={{"st","status"},{"co","checkout $1"},{"ra","remote add $1 $2"},{"say","echo $*"}};for(size_t i=0;i<sizeof(as)/sizeof(as[0]);i++)check(UfCommandRegistryAddAlias(r,&as[i]),UFCOMMAND_OK);
UfCommandBinding b={UFCOMMAND_MOD_CTRL|UFCOMMAND_MOD_SHIFT,"P","status"};check(UfCommandRegistryAddBinding(r,&b),UFCOMMAND_OK);
UfCommandResult *res=NULL;check(UfCommandResultCreate(&res),UFCOMMAND_OK);check(UfCommandParserExecuteBinding(p,r,NULL,b.modifiers,b.key,res),UFCOMMAND_OK);assert(strcmp(UfCommandResultGetResolvedCommand(res),"status")==0);UfCommandResultReset(res);
check(UfCommandParserExecute(p,r,NULL,"remote add origin git@example.com",res),UFCOMMAND_OK);assert(strcmp(s.last,"origin|git@example.com")==0);UfCommandResultReset(res);
check(UfCommandParserExecute(p,r,NULL,"co main",res),UFCOMMAND_OK);assert(strcmp(s.last,"main")==0);UfCommandResultReset(res);
if(argc>1)test_golden(p,r,argv[1],&s);

/* Persistence: the checked-in Lua-compatible golden file can be loaded into
 * a fresh registry and then emitted deterministically for a round-trip. */
UfCommandRegistry *loaded = NULL;
check(UfCommandRegistryCreate(NULL, &loaded), UFCOMMAND_OK);
check(UfCommandRegistryLoadUserConfig(loaded, argc > 2 ? argv[2] : (argc > 1 ? "tests/golden_user_config.lua" : "golden_user_config.lua")), UFCOMMAND_OK);
UfCommandKeyConfig kc;
check(UfCommandRegistryGetKeyConfig(loaded, &kc), UFCOMMAND_OK);
assert(kc.default_modifiers == (UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT));
UfCommandResultReset(res);
check(UfCommandParserExecute(p, loaded, NULL, "co persisted", res), UFCOMMAND_NOT_FOUND);
/* Aliases are attached to the registry; commands remain host-owned. */
UfCommandDefinition pdefs[] = {
    {"checkout", "", handler, &s, 1, 1},
    {"remote add", "", handler, &s, 2, 2},
    {"echo", "", handler, &s, 0, SIZE_MAX},
    {"status", "", handler, &s, 0, 0}
};
for(size_t i=0;i<sizeof(pdefs)/sizeof(pdefs[0]);i++) check(UfCommandRegistryAdd(loaded,&pdefs[i]),UFCOMMAND_OK);
check(UfCommandParserExecute(p, loaded, NULL, "co persisted", res), UFCOMMAND_OK);
assert(strcmp(s.last,"persisted")==0);
check(UfCommandParserExecuteDefaultBinding(p, loaded, NULL, "P", res), UFCOMMAND_OK);

const char *roundtrip = ".ufcommand_roundtrip.lua";
check(UfCommandRegistrySaveUserConfig(loaded, roundtrip), UFCOMMAND_OK);
UfCommandRegistry *reloaded = NULL;
check(UfCommandRegistryCreate(NULL, &reloaded), UFCOMMAND_OK);
check(UfCommandRegistryLoadUserConfig(reloaded, roundtrip), UFCOMMAND_OK);
remove(roundtrip);
UfCommandRegistryDestroy(reloaded);
UfCommandRegistryDestroy(loaded);
/* Dynamic insertion: force many resizes and verify exact lookup. */
char names[256][32];for(size_t i=0;i<256;i++){snprintf(names[i],sizeof(names[i]),"cmd%zu",i);UfCommandDefinition d={names[i],"",handler,&s,0,0};check(UfCommandRegistryAdd(r,&d),UFCOMMAND_OK);}for(size_t i=0;i<256;i++){UfCommandDefinition d;check(UfCommandRegistryFind(r,names[i],&d),UFCOMMAND_OK);assert(strcmp(d.name,names[i])==0);}UfCommandResultDestroy(res);UfCommandRegistryDestroy(r);UfCommandParserDestroy(p);puts("ufcommand tests: PASS");return 0;}
