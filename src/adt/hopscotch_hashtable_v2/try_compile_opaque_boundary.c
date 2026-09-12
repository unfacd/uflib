/**
 * @file
 * @brief Try-compile test for opaque boundary enforcement (Gate 2.8 / D-8).
 *
 * This file attempts to take sizeof(HopscotchHashTable).  If it compiles,
 * the opaque boundary is broken — consumer code can see the struct
 * definition.  The cmake try_compile in the parent CMakeLists.txt
 * expects this compilation to FAIL.
 *
 * DO NOT include _priv.h — this simulates what a consumer sees with
 * only the public header.
 */

#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>

int main(void)
{
    /* This must fail to compile — consumer cannot see the struct size. */
    (void)sizeof(HopscotchHashTable);
    return 0;
}
