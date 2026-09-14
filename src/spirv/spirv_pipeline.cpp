#include "spirv/spirv_pipeline.hpp"

#include <sstream>
#include <string>

#if defined(OPENSHACCG_ENABLE_SPIRV_TOOLS)
#include <spirv-tools/optimizer.hpp>
#endif

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
#include <spirv_cross/spirv_cross.hpp>
#endif

namespace vsc {
namespace {

#if defined(OPENSHACCG_ENABLE_SPIRV_TOOLS)
bool optimize_spirv(const uint32_t *words, size_t word_count,
                    std::vector<uint32_t> &optimized, std::string &why) {
    spvtools::Optimizer optimizer(SPV_ENV_UNIVERSAL_1_6);
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &pos,
                                     const char *message) {
        std::ostringstream ss;
        ss << "SPIRV-Tools";
        if (pos.line || pos.column || pos.index)
            ss << " [" << pos.line << ':' << pos.column << ':' << pos.index << ']';
        ss << ": " << (message ? message : "optimization failed");
        why = ss.str();
    });
    optimizer.RegisterPerformancePasses(true);
    spvtools::OptimizerOptions options;
    options.set_run_validator(true);
    options.set_preserve_bindings(true);
    options.set_preserve_spec_constants(true);
    if (!optimizer.Run(words, word_count, &optimized, options)) {
        if (why.empty()) why = "SPIRV-Tools optimization failed";
        return false;
    }
    return true;
}
#endif

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
bool validate_with_spirv_cross(VscStage stage, const char *entrypoint,
                               const std::vector<uint32_t> &words, std::string &why) {
    try {
        spirv_cross::Compiler compiler(words);
        const auto model = stage == VSC_STAGE_FRAGMENT ? spv::ExecutionModelFragment : spv::ExecutionModelVertex;
        const std::string name = (entrypoint && *entrypoint) ? entrypoint : "main";
        bool found = false;
        for (const auto &ep : compiler.get_entry_points_and_stages()) {
            if (ep.name == name && ep.execution_model == model) {
                found = true;
                break;
            }
        }
        // Preserve the backend's stable "entry point not found" diagnostic.
        // Cross is used here to parse/reflect a matching entry point when one
        // exists, not to replace stage selection policy.
        if (found) {
            compiler.set_entry_point(name, model);
            (void)compiler.get_shader_resources();
        }
        return true;
    } catch (const std::exception &e) {
        why = std::string("SPIRV-Cross: ") + e.what();
        return false;
    }
}
#endif

} // namespace

bool prepare_spirv(VscStage stage, const char *entrypoint,
                   const uint32_t *words, size_t word_count,
                   PreparedSpirv &out, Diagnostic &error) {
    out = {};
    error = {};
    if (!words || word_count < 5) {
        error = {VSC_DIAG_ERROR, 0x2101, 0, 0, "short SPIR-V module"};
        return false;
    }

    SpirvSummary structural_summary;
    Diagnostic structural_error;
    if (!parse_spirv(words, word_count, structural_summary, structural_error)) {
        error = std::move(structural_error);
        return false;
    }

#if defined(OPENSHACCG_ENABLE_SPIRV_TOOLS)
    std::string why;
    if (!optimize_spirv(words, word_count, out.words, why)) {
        error = {VSC_DIAG_ERROR, 0x2102, 0, 0, why};
        return false;
    }
    out.optimized = true;
#else
    out.words.assign(words, words + word_count);
#endif

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    std::string cross_why;
    if (!validate_with_spirv_cross(stage, entrypoint, out.words, cross_why)) {
        error = {VSC_DIAG_ERROR, 0x2103, 0, 0, cross_why};
        return false;
    }
    out.cross_validated = true;
#endif

    return true;
}

} // namespace vsc
