#include "openshacccg/shacccg_compat.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static void *tmalloc(unsigned int n) { return std::malloc(n); }
static void tfree(void *p) { std::free(p); }

int test_abi() {
    int fail = 0;
    SceShaccCgCompileOptions o;
    std::memset(&o, 0xA5, sizeof(o));
    if (sceShaccCgInitializeCompileOptions(&o) != 0) ++fail;
    if (!o.entryFunctionName || std::strcmp(o.entryFunctionName, "main") != 0) ++fail;
    if (o.targetProfile != SCE_SHACCCG_PROFILE_VP) ++fail;
    if (o.optimizationLevel != 3) ++fail;
    if (o.useFastmath != 1 || o.useFastprecision != 0 || o.useFastint != 1) ++fail;
    if (o.performanceWarnings != 1) ++fail;
    // The reference initializer only clears 0x60 bytes. The final two words
    // are intentionally left untouched.
    if (o.field_60 != static_cast<int>(0xA5A5A5A5u)) ++fail;
    if (o.field_64 != static_cast<int>(0xA5A5A5A5u)) ++fail;
    SceShaccCgCallbackList c;
    std::memset(&c, 0xA5, sizeof(c));
    sceShaccCgInitializeCallbackList(&c, SCE_SHACCCG_TRIVIAL);
    if (c.openFile || c.releaseFile || c.locateFile || c.absolutePath || c.releaseFileName || c.fileDate) ++fail;
    if (sceShaccCgSetDefaultAllocator(tmalloc, tfree) != 0) ++fail;
    if (!sceShaccCgGetVersionString()) ++fail;
    if (fail) std::printf("test_abi: %d failures\n", fail);
    return fail;
}
