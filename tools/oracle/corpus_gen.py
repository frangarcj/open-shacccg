#!/usr/bin/env python3
"""Generate focused differential Cg shaders for a clean-room Sony oracle."""
import argparse
import json
import pathlib

SCALAR_VECTOR_TYPES = ["float", "float2", "float3", "float4", "half", "half2", "half4"]
BINARY = {"add": "a + b", "sub": "a - b", "mul": "a * b", "div": "a / b",
          "min": "min(a,b)", "max": "max(a,b)"}
UNARY = {"sat": "saturate(a)", "abs": "abs(a)", "neg": "-a"}


def emit(root, manifest, name, profile, source, feature, **meta):
    p = root / f"{name}.cg"
    p.write_text(source.rstrip() + "\n")
    row = {"name": name, "file": p.name, "profile": profile, "feature": feature}
    row.update(meta)
    manifest.append(row)


def vector_expr(expr, ty):
    if ty in ("float", "half"):
        return expr.replace(".xxxx", "")
    return expr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    args = ap.parse_args()
    root = pathlib.Path(args.out)
    root.mkdir(parents=True, exist_ok=True)
    manifest = []

    for ty in SCALAR_VECTOR_TYPES:
        args3 = f"{ty} a : TEXCOORD0, {ty} b : TEXCOORD1, {ty} c : TEXCOORD2"
        for opname, expr in BINARY.items():
            emit(root, manifest, f"fp-{opname}-{ty}", "sce_fp_psp2",
                 f"{ty} main({args3}) : COLOR0 {{ return {expr}; }}", "alu", type=ty, op=opname)
        for opname, expr in UNARY.items():
            emit(root, manifest, f"fp-{opname}-{ty}", "sce_fp_psp2",
                 f"{ty} main({args3}) : COLOR0 {{ return {expr}; }}", "alu", type=ty, op=opname)
        emit(root, manifest, f"fp-mad-{ty}", "sce_fp_psp2",
             f"{ty} main({args3}) : COLOR0 {{ return a * b + c; }}", "alu", type=ty, op="mad")
        emit(root, manifest, f"fp-dot-{ty}", "sce_fp_psp2",
             f"{ty} main({args3}) : COLOR0 {{ return {vector_expr('dot(a,b).xxxx', ty)}; }}", "alu", type=ty, op="dot")

    emit(root, manifest, "fp-swizzle-wzyx", "sce_fp_psp2",
         "float4 main(float4 a : TEXCOORD0) : COLOR0 { return a.wzyx; }", "swizzle")
    emit(root, manifest, "fp-swizzle-xxxx", "sce_fp_psp2",
         "float4 main(float4 a : TEXCOORD0) : COLOR0 { return a.xxxx; }", "swizzle")
    emit(root, manifest, "fp-constant-red", "sce_fp_psp2",
         "float4 main() : COLOR0 { return float4(1.0, 0.0, 0.0, 1.0); }", "constant")
    emit(root, manifest, "fp-uniform", "sce_fp_psp2",
         "uniform float4 color; float4 main() : COLOR0 { return color; }", "uniform")
    emit(root, manifest, "fp-s32-uniform-pass", "sce_fp_psp2",
         "uniform int x; int main() : COLOR0 { return x; }", "integer", type="int", op="pass")
    emit(root, manifest, "fp-s32-uniform-or", "sce_fp_psp2",
         "uniform int x; uniform int y; int main() : COLOR0 { return x | y; }", "integer", type="int", op="or")
    emit(root, manifest, "fp-s32-uniform-xor", "sce_fp_psp2",
         "uniform int x; int main() : COLOR0 { return x ^ 4660; }", "integer", type="int", op="xor-imm")
    emit(root, manifest, "fp-s32-uniform-and", "sce_fp_psp2",
         "uniform int x; int main() : COLOR0 { return x & 255; }", "integer", type="int", op="and-imm")
    emit(root, manifest, "fp-s32-uniform-shl", "sce_fp_psp2",
         "uniform int x; int main() : COLOR0 { return x << 3; }", "integer", type="int", op="shl-imm")
    emit(root, manifest, "fp-s32-uniform-shr", "sce_fp_psp2",
         "uniform int x; int main() : COLOR0 { return x >> 3; }", "integer", type="int", op="asr-imm")
    emit(root, manifest, "fp-s32-input-pass", "sce_fp_psp2",
         "int main(int a : TEXCOORD0) : COLOR0 { return a; }", "integer", type="int", op="input-pass")
    emit(root, manifest, "fp-f32-to-s32", "sce_fp_psp2",
         "int main(float a : TEXCOORD0) : COLOR0 { return (int)a; }", "integer", type="int", op="f32-to-s32")
    emit(root, manifest, "fp-s32x2-uniform-pass", "sce_fp_psp2",
         "uniform int2 x; int2 main() : COLOR0 { return x; }", "integer", type="int2", op="pass")
    emit(root, manifest, "fp-s32x2-uniform-or", "sce_fp_psp2",
         "uniform int2 x; uniform int2 y; int2 main() : COLOR0 { return x | y; }", "integer", type="int2", op="or")
    emit(root, manifest, "fp-s32-to-f32", "sce_fp_psp2",
         "uniform int x; float main() : COLOR0 { return (float)x; }", "integer", type="int", op="s32-to-f32")
    emit(root, manifest, "fp-floor-float", "sce_fp_psp2",
         "float main(float a:TEXCOORD0):COLOR0 { return floor(a); }", "primitives", type="float", op="floor")
    emit(root, manifest, "fp-mod-float", "sce_fp_psp2",
         "float main(float a:TEXCOORD0,float b:TEXCOORD1):COLOR0 { return fmod(a,b); }", "primitives", type="float", op="fmod")
    emit(root, manifest, "fp-discard-and", "sce_fp_psp2", """
float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1,float4 c:TEXCOORD2):COLOR0 {
    if (a.x>b.x && b.x>c.x) discard;
    return a;
}
""", "primitives", op="logical-and-discard")
    emit(root, manifest, "fp-if", "sce_fp_psp2",
         "float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1):COLOR0 { if (a.x > b.x) return a; return b; }", "control")
    emit(root, manifest, "fp-ternary", "sce_fp_psp2",
         "float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1):COLOR0 { return a.x > b.x ? a : b; }", "control")
    emit(root, manifest, "fp-if-big", "sce_fp_psp2", """
float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1,float4 c:TEXCOORD2):COLOR0 {
    float4 x;
    if (a.x > b.x) {
        x=a*b+c; x=x*b+c; x=x*b+c; x=x*b+c; x=x*b+c; x=x*b+c;
    } else {
        x=b*a-c; x=x*a-c; x=x*a-c; x=x*a-c; x=x*a-c; x=x*a-c;
    }
    return x;
}
""", "control", op="branch-forward")
    emit(root, manifest, "fp-loop", "sce_fp_psp2", """
uniform int n;
float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1,float4 c:TEXCOORD2):COLOR0 {
    float4 x=a;
    for (int i=0; i<n; ++i) x=x*b+c;
    return x;
}
""", "control", op="branch-backward")
    for step in (2, 3):
        emit(root, manifest, f"fp-loop-step{step}", "sce_fp_psp2", f"""
uniform int n;
float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1,float4 c:TEXCOORD2):COLOR0 {{
    float4 x=a;
    for (int i=0; i<n; i+={step}) x=x*b+c;
    return x;
}}
""", "control", op=f"loop-step{step}")
    for suffix, compare in (("eq", "=="), ("ne", "!="), ("lt", "<"),
                            ("le", "<="), ("gt", ">"), ("ge", ">=")):
        emit(root, manifest, f"fp-cmp-{suffix}-big", "sce_fp_psp2", f"""
float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1,float4 c:TEXCOORD2):COLOR0 {{
    float4 x;
    if (a.x {compare} b.x) {{
        x=a*b+c; x=x*b+c; x=x*b+c; x=x*b+c; x=x*b+c; x=x*b+c;
    }} else {{
        x=b*a-c; x=x*a-c; x=x*a-c; x=x*a-c; x=x*a-c; x=x*a-c;
    }}
    return x;
}}
""", "control", op=f"f32-compare-{suffix}")
    emit(root, manifest, "fp-texture2d", "sce_fp_psp2",
         "uniform sampler2D tex; float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(tex, uv); }", "texture")

    emit(root, manifest, "vp-passthrough", "sce_vp_psp2",
         "float4 main(float4 p:POSITION) : POSITION { return p; }", "vertex")
    emit(root, manifest, "vp-uniform-mul", "sce_vp_psp2",
         "uniform float4x4 mvp; float4 main(float4 p:POSITION) : POSITION { return mul(mvp, p); }", "matrix")
    emit(root, manifest, "vp-varying", "sce_vp_psp2",
         "struct O { float4 p:POSITION; float2 uv:TEXCOORD0; }; O main(float4 p:POSITION,float2 uv:TEXCOORD0) { O o; o.p=p; o.uv=uv; return o; }", "interface")
    emit(root, manifest, "vp-geometrizer-poly", "sce_vp_psp2", """
uniform float2 u_screen_size;
uniform float u_z_max;
struct O { float4 position:POSITION; float4 color:COLOR; };
O main(float3 a_pos:TEXCOORD0, float4 a_color:TEXCOORD1) {
    O o;
    float2 ndc=float2((a_pos.x/u_screen_size.x)*2.0-1.0,
                      1.0-(a_pos.y/u_screen_size.y)*2.0);
    float zn=log2(1.0+clamp(a_pos.z,0.0,u_z_max))*0.0602059935;
    float clip_z=zn*1.998-1.0;
    o.position=float4(ndc,clip_z,1.0);
    o.color=a_color;
    return o;
}
""", "integration", project="geometrizer", shader="POLY_VS")
    emit(root, manifest, "vp-geometrizer-poly3d", "sce_vp_psp2", """
struct O { float4 position:POSITION; float4 color:COLOR0; };
O main(float4 a_m0:TEXCOORD0, float4 a_m1:TEXCOORD1, float4 a_m2:TEXCOORD2,
       float4 a_pos:TEXCOORD3, float4 a_color:COLOR0,
       uniform float2 u_xc, uniform float2 u_zoom, uniform float2 u_view,
       uniform float2 u_screen, uniform float u_z_max) {
    O o;
    float4 ph=float4(a_pos.xyz,1.0);
    float vx=dot(a_m0,ph);
    float vy=dot(a_m1,ph);
    float vz=dot(a_m2,ph);
    float ax=(u_xc.x+u_view.x)*2.0/u_screen.x-1.0;
    float bx=u_zoom.x*2.0/u_screen.x;
    float ay=1.0-(u_xc.y-u_view.y)*2.0/u_screen.y;
    float by=u_zoom.y*2.0/u_screen.y;
    float zn=log2(1.0+clamp(a_pos.w,0.0,u_z_max))*0.0602059935;
    float clip_z=zn*1.998-1.0;
    o.position=float4(ax*vz+bx*vx,ay*vz+by*vy,clip_z*vz,vz);
    o.color=a_color;
    return o;
}
""", "integration", project="geometrizer", shader="POLY3D_VS")
    emit(root, manifest, "fp-geometrizer-cmp", "sce_fp_psp2", """
uniform sampler2D u_tex;
uniform float u_force_opaque;
float4 main(float2 v_uv:TEXCOORD0):COLOR0 {
    float4 c=tex2D(u_tex,v_uv);
    return float4(c.rgb,u_force_opaque>0.5 ? 1.0 : c.a);
}
""", "integration", project="geometrizer", shader="CMP_FS")
    emit(root, manifest, "fp-geometrizer-tm2-fast", "sce_fp_psp2", """
uniform sampler2D u_page;
uniform float u_opaque;
uniform float u_cat_match;
float4 main(float2 v_uv:TEXCOORD0):COLOR0 {
    float4 c=tex2D(u_page,v_uv);
    float meta=floor(c.a*255.0+0.5);
    if (u_cat_match>=0.0) {
        float cat=meta>=170.0 ? 1.0 : 0.0;
        if (cat!=u_cat_match) discard;
    }
    if (u_opaque<0.5 && fmod(meta,170.0)<42.0) discard;
    return float4(c.rgb,1.0);
}
""", "integration", project="geometrizer", shader="TM2_FAST_FS")

    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"generated {len(manifest)} shaders in {root}")


if __name__ == "__main__":
    main()
