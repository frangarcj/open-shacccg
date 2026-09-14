#include "openshacccg/shacccg_compat.h"
#include "openshacccg/compiler.h"
#include <cstdlib>
#include <cstring>

#if UINTPTR_MAX == 0xffffffffu
static_assert(sizeof(SceShaccCgSourceFile) == 0x0c, "SceShaccCgSourceFile ABI drift");
static_assert(sizeof(SceShaccCgSourceLocation) == 0x0c, "SceShaccCgSourceLocation ABI drift");
static_assert(sizeof(SceShaccCgCallbackList) == 0x18, "SceShaccCgCallbackList ABI drift");
static_assert(sizeof(SceShaccCgCompileOptions) == 0x68, "SceShaccCgCompileOptions ABI drift");
static_assert(sizeof(SceShaccCgDiagnosticMessage) == 0x10, "SceShaccCgDiagnosticMessage ABI drift");
static_assert(sizeof(SceShaccCgCompileOutput) == 0x10, "SceShaccCgCompileOutput ABI drift");
#endif

namespace {
using SonyMalloc = void *(*)(unsigned int);
using SonyFree = void (*)(void *);
SonyMalloc g_malloc = [](unsigned int n) -> void * { return std::malloc(n); };
SonyFree g_free = [](void *p) { std::free(p); };

struct OwnedOutput {
    SceShaccCgCompileOutput pub{};
    VscCompileResult core{};
    SceShaccCgDiagnosticMessage *diags = nullptr;
    SceShaccCgSourceLocation *locations = nullptr;
    SceShaccCgSourceFile source{};
    char *source_name_owned = nullptr;
};

void *abi_alloc(size_t n) { return g_malloc ? g_malloc(static_cast<unsigned int>(n)) : nullptr; }
void abi_free(void *p) { if (p && g_free) g_free(p); }
void *core_alloc(size_t n, void *) { return abi_alloc(n); }
void core_free(void *p, void *) { abi_free(p); }

OwnedOutput *new_output() {
    auto *p = static_cast<OwnedOutput *>(abi_alloc(sizeof(OwnedOutput)));
    if (p) std::memset(p, 0, sizeof(*p));
    return p;
}

void fill_diagnostics(OwnedOutput *o) {
    const size_t n = o->core.diagnostic_count;
    if (!n) return;
    o->diags = static_cast<SceShaccCgDiagnosticMessage *>(abi_alloc(n * sizeof(*o->diags)));
    o->locations = static_cast<SceShaccCgSourceLocation *>(abi_alloc(n * sizeof(*o->locations)));
    if (!o->diags || !o->locations) return;
    std::memset(o->diags, 0, n * sizeof(*o->diags));
    std::memset(o->locations, 0, n * sizeof(*o->locations));
    for (size_t i = 0; i < n; ++i) {
        const VscDiagnostic &d = o->core.diagnostics[i];
        o->locations[i].file = &o->source;
        o->locations[i].lineNumber = d.line;
        o->locations[i].columnNumber = d.column;
        o->diags[i].level = static_cast<SceShaccCgDiagnosticLevel>(d.severity);
        o->diags[i].code = d.code;
        o->diags[i].location = (d.line || d.column) ? &o->locations[i] : nullptr;
        o->diags[i].message = d.message;
    }
    o->pub.diagnosticCount = static_cast<int32_t>(n);
    o->pub.diagnostics = o->diags;
}
}

extern "C" int sceShaccCgSetDefaultAllocator(void *(*malloc_cb)(unsigned int), void (*free_cb)(void *)) {
    if (!malloc_cb || !free_cb) return -1;
    g_malloc = malloc_cb;
    g_free = free_cb;
    return 0;
}

extern "C" int sceShaccCgInitializeCompileOptions(SceShaccCgCompileOptions *options) {
    if (!options) return -1;

    // Reference SceShaccCg (PSM SDK 1.6.5) clears exactly the first 0x60 bytes.
    // The two trailing words exposed by the current VitaSDK reconstruction are
    // not touched by the original initializer. Preserve that quirk for ABI
    // compatibility instead of blindly memset(sizeof(*options)).
    std::memset(options, 0, 0x60);
    options->entryFunctionName = "main";
    options->targetProfile = SCE_SHACCCG_PROFILE_VP;
    options->optimizationLevel = 3;
    options->useFastmath = 1;
    options->useFastprecision = 0;
    options->useFastint = 1;
    options->performanceWarnings = 1;
    return 0;
}

extern "C" void sceShaccCgInitializeCallbackList(SceShaccCgCallbackList *callbacks, SceShaccCgCallbackDefaults) {
    if (!callbacks) return;
    std::memset(callbacks, 0, sizeof(*callbacks));
}

extern "C" const SceShaccCgCompileOutput *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options, const SceShaccCgCallbackList *callbacks, int) {
    OwnedOutput *o = new_output();
    if (!o) return nullptr;

    if (!options || !callbacks || !callbacks->openFile || !options->mainSourceFile) {
        // Route through the core so allocation/lifetime stays identical.
        VscCompileRequest bad{};
        bad.source_name = options && options->mainSourceFile ? options->mainSourceFile : "<unknown>";
        bad.source = nullptr;
        bad.source_size = 0;
        bad.entrypoint = options && options->entryFunctionName ? options->entryFunctionName : "main";
        bad.stage = VSC_STAGE_VERTEX;
        bad.allocator = {core_alloc, core_free, nullptr};
        vsc_compile(&bad, &o->core);
        fill_diagnostics(o);
        return &o->pub;
    }

    const char *open_error = nullptr;
    SceShaccCgSourceFile *src = callbacks->openFile(options->mainSourceFile, nullptr, options, &open_error);
    if (!src || !src->text) {
        VscCompileRequest bad{};
        bad.source_name = options->mainSourceFile;
        bad.entrypoint = options->entryFunctionName ? options->entryFunctionName : "main";
        bad.stage = options->targetProfile == SCE_SHACCCG_PROFILE_FP ? VSC_STAGE_FRAGMENT : VSC_STAGE_VERTEX;
        bad.allocator = {core_alloc, core_free, nullptr};
        vsc_compile(&bad, &o->core);
        fill_diagnostics(o);
        if (src && callbacks->releaseFile) callbacks->releaseFile(src, options);
        return &o->pub;
    }

    o->source = *src;
    const char *stable_name = src->fileName ? src->fileName : options->mainSourceFile;
    if (stable_name) {
        const size_t stable_name_len = std::strlen(stable_name);
        o->source_name_owned = static_cast<char *>(abi_alloc(stable_name_len + 1));
        if (o->source_name_owned) {
            std::memcpy(o->source_name_owned, stable_name, stable_name_len + 1);
            o->source.fileName = o->source_name_owned;
        }
    }
    VscCompileRequest req{};
    req.source_name = src->fileName ? src->fileName : options->mainSourceFile;
    req.source = src->text;
    req.source_size = src->size;
    req.entrypoint = options->entryFunctionName ? options->entryFunctionName : "main";
    req.stage = options->targetProfile == SCE_SHACCCG_PROFILE_FP ? VSC_STAGE_FRAGMENT : VSC_STAGE_VERTEX;
    req.optimization_level = options->optimizationLevel;
    req.fast_math = options->useFastmath;
    req.fast_precision = options->useFastprecision;
    req.fast_int = options->useFastint;
    req.allocator = {core_alloc, core_free, nullptr};

    vsc_compile(&req, &o->core);
    o->pub.programData = o->core.gxp_data;
    o->pub.programSize = static_cast<uint32_t>(o->core.gxp_size);
    fill_diagnostics(o);

    if (callbacks->releaseFile) callbacks->releaseFile(src, options);
    return &o->pub;
}

extern "C" void sceShaccCgDestroyCompileOutput(const SceShaccCgCompileOutput *output) {
    if (!output) return;
    auto *o = reinterpret_cast<OwnedOutput *>(const_cast<SceShaccCgCompileOutput *>(output));
    VscAllocator a{core_alloc, core_free, nullptr};
    vsc_destroy_result(&a, &o->core);
    abi_free(o->locations);
    abi_free(o->diags);
    abi_free(o->source_name_owned);
    abi_free(o);
}

extern "C" void sceShaccCgReleaseCompiler(void) {
    // Stateless bootstrap. Future frontend caches belong behind this call.
}

extern "C" const char *sceShaccCgGetVersionString(void) {
    return vsc_version_string();
}
