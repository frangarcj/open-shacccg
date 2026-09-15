#include "core/internal.hpp"

#if defined(OPENSHACCG_ENABLE_GLSLANG)
#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

namespace vsc {

namespace {

#if defined(OPENSHACCG_ENABLE_GLSLANG)
bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_cg_type_token(const std::string &token) {
    static constexpr const char *bases[] = {"bool", "fixed", "float", "half", "int", "uint", "short"};
    for (const char *base : bases) {
        const std::string prefix(base);
        if (token == prefix) return true;
        if (token.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = token.substr(prefix.size());
        if (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '4') return true;
        if (suffix.size() == 3 && suffix[0] >= '1' && suffix[0] <= '4' && suffix[1] == 'x' &&
            suffix[2] >= '1' && suffix[2] <= '4') return true;
    }
    return false;
}

bool short_type_suffix(const std::string &token, std::string &suffix) {
    if (token.compare(0,5,"short")!=0) return false;
    suffix=token.substr(5);
    return suffix.empty() || (suffix.size()==1 && suffix[0]>='1' && suffix[0]<='4');
}

bool int_type_suffix(const std::string &token, std::string &suffix) {
    if (token.compare(0,3,"int")!=0) return false;
    suffix=token.substr(3);
    return suffix.empty() || (suffix.size()==1 && suffix[0]>='1' && suffix[0]<='4');
}

std::string normalize_cg_integer_spellings(const std::string &input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i=0;i<input.size();) {
        if (!is_identifier_start(input[i])) {
            output.push_back(input[i++]);
            continue;
        }
        size_t end=i+1;
        while (end<input.size() && is_identifier_char(input[end])) ++end;
        const std::string token=input.substr(i,end-i);
        if (token=="unsigned") {
            size_t next=end;
            while (next<input.size() && std::isspace(static_cast<unsigned char>(input[next]))) ++next;
            if (next<input.size() && is_identifier_start(input[next])) {
                size_t next_end=next+1;
                while (next_end<input.size() && is_identifier_char(input[next_end])) ++next_end;
                const std::string type=input.substr(next,next_end-next);
                std::string suffix;
                if (short_type_suffix(type,suffix) || int_type_suffix(type,suffix)) {
                    output += "uint" + suffix;
                    i=next_end;
                    continue;
                }
            }
        }
        std::string suffix;
        if (short_type_suffix(token,suffix)) output += "int" + suffix;
        else output += token;
        i=end;
    }
    return output;
}

std::string normalize_cg_scalar_bit_casts(const std::string &input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i=0;i<input.size();) {
        if (!is_identifier_start(input[i])) {
            output.push_back(input[i++]);
            continue;
        }
        size_t end=i+1;
        while (end<input.size() && is_identifier_char(input[end])) ++end;
        const std::string token=input.substr(i,end-i);
        if (token=="bit_cast") {
            size_t p=end;
            while (p<input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
            if (p<input.size() && input[p]=='<') {
                ++p;
                while (p<input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
                if (p<input.size() && is_identifier_start(input[p])) {
                    const size_t type_begin=p++;
                    while (p<input.size() && is_identifier_char(input[p])) ++p;
                    const std::string type=input.substr(type_begin,p-type_begin);
                    while (p<input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
                    if (p<input.size() && input[p]=='>' && type=="uint") {
                        output += "asuint";
                        i=p+1;
                        continue;
                    }
                }
            }
        }
        output += token;
        i=end;
    }
    return output;
}

struct GlobalInterfaceDecl {
    std::string type;
    std::string name;
    std::string semantic;
    bool output = false;
};

void skip_space(const std::string &text, size_t &offset) {
    while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset]))) ++offset;
}

bool read_identifier(const std::string &text, size_t &offset, std::string &value) {
    skip_space(text, offset);
    if (offset >= text.size() || !is_identifier_start(text[offset])) return false;
    const size_t begin = offset++;
    while (offset < text.size() && is_identifier_char(text[offset])) ++offset;
    value = text.substr(begin, offset - begin);
    return true;
}

bool parse_global_interface_line(const std::string &line, GlobalInterfaceDecl &decl) {
    size_t offset = 0;
    std::string type, direction, name, semantic;
    if (!read_identifier(line, offset, type) || !is_cg_type_token(type) ||
        !read_identifier(line, offset, direction) || (direction != "in" && direction != "out") ||
        !read_identifier(line, offset, name)) return false;
    skip_space(line, offset);
    if (offset >= line.size() || line[offset++] != ':') return false;
    if (!read_identifier(line, offset, semantic)) return false;
    skip_space(line, offset);
    if (offset >= line.size() || line[offset++] != ';') return false;
    skip_space(line, offset);
    if (offset < line.size() && line.compare(offset, 2, "//") != 0) return false;
    decl = {std::move(type), std::move(name), std::move(semantic), direction == "out"};
    return true;
}

int top_level_brace_delta(const std::string &line, bool &block_comment) {
    int delta = 0;
    for (size_t i = 0; i < line.size();) {
        if (block_comment) {
            const size_t end = line.find("*/", i);
            if (end == std::string::npos) break;
            block_comment = false;
            i = end + 2;
            continue;
        }
        if (line.compare(i, 2, "//") == 0) break;
        if (line.compare(i, 2, "/*") == 0) {
            block_comment = true;
            i += 2;
            continue;
        }
        if (line[i] == '{') ++delta;
        else if (line[i] == '}') --delta;
        ++i;
    }
    return delta;
}

bool find_entrypoint_parameters(const std::string &source, const std::string &entrypoint,
                                size_t &open_paren, size_t &close_paren) {
    for (size_t i = 0; i < source.size();) {
        if (!is_identifier_start(source[i])) { ++i; continue; }
        size_t end = i + 1;
        while (end < source.size() && is_identifier_char(source[end])) ++end;
        if (source.compare(i, end - i, entrypoint) != 0) { i = end; continue; }
        size_t next = end;
        skip_space(source, next);
        if (next >= source.size() || source[next] != '(') { i = end; continue; }
        open_paren = next;
        int depth = 1;
        for (size_t p = next + 1; p < source.size(); ++p) {
            if (source[p] == '(') ++depth;
            else if (source[p] == ')' && --depth == 0) {
                close_paren = p;
                return true;
            }
        }
        return false;
    }
    return false;
}

struct MutableUniformParam {
    std::string type;
    std::string name;
    std::string local_name;
};

bool code_identifier_assigned(const std::string &text, size_t begin, size_t end,
                              const std::string &name) {
    bool line_comment=false,block_comment=false,string_literal=false,char_literal=false,escape=false;
    for (size_t i=begin;i<end;) {
        if (line_comment) {
            if (text[i]=='\n') line_comment=false;
            ++i; continue;
        }
        if (block_comment) {
            if (i+1<end && text[i]=='*' && text[i+1]=='/') { block_comment=false; i+=2; }
            else ++i;
            continue;
        }
        if (string_literal || char_literal) {
            const char quote=string_literal?'"':'\'';
            if (escape) escape=false;
            else if (text[i]=='\\') escape=true;
            else if (text[i]==quote) { string_literal=false; char_literal=false; }
            ++i; continue;
        }
        if (i+1<end && text[i]=='/' && text[i+1]=='/') { line_comment=true; i+=2; continue; }
        if (i+1<end && text[i]=='/' && text[i+1]=='*') { block_comment=true; i+=2; continue; }
        if (text[i]=='"') { string_literal=true; ++i; continue; }
        if (text[i]=='\'') { char_literal=true; ++i; continue; }
        if (!is_identifier_start(text[i])) { ++i; continue; }
        const size_t token_begin=i++;
        while (i<end && is_identifier_char(text[i])) ++i;
        if (text.compare(token_begin,i-token_begin,name)!=0) continue;
        size_t p=i;
        while (p<end && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
        if (p>=end) continue;
        if (text[p]=='=' && (p+1>=end || text[p+1]!='=')) return true;
        if (p+1<end && text[p+1]=='=' &&
            (text[p]=='+' || text[p]=='-' || text[p]=='*' || text[p]=='/' || text[p]=='%' ||
             text[p]=='&' || text[p]=='|' || text[p]=='^')) return true;
        if (p+1<end && ((text[p]=='+' && text[p+1]=='+') || (text[p]=='-' && text[p+1]=='-')))
            return true;
    }
    return false;
}

std::string replace_code_identifiers(const std::string &text, size_t begin, size_t end,
                                     const std::vector<MutableUniformParam> &params) {
    std::string output;
    output.reserve(end-begin);
    bool line_comment=false,block_comment=false,string_literal=false,char_literal=false,escape=false;
    for (size_t i=begin;i<end;) {
        if (line_comment) {
            output.push_back(text[i]);
            if (text[i++]=='\n') line_comment=false;
            continue;
        }
        if (block_comment) {
            if (i+1<end && text[i]=='*' && text[i+1]=='/') {
                output.append("*/"); i+=2; block_comment=false;
            } else output.push_back(text[i++]);
            continue;
        }
        if (string_literal || char_literal) {
            const char c=text[i++];
            output.push_back(c);
            if (escape) escape=false;
            else if (c=='\\') escape=true;
            else if ((string_literal && c=='"') || (char_literal && c=='\'')) {
                string_literal=false; char_literal=false;
            }
            continue;
        }
        if (i+1<end && text[i]=='/' && text[i+1]=='/') {
            output.append("//"); i+=2; line_comment=true; continue;
        }
        if (i+1<end && text[i]=='/' && text[i+1]=='*') {
            output.append("/*"); i+=2; block_comment=true; continue;
        }
        if (text[i]=='"') { string_literal=true; output.push_back(text[i++]); continue; }
        if (text[i]=='\'') { char_literal=true; output.push_back(text[i++]); continue; }
        if (!is_identifier_start(text[i])) { output.push_back(text[i++]); continue; }
        const size_t token_begin=i++;
        while (i<end && is_identifier_char(text[i])) ++i;
        const std::string token=text.substr(token_begin,i-token_begin);
        const auto found=std::find_if(params.begin(),params.end(),[&](const MutableUniformParam &p) {
            return p.name==token;
        });
        output += found==params.end()?token:found->local_name;
    }
    return output;
}

std::string rewrite_mutated_uniform_parameters(const std::string &input, const std::string &entrypoint) {
    size_t open_paren=0,close_paren=0;
    if (!find_entrypoint_parameters(input,entrypoint,open_paren,close_paren)) return input;
    const size_t body_open=input.find('{',close_paren);
    if (body_open==std::string::npos) return input;
    size_t body_close=std::string::npos;
    int depth=1;
    bool line_comment=false,block_comment=false,string_literal=false,char_literal=false,escape=false;
    for (size_t i=body_open+1;i<input.size();++i) {
        if (line_comment) { if (input[i]=='\n') line_comment=false; continue; }
        if (block_comment) {
            if (i+1<input.size() && input[i]=='*' && input[i+1]=='/') { block_comment=false; ++i; }
            continue;
        }
        if (string_literal || char_literal) {
            if (escape) escape=false;
            else if (input[i]=='\\') escape=true;
            else if ((string_literal && input[i]=='"') || (char_literal && input[i]=='\'')) {
                string_literal=false; char_literal=false;
            }
            continue;
        }
        if (i+1<input.size() && input[i]=='/' && input[i+1]=='/') { line_comment=true; ++i; continue; }
        if (i+1<input.size() && input[i]=='/' && input[i+1]=='*') { block_comment=true; ++i; continue; }
        if (input[i]=='"') { string_literal=true; continue; }
        if (input[i]=='\'') { char_literal=true; continue; }
        if (input[i]=='{') ++depth;
        else if (input[i]=='}' && --depth==0) { body_close=i; break; }
    }
    if (body_close==std::string::npos) return input;

    std::vector<MutableUniformParam> candidates;
    size_t segment_begin=open_paren+1;
    int nested=0;
    for (size_t p=segment_begin;p<=close_paren;++p) {
        const char c=p<close_paren?input[p]:',';
        if (c=='(' || c=='[') ++nested;
        else if (c==')' || c==']') --nested;
        if (c!=',' || nested!=0) continue;
        const std::string segment=input.substr(segment_begin,p-segment_begin);
        std::vector<std::string> tokens;
        for (size_t s=0;s<segment.size();) {
            if (!is_identifier_start(segment[s])) { ++s; continue; }
            const size_t b=s++;
            while (s<segment.size() && is_identifier_char(segment[s])) ++s;
            tokens.push_back(segment.substr(b,s-b));
        }
        for (size_t t=0;t+2<tokens.size();++t) {
            if (tokens[t]!="uniform" || !is_cg_type_token(tokens[t+1])) continue;
            const std::string &name=tokens[t+2];
            if (code_identifier_assigned(input,body_open+1,body_close,name))
                candidates.push_back({tokens[t+1],name,"_vsc_local_"+name});
            break;
        }
        segment_begin=p+1;
    }
    if (candidates.empty()) return input;

    std::string init;
    for (const auto &param:candidates)
        init += "\n    " + param.type + " " + param.local_name + " = " + param.name + ";";
    std::string output=input.substr(0,body_open+1);
    output += init;
    output += replace_code_identifiers(input,body_open+1,body_close,candidates);
    output += input.substr(body_close);
    return output;
}

std::string rewrite_global_cg_interfaces(const std::string &input, const std::string &entrypoint) {
    std::vector<GlobalInterfaceDecl> interfaces;
    std::string rewritten;
    rewritten.reserve(input.size());
    int brace_depth = 0;
    bool block_comment = false;
    for (size_t offset = 0; offset < input.size();) {
        const size_t end = input.find('\n', offset);
        const size_t line_end = end == std::string::npos ? input.size() : end;
        const std::string line = input.substr(offset, line_end - offset);
        GlobalInterfaceDecl decl;
        if (brace_depth == 0 && !block_comment && parse_global_interface_line(line, decl)) {
            interfaces.push_back(decl);
            if (!decl.output) rewritten += "static " + decl.type + " " + decl.name + ";";
        } else {
            rewritten += line;
        }
        const int delta = top_level_brace_delta(line, block_comment);
        brace_depth += delta;
        if (end == std::string::npos) break;
        rewritten.push_back('\n');
        offset = end + 1;
    }
    if (interfaces.empty()) return rewritten;

    size_t open_paren = 0, close_paren = 0;
    if (!find_entrypoint_parameters(rewritten, entrypoint, open_paren, close_paren)) return rewritten;
    std::string added;
    bool have_existing = false;
    for (size_t i = open_paren + 1; i < close_paren; ++i)
        if (!std::isspace(static_cast<unsigned char>(rewritten[i]))) { have_existing = true; break; }
    for (const auto &decl : interfaces) {
        if (have_existing || !added.empty()) added += ", ";
        if (decl.output) {
            added += "out " + decl.type + " " + decl.name + " : " + decl.semantic;
        } else {
            added += decl.type + " _vsc_in_" + decl.name + " : " + decl.semantic;
        }
    }
    rewritten.insert(close_paren, added);

    close_paren += added.size();
    const size_t body = rewritten.find('{', close_paren);
    if (body == std::string::npos) return rewritten;
    std::string assignments;
    for (const auto &decl : interfaces) {
        if (!decl.output)
            assignments += "\n    " + decl.name + " = _vsc_in_" + decl.name + ";";
    }
    if (!assignments.empty()) rewritten.insert(body + 1, assignments);
    return rewritten;
}

std::string normalize_cg_for_hlsl(const char *source, size_t size, const char *entrypoint) {
    std::string input(source, size);
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf)
        input.erase(0, 3);

    input = normalize_cg_integer_spellings(input);
    input = normalize_cg_scalar_bit_casts(input);
    input = rewrite_global_cg_interfaces(input, entrypoint ? entrypoint : "main");
    input = rewrite_mutated_uniform_parameters(input, entrypoint ? entrypoint : "main");

    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (!is_identifier_start(input[i])) {
            output.push_back(input[i++]);
            continue;
        }

        size_t token_end = i + 1;
        while (token_end < input.size() && is_identifier_char(input[token_end])) ++token_end;
        const std::string token = input.substr(i, token_end - i);
        if (is_cg_type_token(token)) {
            size_t qualifier = token_end;
            while (qualifier < input.size() && std::isspace(static_cast<unsigned char>(input[qualifier]))) ++qualifier;
            size_t qualifier_end=qualifier;
            while (qualifier_end<input.size() && is_identifier_char(input[qualifier_end])) ++qualifier_end;
            const std::string direction=input.substr(qualifier,qualifier_end-qualifier);
            if ((direction=="out" || direction=="inout" || direction=="in") &&
                qualifier_end>qualifier) {
                output += direction;
                output.append(input, token_end, qualifier - token_end);
                output += token;
                i = qualifier_end;
                continue;
            }
        }
        output += token;
        i = token_end;
    }
    return output;
}

EShLanguage glslang_stage(VscStage stage) {
    return stage == VSC_STAGE_FRAGMENT ? EShLangFragment : EShLangVertex;
}

bool ensure_glslang_initialized() {
    static const bool initialized = glslang::InitializeProcess();
    return initialized;
}
#endif

} // namespace

bool cg_to_spirv(const VscCompileRequest &request, FrontendOutput &out) {
    out = {};
    if (!request.source || request.source_size == 0) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1001, 0, 0, "empty shader source"});
        return false;
    }
    if (!request.entrypoint || !*request.entrypoint) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1002, 0, 0, "missing shader entry point"});
        return false;
    }

#if defined(OPENSHACCG_ENABLE_GLSLANG)
    if (request.source_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1003, 0, 0, "shader source is too large for glslang"});
        return false;
    }
    if (!ensure_glslang_initialized()) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10F0, 0, 0, "glslang initialization failed"});
        return false;
    }

    const std::string source = normalize_cg_for_hlsl(request.source, request.source_size, request.entrypoint);
    const char *source_ptr = source.c_str();
    const int source_length = static_cast<int>(source.size());
    const char *source_name = (request.source_name && *request.source_name) ? request.source_name : "shader.cg";
    const EShLanguage stage = glslang_stage(request.stage);
    glslang::TShader shader(stage);
    shader.setStringsWithLengthsAndNames(&source_ptr, &source_length, &source_name, 1);
    shader.setEntryPoint(request.entrypoint);
    shader.setSourceEntryPoint(request.entrypoint);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEnvTargetHlslFunctionality1();
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);

    const auto messages = static_cast<EShMessages>(
        EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslDX9Compatible | EShMsgDisplayErrorColumn);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        std::string message = shader.getInfoLog();
        if (const char *debug = shader.getInfoDebugLog(); debug && *debug) {
            if (!message.empty()) message += '\n';
            message += debug;
        }
        if (message.empty()) message = "Cg/HLSL parse failed";
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1010, 0, 0, std::move(message)});
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        std::string message = program.getInfoLog();
        if (const char *debug = program.getInfoDebugLog(); debug && *debug) {
            if (!message.empty()) message += '\n';
            message += debug;
        }
        if (message.empty()) message = "Cg/HLSL link failed";
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1011, 0, 0, std::move(message)});
        return false;
    }

    const glslang::TIntermediate *intermediate = program.getIntermediate(stage);
    if (!intermediate) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1012, 0, 0, "glslang produced no shader intermediate"});
        return false;
    }

    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*intermediate, out.spirv, &options);
    if (out.spirv.empty()) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1013, 0, 0, "glslang produced empty SPIR-V"});
        return false;
    }
    return true;
#else
    out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10FF, 0, 0,
        "Cg frontend unavailable: build with OPENSHACCG_ENABLE_GLSLANG=ON"});
    return false;
#endif
}

} // namespace vsc
