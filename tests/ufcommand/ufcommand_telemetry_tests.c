/*
 * White-box test for the telemetry seam.  The counters are queried through the
 * module's private header, because a host has no access to them: the public
 * interface is a single call, UfCommandRegistryDumpTelemetryJson().
 */
#include <uflib/ufcommand/ufcommand.h>
#include <uflib/ufcommand/ufcommand_telemetry.h>
#include "ufcommand_telemetry_priv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UfCommandStatus sHandler(UfCommandContext *c,const char*n,const UfCommandArg*a,size_t ac,void*u){(void)c;(void)n;(void)a;(void)ac;(void)u;return UFCOMMAND_OK;}
static void sCount(UfCommandTelemetry*t,const char*n,uint64_t expected){uint64_t v=99;assert(UfCommandTelemetryGetCommandCount(t,n,&v)==UFCOMMAND_OK);if(v!=expected)fprintf(stderr,"count %s got %llu expected %llu\n",n,(unsigned long long)v,(unsigned long long)expected);assert(v==expected);}
static void sAliasCount(UfCommandTelemetry*t,const char*n,uint64_t expected){uint64_t v=99;assert(UfCommandTelemetryGetAliasCount(t,n,&v)==UFCOMMAND_OK);assert(v==expected);}

static char *slurp(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f);
    assert(fseek(f,0,SEEK_END)==0); long n=ftell(f); assert(n>=0); assert(fseek(f,0,SEEK_SET)==0);
    char *b=malloc((size_t)n+1); assert(b);
    size_t got=fread(b,1,(size_t)n,f); assert(got==(size_t)n); b[n]='\0';
    fclose(f); return b;
}

int main(void){
    /* A registry that did not opt in has nothing to dump, and says so. */
    UfCommandRegistry *plain=NULL;
    assert(UfCommandRegistryCreate(NULL,&plain)==UFCOMMAND_OK);
    assert(UfCommandRegistryGetTelemetry(plain)==NULL);
    assert(UfCommandTelemetrySaveJson(NULL,"telemetry_should_not_exist.json")==UFCOMMAND_INVALID_ARGUMENT);
    /* The explicit seam, on its own registry: a host-owned observer attached,
     * detached, then re-attached.  Detaching stops the counting without
     * discarding what was counted. */
    UfCommandTelemetry *external=NULL;
    assert(UfCommandTelemetryCreate(&external)==UFCOMMAND_OK);
    assert(UfCommandRegistrySetTelemetry(plain,external)==UFCOMMAND_OK);
    assert(UfCommandRegistryGetTelemetry(plain)==external);
    assert(UfCommandRegistrySetTelemetry(plain,NULL)==UFCOMMAND_OK);    /* detach */
    assert(UfCommandRegistryGetTelemetry(plain)==NULL);
    assert(UfCommandRegistrySetTelemetry(plain,external)==UFCOMMAND_OK); /* resume */
    /* switching to an observer the registry created for itself must not leak the
     * host's: the registry destroys only what it owns */
    assert(UfCommandRegistrySetTelemetry(plain,NULL)==UFCOMMAND_OK);

    /* Destroy order for a host-owned observer: registry first, then the observer
     * it was borrowing. */
    UfCommandRegistryDestroy(plain);
    UfCommandTelemetryDestroy(external);

    UfCommandRegistryConfig cfg={0}; cfg.telemetry_enabled=true;
    UfCommandRegistry *r=NULL; UfCommandParser *p=NULL; UfCommandResult *res=NULL;
    assert(UfCommandRegistryCreate(&cfg,&r)==UFCOMMAND_OK);
    assert(UfCommandParserCreate(NULL,&p)==UFCOMMAND_OK);
    assert(UfCommandResultCreate(&res)==UFCOMMAND_OK);
    UfCommandTelemetry *t=UfCommandRegistryGetTelemetry(r);
    assert(t!=NULL);   /* the config asked the registry to create one */

    UfCommandDefinition remote={"remote","root",sHandler,NULL,0,SIZE_MAX}, add={"remote add","add",sHandler,NULL,1,2}, status={"status","status",sHandler,NULL,0,0};
    assert(UfCommandRegistryAdd(r,&remote)==UFCOMMAND_OK); assert(UfCommandRegistryAdd(r,&add)==UFCOMMAND_OK); assert(UfCommandRegistryAdd(r,&status)==UFCOMMAND_OK);
    UfCommandAliasDefinition co={"co","remote add $1"}; assert(UfCommandRegistryAddAlias(r,&co)==UFCOMMAND_OK);
    UfCommandBinding bind={UFCOMMAND_MOD_CTRL|UFCOMMAND_MOD_SHIFT,"P","status"}; assert(UfCommandRegistryAddBinding(r,&bind)==UFCOMMAND_OK);

    assert(UfCommandParserExecute(p,r,NULL,"remote add origin",res)==UFCOMMAND_OK);
    assert(UfCommandParserExecute(p,r,NULL,"co origin",res)==UFCOMMAND_OK);
    assert(UfCommandParserExecute(p,r,NULL,"remote add origin",res)==UFCOMMAND_OK);
    assert(UfCommandParserExecuteBinding(p,r,NULL,UFCOMMAND_MOD_CTRL|UFCOMMAND_MOD_SHIFT,"P",res)==UFCOMMAND_OK);

    /* every registered prefix of the match is counted, so 3 dispatches through
     * 'remote add' count both 'remote' and 'remote add' three times */
    sCount(t,"remote",3); sCount(t,"remote add",3); sCount(t,"status",1);
    sAliasCount(t,"co",1);
    uint64_t bc=0; assert(UfCommandTelemetryGetBindingCount(t,UFCOMMAND_MOD_CTRL|UFCOMMAND_MOD_SHIFT,"P",&bc)==UFCOMMAND_OK); assert(bc==1);

    /* the one public call */
    const char *path="ufcommand_telemetry_test.json";
    assert(UfCommandTelemetrySaveJson(t,path)==UFCOMMAND_OK);
    assert(UfCommandTelemetrySaveJson(NULL,path)==UFCOMMAND_INVALID_ARGUMENT);
    char *json=slurp(path);
    puts(json);
    assert(strstr(json,"\"remote\", \"count\": 3")!=NULL);
    assert(strstr(json,"\"remote add\", \"count\": 3")!=NULL);
    assert(strstr(json,"\"co\", \"count\": 1")!=NULL);
    assert(strstr(json,"\"key\": \"P\", \"count\": 1")!=NULL);
    free(json);
    remove(path);

    /* Reset clears the observations but keeps the dictionary, so a later query
     * succeeds with zero and the record still lists the names. */
    UfCommandTelemetryReset(t);
    sCount(t,"remote",0); sAliasCount(t,"co",0);
    assert(UfCommandTelemetryGetBindingCount(t,UFCOMMAND_MOD_CTRL|UFCOMMAND_MOD_SHIFT,"P",&bc)==UFCOMMAND_OK&&bc==0);

    /* The registry owns the observer: destroying it last is the only ordering
     * a host has to get right, and there is no separate object to leak. */
    UfCommandResultDestroy(res); UfCommandParserDestroy(p); UfCommandRegistryDestroy(r);
    return 0;
}
