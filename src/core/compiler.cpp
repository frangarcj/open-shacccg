#include "openshacccg/compiler.h"
#include "core/internal.hpp"
#include <cstring>
#include <vector>

static void clear_result(VscCompileResult *r) {
    if (r) std::memset(r, 0, sizeof(*r));
}

static int publish_result(const VscAllocator &allocator,
                          bool ok,
                          const vsc::BackendOutput &back,
                          const std::vector<vsc::Diagnostic> &diagnostics,
                          VscCompileResult *result) {
    if (!back.gxp.empty()) {
        result->gxp_data = static_cast<uint8_t *>(vsc::alloc(allocator, back.gxp.size()));
        if (!result->gxp_data) return -2;
        std::memcpy(result->gxp_data, back.gxp.data(), back.gxp.size());
        result->gxp_size = back.gxp.size();
    }

    if (!diagnostics.empty()) {
        result->diagnostics = static_cast<VscDiagnostic *>(
            vsc::alloc(allocator, diagnostics.size() * sizeof(VscDiagnostic)));
        if (!result->diagnostics) {
            vsc_destroy_result(&allocator, result);
            return -2;
        }
        std::memset(result->diagnostics, 0, diagnostics.size() * sizeof(VscDiagnostic));
        result->diagnostic_count = diagnostics.size();
        for (size_t i = 0; i < diagnostics.size(); ++i) {
            const auto &d = diagnostics[i];
            result->diagnostics[i].severity = d.severity;
            result->diagnostics[i].code = d.code;
            result->diagnostics[i].line = d.line;
            result->diagnostics[i].column = d.column;
            result->diagnostics[i].message = vsc::dup_string(allocator, d.message);
            if (!result->diagnostics[i].message) {
                vsc_destroy_result(&allocator, result);
                return -2;
            }
        }
    }
    return ok ? 0 : 1;
}

extern "C" const char *vsc_version_string(void) {
    return "open-shacccg 0.2.0-research";
}

extern "C" int vsc_compile(const VscCompileRequest *request, VscCompileResult *result) {
    if (!result) return -1;
    clear_result(result);
    if (!request) return -1;

    vsc::FrontendOutput front;
    vsc::BackendOutput back;
    bool ok = vsc::cg_to_spirv(*request, front);
    if (ok) {
        ok = vsc::spirv_to_gxp(request->stage, request->entrypoint,
                               front.spirv.data(), front.spirv.size(), back);
    }

    std::vector<vsc::Diagnostic> all;
    all.reserve(front.diagnostics.size() + back.diagnostics.size());
    all.insert(all.end(), front.diagnostics.begin(), front.diagnostics.end());
    all.insert(all.end(), back.diagnostics.begin(), back.diagnostics.end());
    return publish_result(request->allocator, ok, back, all, result);
}

extern "C" int vsc_compile_spirv(const VscSpirvRequest *request, VscCompileResult *result) {
    if (!result) return -1;
    clear_result(result);
    if (!request || !request->words || request->word_count == 0) return -1;

    vsc::BackendOutput back;
    const bool ok = vsc::spirv_to_gxp(request->stage, request->entrypoint,
                                      request->words, request->word_count, back);
    return publish_result(request->allocator, ok, back, back.diagnostics, result);
}

extern "C" void vsc_destroy_result(const VscAllocator *allocator, VscCompileResult *result) {
    if (!result) return;
    VscAllocator a{};
    if (allocator) a = *allocator;
    if (result->diagnostics) {
        for (size_t i = 0; i < result->diagnostic_count; ++i)
            vsc::dealloc(a, const_cast<char *>(result->diagnostics[i].message));
        vsc::dealloc(a, result->diagnostics);
    }
    vsc::dealloc(a, result->gxp_data);
    clear_result(result);
}
