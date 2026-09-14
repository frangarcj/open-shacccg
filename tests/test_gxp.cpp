#include "gxp/gxp_reader.hpp"
#include "gxp/gxp_writer.hpp"
#include "backend/program_builder.hpp"
#include "backend/vita_ir.hpp"
#include "usse/usse.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int fail(const char *msg) {
    std::fprintf(stderr, "test_gxp: %s\n", msg);
    return 1;
}

std::vector<uint8_t> load(const char *name) {
    std::string path = std::string(OPENSHACCG_SOURCE_DIR) +
        "/research/public_samples/libvita2d/" + name;
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Expected {
    const char *name;
    vsc::gxp::ProgramType type;
    uint32_t logical_size;
    uint32_t params;
    uint32_t primary;
    uint32_t secondary;
};
} // namespace

int test_gxp() {
    using namespace vsc::gxp;
    int failures = 0;
    const Expected samples[] = {
        {"clear_f.gxp", ProgramType::Fragment, 244, 1, 2, 1},
        {"clear_v.gxp", ProgramType::Vertex,   266, 1, 6, 0},
        {"color_f.gxp", ProgramType::Fragment, 216, 0, 2, 0},
        {"color_v.gxp", ProgramType::Vertex,   341, 3, 9, 0},
        {"texture_f.gxp", ProgramType::Fragment, 228, 1, 1, 0},
        {"texture_tint_f.gxp", ProgramType::Fragment, 295, 2, 5, 0},
        {"texture_v.gxp", ProgramType::Vertex, 344, 3, 9, 0},
    };

    for (const auto &exp : samples) {
        auto bytes = load(exp.name);
        if (bytes.empty()) { failures += fail("could not load public GXP sample"); continue; }
        ProgramView view(bytes.data(), bytes.size());
        if (!view.valid()) {
            std::fprintf(stderr, "test_gxp: %s rejected: %s\n", exp.name, view.error());
            ++failures;
            continue;
        }
        if (view.major_version() != 1 || view.minor_version() != 4)
            failures += fail("unexpected GXP version");
        if (view.type() != exp.type) failures += fail("wrong GXP stage");
        if (view.logical_size() != exp.logical_size) failures += fail("wrong GXP logical size");
        if (view.parameter_count() != exp.params) failures += fail("wrong GXP parameter count");
        if (view.primary_instruction_count() != exp.primary) failures += fail("wrong primary instruction count");
        if (view.secondary_instruction_count() != exp.secondary) failures += fail("wrong secondary instruction count");
        if (view.primary_program().size != exp.primary * ProgramView::kInstructionSize)
            failures += fail("wrong primary code range");
        if (view.secondary_program().size != exp.secondary * ProgramView::kInstructionSize)
            failures += fail("wrong secondary code range");
        if (view.varyings().size != 32) failures += fail("missing varying block");
    }

    {
        auto bytes = load("clear_f.gxp");
        ProgramView view(bytes.data(), bytes.size());
        ParameterView p;
        if (!view.parameter(0, p)) failures += fail("clear_f parameter parse failed");
        else {
            if (p.name != "uClearColor") failures += fail("clear_f parameter name mismatch");
            if (p.category != 1 || p.type != 0 || p.component_count != 4 || p.container_index != 14)
                failures += fail("clear_f parameter metadata mismatch");
            if (p.array_size != 1 || p.resource_index != 0)
                failures += fail("clear_f parameter layout mismatch");
        }
    }

    {
        auto bytes = load("texture_tint_f.gxp");
        ProgramView view(bytes.data(), bytes.size());
        ParameterView a, b;
        if (!view.parameter(0, a) || !view.parameter(1, b)) failures += fail("texture_tint parameters parse failed");
        else {
            if (a.name != "uTintColor" || b.name != "tex") failures += fail("texture_tint parameter names mismatch");
            if (a.category != 1 || b.category != 2) failures += fail("texture_tint parameter categories mismatch");
        }
    }

    {
        auto expected = load("clear_f.gxp");
        vsc::backend::FragmentIr ir;
        ir.uniforms = {{"uClearColor",0}};
        ir.binary_guid = 0xb0a7ca0e;
        ir.source_guid = 0x1f6aa3af;
        vsc::backend::IrCompileResult generated;
        if (!vsc::backend::compile_fragment_ir(ir,generated)) failures += fail("clear_f fragment IR compilation failed");
        else if (generated.gxp.size()!=expected.size() || std::memcmp(generated.gxp.data(),expected.data(),expected.size())!=0) {
            size_t first=0; while(first<generated.gxp.size() && first<expected.size() && generated.gxp[first]==expected[first]) ++first;
            std::fprintf(stderr,"test_gxp: clear_f fragment IR differs at 0x%zx (generated=%zu expected=%zu)\n",first,generated.gxp.size(),expected.size());
            ++failures;
        }
    }

    {
        auto expected = load("color_f.gxp");
        vsc::backend::FragmentIr ir;
        ir.op = vsc::backend::FragmentOpKind::VaryingColor;
        ir.binary_guid = 0x989d839a;
        ir.source_guid = 0x0027145a;
        vsc::backend::IrCompileResult generated;
        if (!vsc::backend::compile_fragment_ir(ir,generated)) failures += fail("color_f fragment IR compilation failed");
        else if (generated.gxp.size()!=expected.size() || std::memcmp(generated.gxp.data(),expected.data(),expected.size())!=0) {
            size_t first=0; while(first<generated.gxp.size() && first<expected.size() && generated.gxp[first]==expected[first]) ++first;
            std::fprintf(stderr,"test_gxp: color_f fragment IR differs at 0x%zx (generated=%zu expected=%zu)\n",first,generated.gxp.size(),expected.size());
            ++failures;
        }
    }

    {
        auto expected = load("texture_f.gxp");
        vsc::backend::FragmentIr ir;
        ir.op = vsc::backend::FragmentOpKind::Texture2D;
        ir.samplers = {{"tex",0}};
        ir.binary_guid = 0xa0cb639e;
        ir.source_guid = 0x6033c77b;
        vsc::backend::IrCompileResult generated;
        if (!vsc::backend::compile_fragment_ir(ir,generated)) failures += fail("texture_f fragment IR compilation failed");
        else if (generated.gxp.size()!=expected.size() || std::memcmp(generated.gxp.data(),expected.data(),expected.size())!=0) {
            size_t first=0; while(first<generated.gxp.size() && first<expected.size() && generated.gxp[first]==expected[first]) ++first;
            std::fprintf(stderr,"test_gxp: texture_f fragment IR differs at 0x%zx (generated=%zu expected=%zu)\n",first,generated.gxp.size(),expected.size());
            ++failures;
        }
    }

    {
        auto expected = load("texture_tint_f.gxp");
        vsc::backend::FragmentIr ir;
        ir.op = vsc::backend::FragmentOpKind::TextureTint2D;
        ir.uniforms = {{"uTintColor",0}};
        ir.samplers = {{"tex",0}};
        ir.binary_guid = 0x69742226;
        ir.source_guid = 0x35cc6eed;
        vsc::backend::IrCompileResult generated;
        if (!vsc::backend::compile_fragment_ir(ir,generated)) failures += fail("texture_tint_f fragment IR compilation failed");
        else if (generated.gxp.size()!=expected.size() || std::memcmp(generated.gxp.data(),expected.data(),expected.size())!=0) {
            size_t first=0; while(first<generated.gxp.size() && first<expected.size() && generated.gxp[first]==expected[first]) ++first;
            std::fprintf(stderr,"test_gxp: texture_tint_f fragment IR differs at 0x%zx (generated=%zu expected=%zu)\n",first,generated.gxp.size(),expected.size());
            ++failures;
        }
    }

    {
        vsc::backend::FragmentIr ordered;
        ordered.op = vsc::backend::FragmentOpKind::Arithmetic;
        ordered.uniforms = {{"uScale",0},{"uBias",4}};
        ordered.expressions = {
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Mul,0,1,4},
            {vsc::backend::FragmentExprKind::Uniform,1,0,4},
            {vsc::backend::FragmentExprKind::Add,2,3,4},
        };
        ordered.root_expression = 4;

        vsc::backend::FragmentIr shuffled = ordered;
        shuffled.expressions = {
            {vsc::backend::FragmentExprKind::Add,1,4,4},
            {vsc::backend::FragmentExprKind::Mul,2,3,4},
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,1,0,4},
        };
        shuffled.root_expression = 0;

        vsc::backend::IrCompileResult a, b;
        if (!vsc::backend::compile_fragment_ir(ordered,a) ||
            !vsc::backend::compile_fragment_ir(shuffled,b)) {
            failures += fail("arithmetic DAG allocation depends on topological storage order");
        } else if (a.gxp != b.gxp) {
            failures += fail("arithmetic DAG register allocation changed for equivalent node ordering");
        }

        vsc::backend::FragmentIr cyclic;
        cyclic.op = vsc::backend::FragmentOpKind::Arithmetic;
        cyclic.uniforms = {{"uBias",0}};
        cyclic.expressions = {
            {vsc::backend::FragmentExprKind::Add,0,1,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
        };
        cyclic.root_expression = 0;
        vsc::backend::IrCompileResult rejected;
        if (vsc::backend::compile_fragment_ir(cyclic,rejected))
            failures += fail("cyclic arithmetic DAG was accepted");
    }

    {
        uint8_t bad[ProgramView::kHeaderSize] = {};
        ProgramView view(bad, sizeof(bad));
        if (view.valid()) failures += fail("invalid magic accepted");
    }

    return failures;
}

// Keep this separate from the public-sample loop: it verifies that our
// independently written canonical serializer round-trips through our reader.
int test_gxp_writer() {
    using namespace vsc::gxp;
    int failures = 0;

    const uint64_t secondary[] = {0x40840d7ea0198002ULL};
    const uint64_t primary[] = {
        0xfa44070000000000ULL,
        0x38800422c5000000ULL,
    };
    const ParameterContainerDesc containers[] = {
        {14, 0, 0, 4},
        {19, 0, 4, 2},
    };
    const ParameterDesc parameters[] = {
        {"uClearColor", 1, 0, 4, 14, 0, 0, 1, 0},
    };
    uint8_t interface_block[32] = {};
    interface_block[10] = 1; // fragment output type, kept opaque by writer
    interface_block[11] = 4; // fragment output component count

    ProgramImage image;
    image.type = ProgramType::Fragment;
    image.binary_guid = 0x12345678;
    image.source_guid = 0x9abcdef0;
    image.primary_register_count = 2;
    image.secondary_register_count = 6;
    image.default_uniform_buffer_count = 2;
    image.interface_block = interface_block;
    image.interface_block_size = sizeof(interface_block);
    image.secondary_instructions = secondary;
    image.secondary_instruction_count = 1;
    image.primary_instructions = primary;
    image.primary_instruction_count = 2;
    image.containers = containers;
    image.container_count = 2;
    image.parameters = parameters;
    image.parameter_count = 1;

    const size_t need = required_size(image);
    if (!need) return fail("canonical GXP writer rejected valid description");
    std::vector<uint8_t> bytes(need);
    WriteResult wr;
    if (!write_program(image, bytes.data(), bytes.size(), &wr))
        return fail("canonical GXP writer failed");
    if (wr.physical_size != need || wr.logical_size > wr.physical_size)
        failures += fail("canonical GXP writer returned invalid sizes");

    ProgramView view(bytes.data(), bytes.size());
    if (!view.valid()) {
        std::fprintf(stderr, "test_gxp_writer: generated GXP rejected: %s\n", view.error());
        return failures + 1;
    }
    if (view.type() != ProgramType::Fragment || view.parameter_count() != 1 ||
        view.primary_instruction_count() != 2 || view.secondary_instruction_count() != 1)
        failures += fail("canonical GXP writer metadata did not round-trip");
    if (view.binary_guid() != image.binary_guid || view.source_guid() != image.source_guid)
        failures += fail("canonical GXP writer GUIDs did not round-trip");
    ParameterView p;
    if (!view.parameter(0, p) || p.name != "uClearColor" || p.container_index != 14 ||
        p.component_count != 4 || p.resource_index != 0)
        failures += fail("canonical GXP writer parameter did not round-trip");

    const auto primary_range = view.primary_program();
    if (primary_range.size != sizeof(primary) ||
        std::memcmp(primary_range.data, primary, sizeof(primary)) != 0)
        failures += fail("canonical GXP writer primary code did not round-trip");
    const auto secondary_range = view.secondary_program();
    if (secondary_range.size != sizeof(secondary) ||
        std::memcmp(secondary_range.data, secondary, sizeof(secondary)) != 0)
        failures += fail("canonical GXP writer secondary code did not round-trip");


    // Full independent texture_v reconstruction. Every operand-bearing USSE
    // instruction is assembled from semantic operands, then serialized from
    // structured GXP metadata. This is intentionally stronger than a parser
    // round-trip: the resulting public sample is expected byte-for-byte.
    {
        using namespace vsc::usse;
        vsc::backend::ProgramBuilder code;
        if (!code.phase()) failures += fail("texture_v PHAS assembly failed");

        VmovSemantic move{};
        move.dst = {RegisterBank::Output, 2};
        move.src = {RegisterBank::PrimaryAttribute, 2};
        move.data_type = DataType::F32;
        move.dest_mask = 3;
        move.swizzle = 4;
        move.skip_invalid = true;
        move.no_schedule = true;
        if (!code.instruction(move)) failures += fail("texture_v VMOV assembly failed");

        VpckSemantic p0{};
        p0.dst = {RegisterBank::Temp, 124};
        p0.src1 = {RegisterBank::PrimaryAttribute, 0};
        p0.src2 = {RegisterBank::PrimaryAttribute, 1};
        p0.src_format = PackFormat::F32;
        p0.dst_format = PackFormat::F32;
        p0.dest_mask = 7;
        p0.skip_invalid = true;
        p0.no_schedule = true;
        if (!code.instruction(p0)) failures += fail("texture_v first VPCK assembly failed");

        VpckSemantic p1{};
        p1.dst = {RegisterBank::Temp, 125};
        p1.src1 = {RegisterBank::SecondaryAttribute, 6};
        p1.src2 = {RegisterBank::SecondaryAttribute, 7};
        p1.src_format = PackFormat::F32;
        p1.dst_format = PackFormat::F32;
        p1.dest_mask = 15;
        p1.skip_invalid = true;
        p1.no_schedule = true;
        if (!code.instruction(p1)) failures += fail("texture_v second VPCK assembly failed");

        VmadSemantic mad[4]{};
        for (auto &m : mad) {
            m.gpi0 = 0;
            m.gpi1 = 1;
            m.vec4 = true;
            m.repeat_mode = RepeatMode::Slmsi;
            m.skip_invalid = true;
            m.src1_swizzle = {{SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W}};
            m.gpi1_swizzle = {{SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W}};
        }
        mad[0].dst = {RegisterBank::Temp, 61};
        mad[0].src1 = {RegisterBank::SecondaryAttribute, 0};
        mad[0].write_mask = 15;
        mad[0].no_schedule = true;
        mad[0].gpi0_swizzle = {{SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X}};
        mad[1] = mad[0];
        mad[1].src1.num = 2;
        mad[1].gpi0_swizzle = {{SwizzleChannel::Y, SwizzleChannel::Y, SwizzleChannel::Y, SwizzleChannel::Y}};
        mad[2] = mad[0];
        mad[2].dst = {RegisterBank::Output, 0};
        mad[2].src1.num = 4;
        mad[2].write_mask = 3;
        mad[2].no_schedule = false;
        mad[2].gpi0_swizzle = {{SwizzleChannel::Z, SwizzleChannel::Z, SwizzleChannel::Z, SwizzleChannel::Z}};
        mad[3] = mad[2];
        mad[3].dst.num = 1;
        mad[3].src1.num = 5;
        mad[3].gpi1_swizzle = {{SwizzleChannel::Z, SwizzleChannel::W, SwizzleChannel::Z, SwizzleChannel::W}};
        for (const auto &m : mad)
            if (!code.instruction(m)) failures += fail("texture_v VMAD assembly failed");
        if (!code.emit()) failures += fail("texture_v EMIT assembly failed");

        const uint8_t texture_interface[32] = {
            0x37,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
            0x00,0x10,0x00,0x06,0x01,0,0,0, 0,0,0,0, 0,0,0,0
        };
        const ParameterContainerDesc texture_containers[] = {
            {14, 0, 0, 16},
            {19, 0, 16, 2},
        };
        const ParameterDesc texture_params[] = {
            {"aPosition", 0, 0, 4, 0, 0, 0, 1, 0},
            {"aTexcoord", 0, 0, 4, 0, 0, 0, 1, 4},
            {"wvp",       1, 0, 4, 14, 0, 0, 4, 0},
        };

        ProgramImage texture{};
        texture.type = ProgramType::Vertex;
        texture.binary_guid = 0xfa8e24ac;
        texture.source_guid = 0x049b2668;
        texture.program_flags = 0x00010000;
        texture.buffer_flags = 0x10000000;
        texture.primary_register_count = 8;
        texture.secondary_register_count = 18;
        texture.data_buffer_count = 2;
        texture.default_uniform_buffer_count = 16;
        texture.compiler_version_raw = 16;
        texture.interface_block = texture_interface;
        texture.interface_block_size = sizeof(texture_interface);
        texture.primary_instructions = code.words().data();
        texture.primary_instruction_count = code.words().size();
        texture.containers = texture_containers;
        texture.container_count = 2;
        texture.parameters = texture_params;
        texture.parameter_count = 3;

        const size_t texture_need = required_size(texture);
        std::vector<uint8_t> generated(texture_need);
        WriteResult texture_wr{};
        if (!write_program(texture, generated.data(), generated.size(), &texture_wr)) {
            failures += fail("texture_v full semantic GXP write failed");
        } else {
            auto known = load("texture_v.gxp");
            if (generated.size() != known.size() ||
                std::memcmp(generated.data(), known.data(), known.size()) != 0) {
                failures += fail("texture_v full semantic GXP is not byte-identical to public sample");
            }
            ProgramView generated_view(generated.data(), generated.size());
            if (!generated_view.valid() || generated_view.primary_instruction_count() != 9 ||
                generated_view.parameter_count() != 3 || generated_view.primary_register_count() != 8 ||
                generated_view.secondary_register_count() != 18)
                failures += fail("texture_v generated GXP semantic metadata mismatch");
        }
    }


    // One level above semantic USSE: describe the shader as Vita IR operations
    // and require the backend to allocate/register-lower it into the same GXP.
    {
        vsc::backend::VertexIr ir;
        ir.binary_guid = 0xfa8e24ac;
        ir.source_guid = 0x049b2668;
        ir.attributes = {
            {"aPosition", 3, 0},
            {"aTexcoord", 2, 4},
        };
        ir.matrices = {{"wvp", 0}};
        ir.ops = {
            {vsc::backend::IrOpKind::TransformPosition, 0, 0},
            {vsc::backend::IrOpKind::CopyVarying, 1, 0},
        };

        vsc::backend::IrCompileResult lowered;
        if (!vsc::backend::compile_vertex_ir(ir, lowered)) {
            std::fprintf(stderr, "test_gxp: IR lowering failed: %s\n", lowered.error.c_str());
            ++failures;
        } else {
            const auto known = load("texture_v.gxp");
            if (lowered.gxp.size() != known.size() ||
                std::memcmp(lowered.gxp.data(), known.data(), known.size()) != 0)
                failures += fail("texture_v IR-lowered GXP is not byte-identical to public sample");
        }

        // Same allocator/lowering path for color_v: only logical varying width
        // and semantic differ from texture_v.
        ir.binary_guid = 0x0f4c3f6b;
        ir.source_guid = 0x41e359f5;
        ir.attributes[1] = {"aColor", 4, 4};
        ir.ops[1].semantic = vsc::backend::IrVaryingSemantic::Color;
        if (!vsc::backend::compile_vertex_ir(ir, lowered)) {
            failures += fail("color_v IR lowering failed");
        } else {
            const auto known_color = load("color_v.gxp");
            if (lowered.gxp.size()!=known_color.size() || std::memcmp(lowered.gxp.data(),known_color.data(),known_color.size())!=0)
                failures += fail("color_v IR-lowered GXP is not byte-identical to public sample");
        }

        // clear_v exercises the no-uniform construction path: float2 -> xy11.
        vsc::backend::VertexIr clear_ir;
        clear_ir.binary_guid=0x6bb0ce7e; clear_ir.source_guid=0xd84a6f2c;
        clear_ir.attributes={{"aPosition",2,0}};
        clear_ir.ops={{vsc::backend::IrOpKind::ConstructPosition,0,0}};
        if (!vsc::backend::compile_vertex_ir(clear_ir, lowered)) {
            std::fprintf(stderr,"test_gxp: clear_v IR lowering failed: %s\n",lowered.error.c_str()); ++failures;
        } else {
            const auto known_clear=load("clear_v.gxp");
            if (lowered.gxp.size()!=known_clear.size() || std::memcmp(lowered.gxp.data(),known_clear.data(),known_clear.size())!=0)
                failures += fail("clear_v IR-lowered GXP is not byte-identical to public sample");
        }

        // The initial IR is deliberately fail-closed for graph shapes whose
        // allocation rules have not been validated yet.
        ir.ops.pop_back();
        if (vsc::backend::compile_vertex_ir(ir, lowered))
            failures += fail("unsupported incomplete vertex IR was accepted");
    }

    return failures;
}
