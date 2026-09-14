#include "core/internal.hpp"
#include "backend/typed_ir.hpp"
#include "backend/vita_ir.hpp"
#include "spirv/spirv_cross_adapter.hpp"
#include "spirv/spirv_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace vsc {
namespace {
constexpr uint32_t kExecutionModelVertex = 0;
constexpr uint32_t kExecutionModelFragment = 4;

constexpr uint16_t kOpName = 5;
constexpr uint16_t kOpEntryPoint = 15;
constexpr uint16_t kOpTypeFloat = 22;
constexpr uint16_t kOpTypeVector = 23;
constexpr uint16_t kOpTypeMatrix = 24;
constexpr uint16_t kOpTypeImage = 25;
constexpr uint16_t kOpTypeSampledImage = 27;
constexpr uint16_t kOpTypePointer = 32;
constexpr uint16_t kOpConstant = 43;
constexpr uint16_t kOpFunction = 54;
constexpr uint16_t kOpFunctionEnd = 56;
constexpr uint16_t kOpVariable = 59;
constexpr uint16_t kOpLoad = 61;
constexpr uint16_t kOpStore = 62;
constexpr uint16_t kOpDecorate = 71;
constexpr uint16_t kOpCompositeConstruct = 80;
constexpr uint16_t kOpImageSampleImplicitLod = 87;
constexpr uint16_t kOpFNegate = 127;
constexpr uint16_t kOpFAdd = 129;
constexpr uint16_t kOpFSub = 131;
constexpr uint16_t kOpFMul = 133;
constexpr uint16_t kOpMatrixTimesVector = 145;
constexpr uint16_t kOpDot = 148;

constexpr uint32_t kStorageUniformConstant = 0;
constexpr uint32_t kStorageInput = 1;
constexpr uint32_t kStorageUniform = 2;
constexpr uint32_t kStorageOutput = 3;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kBuiltInPosition = 0;

std::string literal_string(const uint32_t *words, size_t count) {
    const char *bytes = reinterpret_cast<const char *>(words);
    const size_t max_bytes = count * sizeof(uint32_t);
    size_t n = 0;
    while (n < max_bytes && bytes[n]) ++n;
    if (n == max_bytes) return {};
    return std::string(bytes, n);
}

struct TypeInfo {
    enum class Kind { Unknown, Float, Vector, Matrix, Image, SampledImage, Pointer } kind = Kind::Unknown;
    uint32_t element = 0;
    uint32_t count = 0;
    uint32_t storage = 0;
};
struct DecorationInfo {
    bool has_location = false;
    uint32_t location = 0;
    bool is_position = false;
};
struct VarInfo {
    uint32_t type = 0;
    uint32_t storage = 0;
};
struct ValueInfo {
    enum class Kind { Unknown, Load, Composite, ImageSample, FNegate, FAdd, FSub, FMul, Dot, MatrixTimesVector, ConstantOne } kind = Kind::Unknown;
    uint32_t type = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    std::vector<uint32_t> constituents;
};
struct StoreInfo { uint32_t pointer = 0, object = 0; };

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

static uint32_t pointee_type(const std::vector<TypeInfo> &types, uint32_t pointer_type) {
    if (pointer_type >= types.size() || types[pointer_type].kind != TypeInfo::Kind::Pointer) return 0;
    return types[pointer_type].element;
}

static uint8_t vector_components(const std::vector<TypeInfo> &types, uint32_t type_id) {
    if (type_id >= types.size()) return 0;
    const auto &t = types[type_id];
    if (t.kind == TypeInfo::Kind::Vector && t.count >= 1 && t.count <= 4) return static_cast<uint8_t>(t.count);
    if (t.kind == TypeInfo::Kind::Float) return 1;
    return 0;
}

static backend::TypedType typed_f32(uint8_t components) {
    switch (components) {
    case 1: return backend::TypedType::F32;
    case 2: return backend::TypedType::F32x2;
    case 3: return backend::TypedType::F32x3;
    case 4: return backend::TypedType::F32x4;
    default: return backend::TypedType::Invalid;
    }
}

static uint32_t loaded_variable(const std::vector<ValueInfo> &values, uint32_t id) {
    if (id >= values.size() || values[id].kind != ValueInfo::Kind::Load) return 0;
    return values[id].a;
}

static bool is_one(const std::vector<ValueInfo> &values, uint32_t id) {
    return id < values.size() && values[id].kind == ValueInfo::Kind::ConstantOne;
}

bool lower_vertex_subset(const uint32_t *words, size_t word_count,
                         uint32_t entry_function, backend::TypedShader &typed,
                         std::string &why) {
    typed = backend::TypedShader(backend::TypedStage::Vertex);
    if (!words || word_count < 5 || words[3] == 0) { why = "invalid SPIR-V module"; return false; }
    const uint32_t bound = words[3];
    std::vector<TypeInfo> types(bound);
    std::vector<DecorationInfo> decorations(bound);
    std::vector<VarInfo> vars(bound);
    std::vector<ValueInfo> values(bound);
    std::vector<std::string> names(bound);
    std::vector<StoreInfo> stores;

    bool in_selected_function = false;
    size_t cursor = 5;
    while (cursor < word_count) {
        const uint32_t first = words[cursor];
        const uint16_t wc = static_cast<uint16_t>(first >> 16);
        const uint16_t op = static_cast<uint16_t>(first & 0xffffu);
        if (!wc || cursor + wc > word_count) { why = "malformed instruction while lowering"; return false; }
        auto id_ok = [bound](uint32_t id){ return id < bound; };

        switch (op) {
        case kOpName:
            if (wc >= 3 && id_ok(words[cursor+1])) names[words[cursor+1]] = literal_string(words+cursor+2, wc-2);
            break;
        case kOpDecorate:
            if (wc >= 3 && id_ok(words[cursor+1])) {
                const uint32_t target=words[cursor+1], deco=words[cursor+2];
                if (deco==kDecorationLocation && wc>=4) { decorations[target].has_location=true; decorations[target].location=words[cursor+3]; }
                else if (deco==kDecorationBuiltIn && wc>=4 && words[cursor+3]==kBuiltInPosition) decorations[target].is_position=true;
            }
            break;
        case kOpTypeFloat:
            if (wc>=3 && id_ok(words[cursor+1]) && words[cursor+2]==32) types[words[cursor+1]]={TypeInfo::Kind::Float,0,32,0};
            break;
        case kOpTypeVector:
            if (wc==4 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Vector,words[cursor+2],words[cursor+3],0};
            break;
        case kOpTypeMatrix:
            if (wc==4 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Matrix,words[cursor+2],words[cursor+3],0};
            break;
        case kOpTypePointer:
            if (wc==4 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Pointer,words[cursor+3],0,words[cursor+2]};
            break;
        case kOpVariable:
            if (wc>=4 && id_ok(words[cursor+2])) vars[words[cursor+2]]={words[cursor+1],words[cursor+3]};
            break;
        case kOpConstant:
            if (wc==4 && id_ok(words[cursor+2])) {
                float f=0.0f; uint32_t bits=words[cursor+3]; std::memcpy(&f,&bits,sizeof(f));
                if (f==1.0f) values[words[cursor+2]]={ValueInfo::Kind::ConstantOne,words[cursor+1],0,0,{}};
            }
            break;
        case kOpFunction:
            if (wc>=5) in_selected_function = words[cursor+2] == entry_function;
            break;
        case kOpFunctionEnd:
            in_selected_function = false;
            break;
        case kOpLoad:
            if (in_selected_function && wc>=4 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::Load,words[cursor+1],words[cursor+3],0,{}};
            break;
        case kOpCompositeConstruct:
            if (in_selected_function && wc>=4 && id_ok(words[cursor+2])) {
                ValueInfo v; v.kind=ValueInfo::Kind::Composite; v.type=words[cursor+1];
                v.constituents.assign(words+cursor+3, words+cursor+wc); values[words[cursor+2]]=std::move(v);
            }
            break;
        case kOpMatrixTimesVector:
            if (in_selected_function && wc==5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::MatrixTimesVector,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpStore:
            if (in_selected_function && wc>=3) stores.push_back({words[cursor+1],words[cursor+2]});
            break;
        default: break;
        }
        cursor += wc;
    }

    struct InputRef { uint32_t var=0, location=0; uint8_t components=0; std::string name; };
    std::vector<InputRef> inputs;
    uint32_t uniform_var=0;
    for (uint32_t id=1; id<bound; ++id) {
        if (vars[id].storage==kStorageInput && decorations[id].has_location) {
            const uint32_t pt=pointee_type(types,vars[id].type); const uint8_t comps=vector_components(types,pt);
            if (!comps) { why="input is not a supported float vector"; return false; }
            inputs.push_back({id,decorations[id].location,comps,names[id]});
        } else if (vars[id].storage==kStorageUniform) {
            if (uniform_var) { why="multiple uniforms are not supported yet"; return false; }
            uniform_var=id;
        }
    }
    std::sort(inputs.begin(),inputs.end(),[](const InputRef&a,const InputRef&b){return a.location<b.location;});
    if (inputs.empty()) { why="no located vertex inputs"; return false; }
    auto &program = typed.program();
    std::vector<backend::TypedValue> input_values;
    input_values.reserve(inputs.size());
    for (const auto &in: inputs) {
        if (in.location > 31) { why="input location is too large for initial allocator"; return false; }
        const auto type=typed_f32(in.components);
        if (type==backend::TypedType::Invalid) { why="input has unsupported float width"; return false; }
        const auto value=program.input(type,static_cast<uint16_t>(in.location));
        const std::string input_name=in.name.empty() ? ("attr"+std::to_string(in.location)) : in.name;
        if (value.kind()==backend::TypedValueKind::None ||
            typed.add_resource(backend::TypedResourceKind::Input,value,type,input_name,
                               static_cast<uint16_t>(in.location))==std::numeric_limits<uint16_t>::max()) {
            why="failed to create portable Typed vertex input"; return false;
        }
        input_values.push_back(value);
    }
    auto attr_index_for_var = [&inputs](uint32_t var)->int {
        for(size_t i=0;i<inputs.size();++i) if(inputs[i].var==var) return static_cast<int>(i); return -1;
    };

    const StoreInfo *position_store=nullptr, *varying_store=nullptr;
    for (const auto &st: stores) {
        if (st.pointer>=bound || vars[st.pointer].storage!=kStorageOutput) continue;
        if (decorations[st.pointer].is_position) position_store=&st;
        else if (decorations[st.pointer].has_location) {
            if (varying_store) { why="multiple varying stores are not supported yet"; return false; }
            varying_store=&st;
        }
    }
    if (!position_store) { why="no BuiltIn Position store found"; return false; }

    const std::string position_name=names[position_store->pointer].empty()?"position":names[position_store->pointer];
    const uint16_t position_output=typed.add_resource(backend::TypedResourceKind::Output,{},backend::TypedType::F32x4,
                                                       position_name,0,backend::TypedSemantic::Position,0);
    if (position_output==std::numeric_limits<uint16_t>::max()) { why="failed to create position output resource"; return false; }

    const ValueInfo &pv = position_store->object<bound ? values[position_store->object] : ValueInfo{};
    if (pv.kind == ValueInfo::Kind::Composite) {
        // Validated clear_v shape: vec4(load float2, 1.0, 1.0).
        if (pv.constituents.size()!=3 || !is_one(values,pv.constituents[1]) || !is_one(values,pv.constituents[2])) { why="unsupported constructed position"; return false; }
        const uint32_t posvar=loaded_variable(values,pv.constituents[0]); const int ai=attr_index_for_var(posvar);
        if (ai<0 || inputs[ai].components!=2 || inputs.size()!=1 || uniform_var || varying_store) { why="constructed position does not match validated clear_v shape"; return false; }
        const auto position=program.make_value<backend::TypedType::F32x4>();
        if (position.kind()==backend::TypedValueKind::None ||
            !program.emit<backend::TypedOpcode::ConstructPosition>(0,position,input_values[ai]) ||
            !program.emit<backend::TypedOpcode::StoreOutput>(0,{},position,{},position_output)) {
            why="failed to emit portable Typed constructed position"; return false;
        }
        return true;
    }

    if (pv.kind != ValueInfo::Kind::MatrixTimesVector || !uniform_var || !varying_store) { why="unsupported position expression"; return false; }
    const uint32_t matrix_var=loaded_variable(values,pv.a);
    if (matrix_var != uniform_var) { why="matrix operand is not the supported uniform"; return false; }
    if (pv.b>=bound || values[pv.b].kind!=ValueInfo::Kind::Composite) { why="matrix vector operand must be a composite"; return false; }
    const auto &vc=values[pv.b].constituents;
    if (vc.size()!=2 || !is_one(values,vc[1])) { why="matrix vector must be vec4(position.xyz,1)"; return false; }
    const uint32_t posvar=loaded_variable(values,vc[0]); const int posai=attr_index_for_var(posvar);
    if (posai<0 || inputs[posai].components!=3) { why="matrix position input must be float3"; return false; }

    const uint32_t passvar=loaded_variable(values,varying_store->object); const int passai=attr_index_for_var(passvar);
    if (passai<0 || passai==posai) { why="varying store must directly copy the second input"; return false; }
    const std::string lname=lower_ascii(inputs[passai].name);
    backend::IrVaryingSemantic semantic;
    if (lname.find("color")!=std::string::npos) semantic=backend::IrVaryingSemantic::Color;
    else if (lname.find("tex")!=std::string::npos || lname.find("uv")!=std::string::npos) semantic=backend::IrVaryingSemantic::TexCoord;
    else { why="cannot infer initial varying semantic from input name"; return false; }

    const uint32_t mtype=pointee_type(types,vars[uniform_var].type);
    if (mtype>=types.size() || types[mtype].kind!=TypeInfo::Kind::Matrix || types[mtype].count!=4 || vector_components(types,types[mtype].element)!=4) { why="uniform is not mat4"; return false; }
    const std::string matrix_name=names[uniform_var].empty()?"wvp":names[uniform_var];
    const uint16_t matrix_resource=typed.add_resource(backend::TypedResourceKind::Matrix4,{},backend::TypedType::F32x4,
                                                       matrix_name,0);
    const auto constructed=program.make_value<backend::TypedType::F32x4>();
    const auto transformed=program.make_value<backend::TypedType::F32x4>();
    if (matrix_resource==std::numeric_limits<uint16_t>::max() ||
        constructed.kind()==backend::TypedValueKind::None || transformed.kind()==backend::TypedValueKind::None ||
        !program.emit<backend::TypedOpcode::ConstructPosition>(0,constructed,input_values[posai]) ||
        !program.emit<backend::TypedOpcode::TransformPosition>(0,transformed,constructed,{},matrix_resource) ||
        !program.emit<backend::TypedOpcode::StoreOutput>(0,{},transformed,{},position_output)) {
        why="failed to emit portable Typed matrix position"; return false;
    }

    const auto typed_semantic=semantic==backend::IrVaryingSemantic::Color ?
        backend::TypedSemantic::Color : backend::TypedSemantic::TexCoord;
    const auto pass_type=typed_f32(inputs[passai].components);
    const std::string output_name=names[varying_store->pointer].empty()?inputs[passai].name:names[varying_store->pointer];
    const uint16_t varying_output=typed.add_resource(backend::TypedResourceKind::Output,{},pass_type,
                                                      output_name,0,typed_semantic,0);
    if (varying_output==std::numeric_limits<uint16_t>::max() ||
        !program.emit<backend::TypedOpcode::StoreOutput>(0,{},input_values[passai],{},varying_output)) {
        why="failed to emit portable Typed varying store"; return false;
    }
    return true;
}

bool lower_fragment_subset(const uint32_t *words, size_t word_count,
                           uint32_t entry_function, backend::TypedShader &typed,
                           std::string &why) {
    typed = backend::TypedShader(backend::TypedStage::Fragment);
    if (!words || word_count < 5) { why="short module"; return false; }
    const uint32_t bound=words[3];
    if (bound<2 || bound>(1u<<20)) { why="unreasonable id bound"; return false; }
    std::vector<TypeInfo> types(bound);
    std::vector<DecorationInfo> decorations(bound);
    std::vector<VarInfo> vars(bound);
    std::vector<ValueInfo> values(bound);
    std::vector<std::string> names(bound);
    std::vector<StoreInfo> stores;
    bool in_selected_function=false;

    size_t cursor=5;
    while(cursor<word_count) {
        const uint32_t first=words[cursor];
        const uint16_t wc=static_cast<uint16_t>(first>>16);
        const uint16_t op=static_cast<uint16_t>(first&0xffffu);
        if(!wc || cursor+wc>word_count) { why="malformed instruction while lowering fragment"; return false; }
        auto id_ok=[bound](uint32_t id){return id<bound;};
        switch(op) {
        case kOpName:
            if(wc>=3 && id_ok(words[cursor+1])) names[words[cursor+1]]=literal_string(words+cursor+2,wc-2);
            break;
        case kOpDecorate:
            if(wc>=4 && id_ok(words[cursor+1]) && words[cursor+2]==kDecorationLocation) { decorations[words[cursor+1]].has_location=true; decorations[words[cursor+1]].location=words[cursor+3]; }
            break;
        case kOpTypeFloat:
            if(wc>=3 && id_ok(words[cursor+1]) && words[cursor+2]==32) types[words[cursor+1]]={TypeInfo::Kind::Float,0,32,0};
            break;
        case kOpTypeVector:
            if(wc==4 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Vector,words[cursor+2],words[cursor+3],0};
            break;
        case kOpTypeImage:
            if(wc>=9 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Image,words[cursor+2],0,0};
            break;
        case kOpTypeSampledImage:
            if(wc==3 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::SampledImage,words[cursor+2],0,0};
            break;
        case kOpTypePointer:
            if(wc==4 && id_ok(words[cursor+1])) types[words[cursor+1]]={TypeInfo::Kind::Pointer,words[cursor+3],0,words[cursor+2]};
            break;
        case kOpVariable:
            if(wc>=4 && id_ok(words[cursor+2])) vars[words[cursor+2]]={words[cursor+1],words[cursor+3]};
            break;
        case kOpFunction:
            if(wc>=5) in_selected_function=words[cursor+2]==entry_function;
            break;
        case kOpFunctionEnd:
            in_selected_function=false;
            break;
        case kOpLoad:
            if(in_selected_function && wc>=4 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::Load,words[cursor+1],words[cursor+3],0,{}};
            break;
        case kOpCompositeConstruct:
            if(in_selected_function && wc>=4 && id_ok(words[cursor+2])) { ValueInfo v; v.kind=ValueInfo::Kind::Composite; v.type=words[cursor+1]; v.constituents.assign(words+cursor+3,words+cursor+wc); values[words[cursor+2]]=std::move(v); }
            break;
        case kOpImageSampleImplicitLod:
            if(in_selected_function && wc>=5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::ImageSample,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpFNegate:
            if(in_selected_function && wc==4 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::FNegate,words[cursor+1],words[cursor+3],0,{}};
            break;
        case kOpFAdd:
            if(in_selected_function && wc==5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::FAdd,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpFSub:
            if(in_selected_function && wc==5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::FSub,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpFMul:
            if(in_selected_function && wc==5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::FMul,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpDot:
            if(in_selected_function && wc==5 && id_ok(words[cursor+2])) values[words[cursor+2]]={ValueInfo::Kind::Dot,words[cursor+1],words[cursor+3],words[cursor+4],{}};
            break;
        case kOpStore:
            if(in_selected_function && wc>=3) stores.push_back({words[cursor+1],words[cursor+2]});
            break;
        default: break;
        }
        cursor+=wc;
    }

    uint32_t sampler_var=0, output_var=0;
    std::vector<uint32_t> uniform_vars;
    std::vector<uint32_t> input_vars;
    for(uint32_t id=1;id<bound;++id) {
        if(vars[id].storage==kStorageUniform) {
            uniform_vars.push_back(id);
        } else if(vars[id].storage==kStorageUniformConstant) {
            const uint32_t pt=pointee_type(types,vars[id].type);
            if(pt && pt<types.size() && types[pt].kind==TypeInfo::Kind::SampledImage) {
                if(sampler_var) { why="multiple fragment samplers unsupported"; return false; }
                sampler_var=id;
            }
        } else if(vars[id].storage==kStorageInput && decorations[id].has_location) {
            input_vars.push_back(id);
        } else if(vars[id].storage==kStorageOutput && decorations[id].has_location) {
            if(decorations[id].location!=0 || output_var) { why="fragment subset requires exactly output Location 0"; return false; }
            output_var=id;
        }
    }
    if(!output_var) { why="fragment subset requires color output Location 0"; return false; }
    const uint32_t output_type=pointee_type(types,vars[output_var].type);
    if(vector_components(types,output_type)!=4) { why="fragment output must be float4"; return false; }

    const StoreInfo *color_store=nullptr;
    for(const auto &st:stores) if(st.pointer==output_var) {
        if(color_store){why="multiple fragment color stores unsupported";return false;}
        color_store=&st;
    }
    if(!color_store || color_store->object>=bound) { why="fragment output store missing"; return false; }

    auto &program=typed.program();
    std::unordered_map<uint32_t,backend::TypedValue> resources;
    std::sort(input_vars.begin(),input_vars.end(),[&](uint32_t a,uint32_t b){return decorations[a].location<decorations[b].location;});
    for(uint32_t var:input_vars) {
        const uint8_t components=vector_components(types,pointee_type(types,vars[var].type));
        const auto type=typed_f32(components);
        if(type==backend::TypedType::Invalid || decorations[var].location>31) { why="fragment input is outside portable float subset"; return false; }
        const auto value=program.input(type,static_cast<uint16_t>(decorations[var].location));
        const std::string resource_name=names[var].empty()?("input"+std::to_string(decorations[var].location)):names[var];
        if(value.kind()==backend::TypedValueKind::None ||
           typed.add_resource(backend::TypedResourceKind::Input,value,type,resource_name,
                              static_cast<uint16_t>(decorations[var].location))==std::numeric_limits<uint16_t>::max()) {
            why="failed to create portable Typed fragment input"; return false;
        }
        resources[var]=value;
    }
    for(size_t i=0;i<uniform_vars.size();++i) {
        const uint32_t var=uniform_vars[i];
        if(vector_components(types,pointee_type(types,vars[var].type))!=4) { why="portable fragment uniforms must be float4"; return false; }
        const uint16_t index=static_cast<uint16_t>(i*4u);
        const auto value=program.uniform<backend::TypedType::F32x4>(index);
        const std::string resource_name=names[var].empty()?("u"+std::to_string(i)):names[var];
        if(value.kind()==backend::TypedValueKind::None ||
           typed.add_resource(backend::TypedResourceKind::Uniform,value,backend::TypedType::F32x4,
                              resource_name,index)==std::numeric_limits<uint16_t>::max()) {
            why="failed to create portable Typed fragment uniform"; return false;
        }
        resources[var]=value;
    }
    if(sampler_var) {
        const auto value=program.sampler(0);
        const std::string resource_name=names[sampler_var].empty()?"tex":names[sampler_var];
        if(value.kind()==backend::TypedValueKind::None ||
           typed.add_resource(backend::TypedResourceKind::Sampler2D,value,backend::TypedType::Sampler2D,
                              resource_name,0)==std::numeric_limits<uint16_t>::max()) {
            why="failed to create portable Typed fragment sampler"; return false;
        }
        resources[sampler_var]=value;
    }
    const std::string output_name=names[output_var].empty()?"color":names[output_var];
    const uint16_t output_resource=typed.add_resource(backend::TypedResourceKind::Output,{},backend::TypedType::F32x4,
                                                       output_name,0,backend::TypedSemantic::Color,0);
    if(output_resource==std::numeric_limits<uint16_t>::max()) { why="failed to create portable Typed fragment output"; return false; }

    std::unordered_map<uint32_t,backend::TypedValue> memo;
    std::function<bool(uint32_t,backend::TypedValue&)> build_value = [&](uint32_t id,backend::TypedValue &out_value)->bool {
        if(auto it=memo.find(id);it!=memo.end()){out_value=it->second;return true;}
        if(id>=bound){why="fragment expression id out of range";return false;}
        const auto &v=values[id];
        backend::TypedValue result{};
        if(v.kind==ValueInfo::Kind::Load) {
            auto it=resources.find(v.a);
            if(it==resources.end()) { why="fragment load does not reference a supported resource"; return false; }
            result=it->second;
        } else if(v.kind==ValueInfo::Kind::ImageSample) {
            backend::TypedValue sampled{},coord{};
            if(!build_value(v.a,sampled)||!build_value(v.b,coord) || sampled.type()!=backend::TypedType::Sampler2D ||
               coord.type()!=backend::TypedType::F32x2 || vector_components(types,v.type)!=4) {
                why="portable texture sample operands are unsupported"; return false;
            }
            result=program.make_value<backend::TypedType::F32x4>();
            if(result.kind()==backend::TypedValueKind::None ||
               !program.emit<backend::TypedOpcode::Sample2D>(0,result,sampled,coord)) {
                why="failed to emit portable Typed sample2D"; return false;
            }
        } else if(v.kind==ValueInfo::Kind::Composite) {
            if(v.constituents.size()!=4 || v.constituents[0]!=v.constituents[1] ||
               v.constituents[0]!=v.constituents[2] || v.constituents[0]!=v.constituents[3]) {
                why="portable fragment composite must be a scalar splat"; return false;
            }
            backend::TypedValue scalar{};
            if(!build_value(v.constituents[0],scalar) || scalar.type()!=backend::TypedType::F32) {
                why="portable fragment splat source must be F32"; return false;
            }
            result=program.make_value<backend::TypedType::F32x4>();
            if(result.kind()==backend::TypedValueKind::None ||
               !program.emit<backend::TypedOpcode::FloatSplat>(0,result,scalar)) {
                why="failed to emit portable Typed float splat"; return false;
            }
        } else if(v.kind==ValueInfo::Kind::FNegate) {
            backend::TypedValue source{};
            if(!build_value(v.a,source) || source.type()!=backend::TypedType::F32x4) {
                why="portable negate requires float4"; return false;
            }
            result=program.make_value<backend::TypedType::F32x4>();
            if(result.kind()==backend::TypedValueKind::None ||
               !program.emit<backend::TypedOpcode::FloatUnary>(static_cast<uint8_t>(backend::TypedFloatUnaryOp::Neg),
                                                               result,source)) {
                why="failed to emit portable Typed negate"; return false;
            }
        } else if(v.kind==ValueInfo::Kind::FMul || v.kind==ValueInfo::Kind::FAdd ||
                  v.kind==ValueInfo::Kind::FSub || v.kind==ValueInfo::Kind::Dot) {
            backend::TypedValue a{},b{};
            if(!build_value(v.a,a)||!build_value(v.b,b) || a.type()!=backend::TypedType::F32x4 ||
               b.type()!=backend::TypedType::F32x4) {
                why="portable float arithmetic requires float4 operands"; return false;
            }
            backend::TypedFloatOp op;
            backend::TypedType result_type=backend::TypedType::F32x4;
            if(v.kind==ValueInfo::Kind::FMul) op=backend::TypedFloatOp::Mul;
            else if(v.kind==ValueInfo::Kind::FAdd) op=backend::TypedFloatOp::Add;
            else if(v.kind==ValueInfo::Kind::FSub) op=backend::TypedFloatOp::Sub;
            else { op=backend::TypedFloatOp::Dot; result_type=backend::TypedType::F32; }
            result=program.make_value(result_type);
            if(result.kind()==backend::TypedValueKind::None ||
               !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(op),result,a,b)) {
                why="failed to emit portable Typed float arithmetic"; return false;
            }
        } else {
            why="portable fragment expression contains unsupported opcode"; return false;
        }
        memo[id]=result;
        out_value=result;
        return true;
    };

    backend::TypedValue root{};
    if(!build_value(color_store->object,root) || root.type()!=backend::TypedType::F32x4) {
        if(why.empty()) why="portable fragment root must be float4";
        return false;
    }
    if(!program.emit<backend::TypedOpcode::StoreOutput>(0,{},root,{},output_resource)) {
        why="failed to emit portable Typed output store";
        return false;
    }
    return true;
}
}

bool spirv_to_gxp(VscStage stage, const char *entrypoint,
                  const uint32_t *words, size_t word_count,
                  BackendOutput &out) {
    out = {};
    PreparedSpirv prepared;
    Diagnostic prepare_error;
    if (!prepare_spirv(stage, entrypoint, words, word_count, prepared, prepare_error)) {
        out.diagnostics.push_back(std::move(prepare_error));
        return false;
    }
    words = prepared.words.data();
    word_count = prepared.words.size();

    SpirvSummary summary;
    Diagnostic parse_error;
    if (!parse_spirv(words, word_count, summary, parse_error)) {
        out.diagnostics.push_back(std::move(parse_error));
        return false;
    }

    const char *wanted_name = (entrypoint && *entrypoint) ? entrypoint : "main";
    const uint32_t wanted_model = stage == VSC_STAGE_FRAGMENT ? kExecutionModelFragment : kExecutionModelVertex;
    const SpirvEntryPoint *selected = nullptr;
    for (const auto &ep : summary.entry_points) if (ep.name == wanted_name && ep.execution_model == wanted_model) { selected=&ep; break; }
    if (!selected) {
        std::ostringstream ss; ss << "SPIR-V entry point '" << wanted_name << "' for " << (stage==VSC_STAGE_FRAGMENT?"fragment":"vertex") << " stage was not found";
        out.diagnostics.push_back({VSC_DIAG_ERROR,0x2201,0,0,ss.str()}); return false;
    }

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    {
        backend::TypedShader typed(stage == VSC_STAGE_FRAGMENT ? backend::TypedStage::Fragment
                                                               : backend::TypedStage::Vertex);
        std::string typed_why;
        if (spirv_cross_to_typed_shader(prepared.words, typed.stage(), wanted_name, typed, typed_why)) {
            backend::IrCompileResult lowered;
            if (backend::compile_typed_shader(typed, lowered)) {
                out.gxp = std::move(lowered.gxp);
                return true;
            }
            typed_why = "Typed Vita IR lowering failed: " + lowered.error;
        }
        // Keep the independently tested parser/lowerer as a temporary fallback
        // while Typed IR coverage grows. Diagnostics remain owned by the legacy
        // path until the fallback can be removed completely.
    }
#endif
    backend::TypedShader portable(stage == VSC_STAGE_FRAGMENT ? backend::TypedStage::Fragment
                                                              : backend::TypedStage::Vertex);
    std::string why;
    const bool portable_ok = stage == VSC_STAGE_FRAGMENT ?
        lower_fragment_subset(words,word_count,selected->id,portable,why) :
        lower_vertex_subset(words,word_count,selected->id,portable,why);
    if (!portable_ok) {
        const uint32_t code = stage == VSC_STAGE_FRAGMENT ? 0x2220 : 0x2210;
        out.diagnostics.push_back({VSC_DIAG_ERROR,code,0,0,
            std::string("unsupported ")+(stage==VSC_STAGE_FRAGMENT?"fragment":"vertex")+
            " SPIR-V subset: "+why});
        return false;
    }
    backend::IrCompileResult lowered;
    if (!backend::compile_typed_shader(portable,lowered)) {
        const uint32_t code = stage == VSC_STAGE_FRAGMENT ? 0x2221 : 0x2211;
        out.diagnostics.push_back({VSC_DIAG_ERROR,code,0,0,"Typed Vita IR lowering failed: "+lowered.error});
        return false;
    }
    out.gxp=std::move(lowered.gxp);
    return true;
}

} // namespace vsc
