#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "openshacccg/compiler.h"

namespace vsc {

struct Diagnostic {
    VscDiagnosticSeverity severity = VSC_DIAG_ERROR;
    uint32_t code = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string message;
};

struct FrontendOutput {
    std::vector<uint32_t> spirv;
    std::vector<Diagnostic> diagnostics;
};

struct BackendOutput {
    std::vector<uint8_t> gxp;
    std::vector<Diagnostic> diagnostics;
};

struct SpirvEntryPoint {
    uint32_t id = 0;
    uint32_t execution_model = 0;
    std::string name;
};

struct SpirvSummary {
    uint32_t version = 0;
    uint32_t generator = 0;
    uint32_t bound = 0;
    size_t instruction_count = 0;
    size_t variable_count = 0;
    size_t function_count = 0;
    std::vector<SpirvEntryPoint> entry_points;
};

bool cg_to_spirv(const VscCompileRequest &request, FrontendOutput &out);
bool spirv_to_gxp(VscStage stage, const char *entrypoint,
                  const uint32_t *words, size_t word_count,
                  BackendOutput &out);
bool parse_spirv(const uint32_t *words, size_t word_count,
                 SpirvSummary &summary, Diagnostic &error);

void *alloc(const VscAllocator &a, size_t size);
void dealloc(const VscAllocator &a, void *ptr);
char *dup_string(const VscAllocator &a, const std::string &s);

} // namespace vsc
