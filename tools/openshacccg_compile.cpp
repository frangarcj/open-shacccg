#include "openshacccg/compiler.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " --stage vertex|fragment [--entry main] input.cg output.gxp\n";
}

bool write_binary(const std::string &path, const uint8_t *data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return file.good();
}

} // namespace

int main(int argc, char **argv) {
    const char *entry = "main";
    VscStage stage = VSC_STAGE_FRAGMENT;
    bool have_stage = false;
    std::string input_path;
    std::string output_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stage" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "fragment") stage = VSC_STAGE_FRAGMENT;
            else if (value == "vertex") stage = VSC_STAGE_VERTEX;
            else { usage(argv[0]); return 2; }
            have_stage = true;
        } else if (arg == "--entry" && i + 1 < argc) {
            entry = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (input_path.empty()) {
            input_path = arg;
        } else if (output_path.empty()) {
            output_path = arg;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!have_stage || input_path.empty() || output_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open input: " << input_path << "\n";
        return 2;
    }
    const std::string source(std::istreambuf_iterator<char>(input), {});

    VscCompileRequest request{};
    request.source_name = input_path.c_str();
    request.source = source.data();
    request.source_size = source.size();
    request.entrypoint = entry;
    request.stage = stage;
    request.optimization_level = 3;
    request.fast_math = 1;
    request.fast_int = 1;

    VscCompileResult result{};
    const int rc = vsc_compile(&request, &result);
    for (size_t i = 0; i < result.diagnostic_count; ++i) {
        const auto &d = result.diagnostics[i];
        std::cerr << static_cast<unsigned>(d.severity) << ":0x" << std::hex << d.code
                  << std::dec << ":" << d.line << ":" << d.column << ": "
                  << (d.message ? d.message : "") << "\n";
    }

    if (rc != 0 || !result.gxp_data || !result.gxp_size) {
        vsc_destroy_result(&request.allocator, &result);
        return rc == 0 ? 1 : rc;
    }
    if (!write_binary(output_path, result.gxp_data, result.gxp_size)) {
        std::cerr << "cannot write output: " << output_path << "\n";
        vsc_destroy_result(&request.allocator, &result);
        return 2;
    }

    vsc_destroy_result(&request.allocator, &result);
    return 0;
}
