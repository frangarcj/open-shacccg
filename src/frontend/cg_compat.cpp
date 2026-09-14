#include "core/internal.hpp"

namespace vsc {

bool cg_to_spirv(const VscCompileRequest &request, FrontendOutput &out) {
    if (!request.source || request.source_size == 0) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1001, 0, 0, "empty shader source"});
        return false;
    }
    if (!request.entrypoint || !*request.entrypoint) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1002, 0, 0, "missing shader entry point"});
        return false;
    }

#if defined(OPENSHACCG_ENABLE_GLSLANG)
    // Integration point: pinned glslang HLSL frontend. The Cg compatibility
    // layer should normalize only Vita/Cg-specific syntax and semantics before
    // feeding glslang; it must not become a second optimizer/backend.
    out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10FE, 0, 0,
        "glslang adapter enabled but not vendored in this bootstrap snapshot"});
    return false;
#else
    out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10FF, 0, 0,
        "Cg frontend unavailable: build with the pinned glslang adapter"});
    return false;
#endif
}

} // namespace vsc
