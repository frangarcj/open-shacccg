#include "core/internal.hpp"
#include "backend/vita_ir.hpp"
#include "spirv/spirv_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
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

static uint32_t loaded_variable(const std::vector<ValueInfo> &values, uint32_t id) {
    if (id >= values.size() || values[id].kind != ValueInfo::Kind::Load) return 0;
    return values[id].a;
}

static bool is_one(const std::vector<ValueInfo> &values, uint32_t id) {
    return id < values.size() && values[id].kind == ValueInfo::Kind::ConstantOne;
}

bool lower_vertex_subset(const uint32_t *words, size_t word_count,
                         uint32_t entry_function, backend::VertexIr &ir,
                         std::string &why) {
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
    for (const auto &in: inputs) {
        if (in.location > 31) { why="input location is too large for initial allocator"; return false; }
        ir.attributes.push_back({in.name.empty() ? ("attr"+std::to_string(in.location)) : in.name, in.components, in.location*4});
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

    const ValueInfo &pv = position_store->object<bound ? values[position_store->object] : ValueInfo{};
    if (pv.kind == ValueInfo::Kind::Composite) {
        // Validated clear_v shape: vec4(load float2, 1.0, 1.0).
        if (pv.constituents.size()!=3 || !is_one(values,pv.constituents[1]) || !is_one(values,pv.constituents[2])) { why="unsupported constructed position"; return false; }
        const uint32_t posvar=loaded_variable(values,pv.constituents[0]); const int ai=attr_index_for_var(posvar);
        if (ai<0 || inputs[ai].components!=2 || inputs.size()!=1 || uniform_var || varying_store) { why="constructed position does not match validated clear_v shape"; return false; }
        ir.ops.push_back({backend::IrOpKind::ConstructPosition,static_cast<uint32_t>(ai),0,backend::IrVaryingSemantic::TexCoord});
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
    ir.matrices.push_back({names[uniform_var].empty()?"wvp":names[uniform_var],0});
    ir.ops.push_back({backend::IrOpKind::TransformPosition,static_cast<uint32_t>(posai),0,semantic});
    ir.ops.push_back({backend::IrOpKind::CopyVarying,static_cast<uint32_t>(passai),0,semantic});
    return true;
}

bool lower_fragment_subset(const uint32_t *words, size_t word_count,
                           uint32_t entry_function, backend::FragmentIr &ir,
                           std::string &why) {
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

    uint32_t uniform_var=0, sampler_var=0, output_var=0;
    std::vector<uint32_t> uniform_vars;
    std::vector<uint32_t> input_vars;
    for(uint32_t id=1;id<bound;++id) {
        if(vars[id].storage==kStorageUniform) {
            uniform_vars.push_back(id);
            if(!uniform_var) uniform_var=id;
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
    for(const auto &st:stores) if(st.pointer==output_var) { if(color_store){why="multiple fragment color stores unsupported";return false;} color_store=&st; }
    if(!color_store || color_store->object>=bound) { why="fragment output store missing"; return false; }
    const ValueInfo &color=values[color_store->object];

    if(color.kind==ValueInfo::Kind::Load) {
        const uint32_t srcvar=color.a;
        if(srcvar==uniform_var && uniform_vars.size()==1) {
            const uint32_t uniform_type=pointee_type(types,vars[uniform_var].type);
            if(vector_components(types,uniform_type)!=4) { why="fragment uniform must be float4"; return false; }
            if(!input_vars.empty() || sampler_var) { why="uniform-color fragment has unexpected inputs/sampler"; return false; }
            ir.op=backend::FragmentOpKind::UniformColor;
            ir.uniforms.push_back({names[uniform_var].empty()?"uColor":names[uniform_var],0});
            return true;
        }
        if(vars[srcvar].storage==kStorageInput && decorations[srcvar].has_location && decorations[srcvar].location==0 &&
           vector_components(types,pointee_type(types,vars[srcvar].type))==4 && !uniform_var && !sampler_var && input_vars.size()==1) {
            ir.op=backend::FragmentOpKind::VaryingColor;
            return true;
        }
        why="direct fragment color load is not a supported uniform/varying"; return false;
    }

    if(color.kind==ValueInfo::Kind::FMul) {
        if(!sampler_var || uniform_vars.size()!=1 || input_vars.size()!=1) { why="texture-tint fragment requires one sampler, one uniform and one input"; return false; }
        uint32_t sample_id=0, tint_load_id=0;
        if(color.a<bound && values[color.a].kind==ValueInfo::Kind::ImageSample && color.b<bound && values[color.b].kind==ValueInfo::Kind::Load) { sample_id=color.a; tint_load_id=color.b; }
        else if(color.b<bound && values[color.b].kind==ValueInfo::Kind::ImageSample && color.a<bound && values[color.a].kind==ValueInfo::Kind::Load) { sample_id=color.b; tint_load_id=color.a; }
        else { why="fragment multiply must be texture sample * uniform float4"; return false; }
        if(loaded_variable(values,tint_load_id)!=uniform_var) { why="fragment multiply operand is not the supported tint uniform"; return false; }
        const uint32_t uniform_type=pointee_type(types,vars[uniform_var].type);
        if(vector_components(types,uniform_type)!=4) { why="texture tint uniform must be float4"; return false; }
        const ValueInfo &sample=values[sample_id];
        const uint32_t sampled_var=loaded_variable(values,sample.a);
        const uint32_t coord_var=loaded_variable(values,sample.b);
        const uint32_t in=input_vars[0];
        if(sampled_var!=sampler_var || coord_var!=in || decorations[in].location!=0 ||
           vector_components(types,pointee_type(types,vars[in].type))!=2) { why="texture-tint sample must use sampler0 and float2 Location 0 coordinates"; return false; }
        ir.op=backend::FragmentOpKind::TextureTint2D;
        ir.uniforms.push_back({names[uniform_var].empty()?"uTintColor":names[uniform_var],0});
        ir.samplers.push_back({names[sampler_var].empty()?"tex":names[sampler_var],0});
        return true;
    }

    if(color.kind==ValueInfo::Kind::ImageSample) {
        if(!sampler_var || !uniform_vars.empty() || input_vars.size()!=1) { why="texture fragment requires one sampler and one input"; return false; }
        const uint32_t sampled_var=loaded_variable(values,color.a);
        const uint32_t coord_var=loaded_variable(values,color.b);
        const uint32_t in=input_vars[0];
        if(sampled_var!=sampler_var || coord_var!=in || decorations[in].location!=0 ||
           vector_components(types,pointee_type(types,vars[in].type))!=2) { why="texture sample must use sampler0 and float2 Location 0 coordinates"; return false; }
        ir.op=backend::FragmentOpKind::Texture2D;
        ir.samplers.push_back({names[sampler_var].empty()?"tex":names[sampler_var],0});
        return true;
    }

    // Generic arithmetic fallback. Preserve all canonical shape-specific
    // paths above; only previously unsupported expression trees use this DAG.
    if (sampler_var) { why="generic arithmetic fallback does not yet mix samplers"; return false; }
    if (input_vars.size()>1) { why="generic arithmetic fallback supports at most one located varying"; return false; }
    for (uint32_t u : uniform_vars) {
        const uint32_t ut=pointee_type(types,vars[u].type);
        if(vector_components(types,ut)!=4) { why="generic arithmetic uniforms must be float4"; return false; }
        ir.uniforms.push_back({names[u].empty()?("u"+std::to_string(ir.uniforms.size())):names[u],static_cast<uint32_t>(ir.uniforms.size()*4)});
    }
    std::unordered_map<uint32_t,uint32_t> uniform_index;
    for(size_t i=0;i<uniform_vars.size();++i) uniform_index[uniform_vars[i]]=static_cast<uint32_t>(i);
    std::unordered_map<uint32_t,uint32_t> memo;
    std::function<bool(uint32_t,uint32_t&)> build_expr = [&](uint32_t id,uint32_t &node)->bool {
        if(auto it=memo.find(id);it!=memo.end()){node=it->second;return true;}
        if(id>=bound){why="expression id out of range";return false;}
        const auto &v=values[id]; backend::FragmentExprNode n{};
        if(v.kind==ValueInfo::Kind::Load) {
            const uint32_t var=v.a;
            if(auto it=uniform_index.find(var);it!=uniform_index.end()) { n.kind=backend::FragmentExprKind::Uniform; n.a=it->second; n.components=4; }
            else if(!input_vars.empty() && var==input_vars[0] && decorations[var].location==0 && vector_components(types,pointee_type(types,vars[var].type))==4) { n.kind=backend::FragmentExprKind::Varying; n.a=0; n.components=4; }
            else { why="generic arithmetic leaf is not float4 uniform or Location 0 varying"; return false; }
        } else if(v.kind==ValueInfo::Kind::Composite) {
            if(v.constituents.size()!=4 || v.constituents[0]!=v.constituents[1] || v.constituents[0]!=v.constituents[2] || v.constituents[0]!=v.constituents[3]) { why="generic arithmetic composite must currently be a scalar splat"; return false; }
            uint32_t a=0; if(!build_expr(v.constituents[0],a)) return false;
            n.kind=backend::FragmentExprKind::Splat; n.a=a; n.components=4;
        } else if(v.kind==ValueInfo::Kind::FNegate) {
            uint32_t a=0; if(!build_expr(v.a,a)) return false;
            n.kind=backend::FragmentExprKind::Neg; n.a=a; n.components=4;
        } else if(v.kind==ValueInfo::Kind::FMul || v.kind==ValueInfo::Kind::FAdd || v.kind==ValueInfo::Kind::FSub || v.kind==ValueInfo::Kind::Dot) {
            uint32_t a=0,b=0; if(!build_expr(v.a,a)||!build_expr(v.b,b)) return false;
            n.a=a; n.b=b;
            if(v.kind==ValueInfo::Kind::FMul) n.kind=backend::FragmentExprKind::Mul;
            else if(v.kind==ValueInfo::Kind::FAdd) n.kind=backend::FragmentExprKind::Add;
            else if(v.kind==ValueInfo::Kind::FSub) n.kind=backend::FragmentExprKind::Sub;
            else n.kind=backend::FragmentExprKind::Dot;
            n.components=static_cast<uint8_t>(v.kind==ValueInfo::Kind::Dot?1:4);
        } else { why="generic arithmetic expression contains unsupported opcode"; return false; }
        node=static_cast<uint32_t>(ir.expressions.size()); ir.expressions.push_back(n); memo[id]=node; return true;
    };
    uint32_t root=0;
    if(!build_expr(color_store->object,root)) return false;
    if(ir.expressions[root].components!=4) { why="generic fragment arithmetic root must be float4"; return false; }
    ir.root_expression=root; ir.op=backend::FragmentOpKind::Arithmetic; return true;
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
    if (stage == VSC_STAGE_FRAGMENT) {
        backend::FragmentIr ir; std::string why;
        if (!lower_fragment_subset(words,word_count,selected->id,ir,why)) {
            out.diagnostics.push_back({VSC_DIAG_ERROR,0x2220,0,0,"unsupported fragment SPIR-V subset: "+why}); return false;
        }
        backend::IrCompileResult lowered;
        if (!backend::compile_fragment_ir(ir,lowered)) {
            out.diagnostics.push_back({VSC_DIAG_ERROR,0x2221,0,0,"fragment Vita IR lowering failed: "+lowered.error}); return false;
        }
        out.gxp=std::move(lowered.gxp);
        return true;
    }

    backend::VertexIr ir; std::string why;
    if (!lower_vertex_subset(words,word_count,selected->id,ir,why)) {
        out.diagnostics.push_back({VSC_DIAG_ERROR,0x2210,0,0,"unsupported vertex SPIR-V subset: "+why}); return false;
    }
    backend::IrCompileResult lowered;
    if (!backend::compile_vertex_ir(ir,lowered)) {
        out.diagnostics.push_back({VSC_DIAG_ERROR,0x2211,0,0,"Vita IR lowering failed: "+lowered.error}); return false;
    }
    out.gxp=std::move(lowered.gxp);
    return true;
}

} // namespace vsc
