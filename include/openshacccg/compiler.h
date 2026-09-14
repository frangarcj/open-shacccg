#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VscStage {
    VSC_STAGE_VERTEX = 0,
    VSC_STAGE_FRAGMENT = 1
} VscStage;

typedef enum VscDiagnosticSeverity {
    VSC_DIAG_INFO = 0,
    VSC_DIAG_WARNING = 1,
    VSC_DIAG_ERROR = 2
} VscDiagnosticSeverity;

typedef struct VscAllocator {
    void *(*alloc)(size_t size, void *userdata);
    void (*free)(void *ptr, void *userdata);
    void *userdata;
} VscAllocator;

typedef struct VscCompileRequest {
    const char *source_name;
    const char *source;
    size_t source_size;
    const char *entrypoint;
    VscStage stage;
    int optimization_level;
    int fast_math;
    int fast_precision;
    int fast_int;
    VscAllocator allocator;
} VscCompileRequest;

typedef struct VscSpirvRequest {
    const uint32_t *words;
    size_t word_count;
    const char *entrypoint;
    VscStage stage;
    int optimization_level;
    int fast_math;
    int fast_precision;
    int fast_int;
    VscAllocator allocator;
} VscSpirvRequest;

typedef struct VscDiagnostic {
    VscDiagnosticSeverity severity;
    uint32_t code;
    uint32_t line;
    uint32_t column;
    const char *message;
} VscDiagnostic;

typedef struct VscCompileResult {
    uint8_t *gxp_data;
    size_t gxp_size;
    VscDiagnostic *diagnostics;
    size_t diagnostic_count;
} VscCompileResult;

/* Compile Cg-compatible source through the configured frontend and Vita backend. */
int vsc_compile(const VscCompileRequest *request, VscCompileResult *result);

/*
 * Backend-development entry point. This deliberately bypasses the Cg frontend
 * so USSE/GXP work can progress before glslang/Cg compatibility is complete.
 */
int vsc_compile_spirv(const VscSpirvRequest *request, VscCompileResult *result);

void vsc_destroy_result(const VscAllocator *allocator, VscCompileResult *result);
const char *vsc_version_string(void);

#ifdef __cplusplus
}
#endif
