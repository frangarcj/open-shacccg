#include "openshacccg/shacccg_compat.h"
#include <cstdio>
#include <cstring>

static SceShaccCgSourceFile g_source;
static SceShaccCgSourceFile *open_cb(const char *, const SceShaccCgSourceLocation *, const SceShaccCgCompileOptions *, const char **) {
    return &g_source;
}

int test_compiler() {
    const char shader[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";
    g_source.fileName = "test.cg";
    g_source.text = shader;
    g_source.size = static_cast<uint32_t>(std::strlen(shader));

    SceShaccCgCompileOptions o{};
    SceShaccCgCallbackList c{};
    sceShaccCgInitializeCompileOptions(&o);
    sceShaccCgInitializeCallbackList(&c, SCE_SHACCCG_TRIVIAL);
    o.mainSourceFile = g_source.fileName;
    o.targetProfile = SCE_SHACCCG_PROFILE_VP;
    c.openFile = open_cb;

    const SceShaccCgCompileOutput *out = sceShaccCgCompileProgram(&o, &c, 0);
    if (!out) return 1;
    // Bootstrap is expected to fail cleanly until glslang + USSE backend land.
    int fail = (out->programData != nullptr) || (out->diagnosticCount <= 0) || !out->diagnostics;
    sceShaccCgDestroyCompileOutput(out);
    if (fail) std::printf("test_compiler: unexpected bootstrap result\n");
    return fail;
}
