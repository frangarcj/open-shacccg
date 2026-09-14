#pragma once

#include "backend/typed_ir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vsc {

bool spirv_cross_to_typed_fragment(const std::vector<uint32_t> &words,
                                   const char *entrypoint,
                                   backend::TypedProgram &typed,
                                   std::string &error);

} // namespace vsc
