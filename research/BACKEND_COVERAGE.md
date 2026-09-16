# Backend coverage plan

This file tracks evidence used to prioritize and validate the Vita IR -> USSE -> GXP backend. It records observable instruction facts and corpus statistics only; no GPL/LGPL implementation source is copied into the runtime.

## Reference snapshots

- Vita3K HEAD: `84184a363aa99c7f331a7e75bdd75f43ff63db08` (GPL-2.0; factual decoder/trace reference only)
- DSVita HEAD: `ae36f93a231751c4b0ef2f6672d50e93a3a91dca` (GPL-3.0; Cg corpus analysis only)
- vitaGL HEAD: `93d5cd5b9e826f4465df6b647a93480a5238cde8` (LGPL-3.0/GPL components; shader/corpus reference only)

The runtime remains governed by `research/PROVENANCE.md`: independently observed binary formats and behavior are usable facts, while GPL/LGPL implementation source is not copied into `src/`.

## DSVita Cg feature inventory

A static scan of the 33 Cg shaders under DSVita's graphics shader directories at the snapshot above found these high-value features:

| Feature | Shaders |
| --- | ---: |
| comparisons | 18 / 33 |
| bitwise operations | 17 / 33 |
| `if` | 14 / 33 |
| `tex2D` | 13 / 33 |
| samplers | 12 / 33 |
| `discard` | 10 / 33 |
| `min` / `max` | 8 / 33 |

Representative shaders also use integer/unsigned values, shifts and masks, integer texture reads, gather operations, variable indexing, bitcasts and nested conditionals. This is why the backend should grow typed scalar/integer IR and control flow before spending much more effort on float-only expression shapes.

## Evidence-backed USSE coverage

### Existing before this pass

- PHAS, NOP, EMIT control encoders
- VMOV raw + semantic codec
- VPCK raw + semantic codec
- V32NMAD raw + semantic codec
- VMAD raw + semantic codec
- public libvita2d GXP regressions for clear/color/texture paths

### Added in this pass

- extended predicate model (`p0`..`p3`, inversions exposed where the 3-bit form supports them)
- predication on VMOV, VPCK, V32NMAD and VMAD semantic builders
- VTST raw codec
- semantic scalar U32 comparison -> predicate register
- VBW raw codec
- semantic U32 AND/OR/XOR/SHL/ROL/SHR/ASR
- VBW rotated/inverted immediate encoding with fail-closed behavior for unrepresentable constants
- KILL raw codec
- semantic KILL for the short predicate forms
- compact Machine IR: 32-bit operands, 16-byte generic instructions and
  descriptor-driven use/def metadata
- dynamic virtual-predicate allocation with lifetime reuse and opcode-specific
  encodability constraints
- machine lowering for U32 compare -> VTST and predicate -> KILL

Exact external trace anchors used by tests include:

- `48880185b007c006` - U32 equality test to predicate 1
- `48880181b007c008` - U32 equality test to predicate 0
- `4d880181b007c006` - predicated U32 equality test
- `4e880185b007c007` - predicated U32 equality test
- `50810009e0400600` - U32 OR with immediate zero
- `50810009e0000000` - U32 OR with immediate zero
- `3d800501c1040040` - predicated F32 VMOV
- `f9300406f0000408`, `f9300406f0001c38`, `f9300406f000060c` - KILL using predicate 1; opaque/don't-care payload bits are round-tripped by the raw codec

The semantic KILL encoder emits zero in don't-care fields rather than reproducing unrelated payload bits from a captured shader.

## Compact declarative backend rules

Mechanical representation is now kept inside C++ and shared through small
compile-time descriptors instead of external code generation.

- `src/usse/compact_encoding.hpp` implements generic `BitField<>` and
  `Encoding<>` templates for 64-bit instruction matching/packing.
- `src/usse/raw_encodings.hpp` is the sole raw-USSE layout description for the
  supported families; no Python generator or generated `.inc` is required.
- `src/backend/machine_ops.inc` is the sole Machine IR opcode/role list and is
  included to generate both enum values and runtime descriptors.
- `src/backend/typed_ir_ops.inc` applies the same pattern to typed Vita IR;
  typed values are 4-byte handles and instructions are 16 bytes.
- Machine operands stay 4 bytes and instructions stay 16 bytes; typed template
  factories improve construction without changing storage.
- `ProgramBuilder::instruction<T>()` dispatches semantic encoders by type, so
  the builder itself does not grow one method per USSE family.
- `src/core/compact_layout.hpp` provides reusable member/array/nibble field
  writers for GXP wire metadata. Derived relative offsets remain explicit.

The design rule is: derive masks, widths, roles, wire offsets and trivial
dispatch mechanically; keep semantic decisions, hardware constraints and
fail-closed policy handwritten. This preserves the useful separation found in
compact ISA decoders without importing Vita3K's GPL implementation.

## glslang HLSL frontend validation

An optional host-side Cg compatibility path now uses glslang 16.6.0 in HLSL
DX9-compatible mode to produce SPIR-V. glslang's optimizer is disabled so the
existing SPIRV-Tools stage remains the single explicit optimizer.

Observed source acceptance during the initial probe:

- `oracle_corpus_v2`: 87 / 87 compile unchanged;
- `oracle_corpus`: 65 / 70 compile unchanged; the five rejected probes are the
  scalar/narrow-vector `wzyx` cases whose component selections exceed the
  declared value width;
- public libvita2d Cg corpus: 5 / 7 compile unchanged and 7 / 7 after the sole
  compatibility rewrite `TYPE out name` -> `out TYPE name` used by its two
  vertex shaders.

With glslang + SPIRV-Tools + SPIRV-Cross enabled together, all seven public
libvita2d shaders now compile end-to-end from original Cg source through the
compact Typed Vita IR. All seven now deliberately target the newer SDK 3.0.0
oracle and are byte-identical to ShaccCg 3.0.0 output for their validated GXP
metadata and USSE. The preserved public v1.4 binaries remain historical decoder
and compatibility fixtures. Sony GUID generation remains unknown and open-shacccg
currently emits zero there.

The vitaGL FFP fidelity sweep uses the same SDK 3.0.0 oracle. Direct texture,
texture-tint, point-sprite and alpha-test fragment shapes are byte-identical to
their oracle equivalents. The private Unicorn runner still hits Sony diagnostic
403 on `sampler2D[N]` parameters; scalarizing a constant sampler element to its
equivalent TEXUNIT binding produces the same GXP and is used only to isolate
oracle behavior, not as an OpenShaccCg source rewrite. In particular the
two-texture FFP `replace` tail optimizes to `tex[1]`/TEXCOORD1 and matches the
SDK 3.0 TEXUNIT1 direct-read image byte-for-byte (256 bytes, sampler query
slot 1 = `0x0302`).

Linear fog is now byte-identical as well. The SDK 3.0.0 profile moves reciprocal
and distance setup into a three-word secondary program, uses the v1.5 WPOS
additional-input layout, and reduces the primary stream from the older 16-word
generic lowering to 11 words. The only newly exposed ISA shape is the validated
F32 VMAD2 scalar MAD used by `(fog_far-distance)/fog_range`; all remaining words
are built from existing VCOMP/V32NMAD/VMAD/VPCK semantics.

Exponential-squared fog is byte-identical to SDK 3.0.0 too. Its 12-word primary
stream adds three narrowly validated forms over the linear path: a VMAD3 with
the vec3 extended GPI1=`000` swizzle, a dual-issued scalar FMUL + fog-color VMOV,
and the SDK 3.0 Exp2 VCOMP destination selector. The three-word secondary stream
precomputes `-density * distance` and its second multiply; clamp/mix/output then
reuse the linear-fog semantics.

The vitaGL sRGB output transform now also reproduces SDK 3.0.0 byte-for-byte.
The structural Typed-IR recognizer anchors the three cutoff selects plus the
`log2 -> * (1/2.4) -> exp2` RGB chain and affine/lerp tail rather than shader
names. Codegen emits the observed 20-word v1.5 primary stream, six literal slots,
PA=4/SA=6, and compiler version `0x00033a90`.

The simple FFP vertex profiles now follow SDK 3.0.0 too. Baseline position+PSIZE
and the one-texture transform move the PSIZE VBW immediately after PHAS with
`no_schedule`, eliminating the older NOP; the COLOR variant keeps the same USSE
stream and only migrates to v1.5 metadata. All three are byte-identical to the
SDK 3.0 oracle (364, 396 and 448 bytes on disk respectively).

The two- and three-texture vertex profiles require no ISA change at all: their
14- and 18-word primary streams plus three-word secondary clamp streams already
matched SDK 3.0.0. Migrating only the v1.5 header/compiler fields makes the full
images byte-identical as well (536 and 596 bytes on disk).

The extended three-texture fragment tail needs a real SDK 3.0 ISA update. After
pass1 replaces the earlier color, only TEXUNIT1/2 remain live; scalarizing just
the unsupported `sampler2D[3]` declaration in the private oracle preserves the
FFP DAG and reveals an 8-word combine. Open now matches it exactly: VPCK stages
texture2 RGB, a VMAD3 with extended GPI0=`111` folds the RGB add, MIN/MAX clamp
in place, alpha multiplies into T60.w, and one final VPCK writes COLOR. Both
sampler-query slots are `0x0301`, including the v1.5 `0x30` iterator anchor.

The fixed-point halfword vertex path is also byte-identical to SDK 3.0.0 when
the oracle is run with the exact SceShaccCgExt extension hook required for Cg
`bit_cast`. Sony collapses the old 72-word generic bitcast/narrow expansion to
21 primary instructions: repeated/scaled U16/S16->F32 VPCK unpack, one VADD+VMOV
VDUAL, the existing matrix VMAD forms, and PSIZE output. The resulting real
vitaGL image is 568 bytes with PA=12/SA=36 and compiler `0x00033a90`.

The Phong vertex stage now matches SDK 3.0.0 byte-for-byte as well. The 1004-byte
v1.5 image uses 48 primary + 12 secondary instructions, PA=28/SA=72 and flags
`0x00190004`. The profile is assembled from semantic VMOV/VPCK/VMAD/VBW/VCOMP/
V32NMAD operations plus two SMLSI repeat-control words and the observed
VDP+VMOV / FRCP+VMOV VDUAL forms; no operand-bearing raw instruction is injected
by the runtime profile.

The glslang HLSL frontend is intentionally experimental because its upstream
HLSL mode is deprecated. It is still a high-value compatibility route and
validation oracle while the backend migrates toward SPIRV-Cross -> Typed IR.

## Optional optimized SPIR-V frontend

The host build now has two independent optional stages before Vita lowering:

- `OPENSHACCG_ENABLE_SPIRV_TOOLS` runs the SPIRV-Tools performance recipe with
  validation enabled while preserving interfaces, bindings and specialization
  constants.
- `OPENSHACCG_ENABLE_SPIRV_CROSS` parses the resulting module and performs
  entry-point/resource reflection. Its adapter now lowers scalar U32 control
  work plus float vectors, UBO members, samplers, dependent samples, float
  arithmetic/negate, position expansion, matrix transforms and output stores into the
  compact Typed Vita IR.

A regression exercises both the integer path `SPIR-V -> SPIRV-Tools ->
SPIRV-Cross -> Typed IR -> Machine IR -> VBW/VTST/KILL` and the seven public
Cg shaders through `Cg -> glslang -> SPIRV-Tools -> SPIRV-Cross -> Typed IR ->
GXP`. The old dependency-free SPIR-V lowering remains a temporary fallback and
the default Vita static build does not link host SPIRV-Tools/SPIRV-Cross
libraries.

## Bank-aware float Machine IR

Machine IR now owns both the physical allocation policy and instruction-level
representation used by the validated float paths. A compact 4-byte value
descriptor plus 8-byte live range colors scalar TEMP values, paired F32/F16
TEMP spans, the TEMP124..127 GPI aliases and the observed VMAD accumulator slot.
The existing U32 Machine IR allocator uses the same core.

The generic 16-byte Machine instruction now covers PHAS/NOP/EMIT, VMOV, VPCK,
V32NMAD and the evidence-backed VMAD matrix profile. VMAD references its two
virtual GPI values through one packed pair operand. A zero-word
`DependentSample` pseudo-op models the validated texture-unit handoff so the
coordinate GPI and resulting TEMP participate in normal lifetimes without
inventing an SMP instruction that is absent from the public sample.

The vertex, texture-tint and generic fragment arithmetic paths no longer carry
their own TEMP/GPI free lists, direct register-bank selections or semantic USSE
instruction construction in `shader_profiles.cpp`. All seven public libvita2d
profiles track the SDK 3.0.0 v1.5 oracle layouts. The two matrix vertex profiles
share the same 3.0.0 VMAD lane/source ordering and SA=16 metadata, while `clear_v`
uses the compact two-V32NMAD constructed-position form with no secondary state.

Typed IR now lowers F32x4 mul/add/sub/min/max, scalar dot, negate and absolute
through the same Machine IR. SPIRV-Cross also recognizes `OpFNegate` without
requiring SPIRV-Tools to canonicalize it first. Narrow float vectors and
unvalidated config/swizzle forms are rejected rather than widened implicitly.

The SPIRV-Cross path now also parses the validated `GLSL.std.450` FAbs/FMin/FMax
forms, identity/X-splat vector shuffles, scalar-to-float4 splats and F32x4 to
F16x4 `OpFConvert`. The latter lowers through a compact Machine IR `PackValue`
operation that derives the second consecutive F32 source register from the
logical float4 value and emits the known F32->F16 VPCK form. Y/Z/W shuffles,
arbitrary permutations and reverse/other float conversions remain fail-closed.

The stage-specific compatibility IR bridges have now been removed from runtime
compilation. SPIRV-Cross and the dependency-free recognizer both produce
`TypedShader` directly, and canonical vertex/fragment forms select compact
Machine/GXP profiles without constructing `VertexIr` or `FragmentIr`. Generic
arithmetic likewise remains SSA-like Typed/Machine IR end to end.

## Oracle-validated branch control flow

The local clean-room Unicorn runner can now execute the user-supplied original
SceShaccCg 1.6.5 module far enough to compile differential Cg shaders. Two clean
corpus cases force control flow that the compiler cannot if-convert: a large
`if/else` and a dynamic loop. They establish the USSE BR encoding independently
of the public libvita2d corpus:

- `0xf90000400000000c`: P0, forward +12 instructions
- `0xf80000400000000b`: unconditional, forward +11
- `0xfd00004000000006`: !P0, forward +6
- `0xf8000040000ffffa`: unconditional, backward -6

The offset is therefore a signed 20-bit instruction delta relative to the BR
itself. Raw/semantic USSE codecs and compact Machine IR labels now reproduce
these words exactly. Typed IR also owns label side tables plus conditional and
unconditional jumps, and the SPIRV-Cross U32 control-flow adapter lowers a
structured `SelectionMerge`/`BranchConditional` graph through those labels.
P1/!P1 BR forms and general phi consumers remain fail-closed until their oracle
evidence/lowering rules are added.

The same oracle corpus now carries six large F32 compare cases. They establish
the VTST floating subtract/test profile for `==`, `!=`, `<`, `<=`, `>` and `>=`.
All six share `precision=F32`, ALU select 0/op 14 and differ only in the
sign/zero/CR-combine tests. Semantic USSE, Machine IR and Typed IR reproduce the
oracle words exactly; the SPIRV-Cross scalar-F32 control adapter maps the ordered
SPIR-V comparisons onto this profile.

The first value-producing merge is now covered end to end. Glslang's validated
fragment pattern (`BranchConditional`, two predecessor blocks, two-way `OpPhi`,
single COLOR0 store) is lowered without inventing a physical phi instruction:
the output store is sunk into each predecessor before its jump to the merge.
`fp-if-big.cg` now compiles through `Cg -> SPIR-V -> Typed CFG -> Machine BR ->
GXP`; the regression requires PA=12, the oracle control flags, the exact F32
VTST anchor and decodable non-zero BR offsets. More general phi consumers and
general non-output selection phis remain fail-closed.

The differential control corpus now compiles 12/12 cases with both Sony and
OpenShaccCg. Three frontend/control compatibility gaps were closed
without adding guessed machine families: glslang's case-free `OpSwitch` wrapper
for early returns is treated as its exact unconditional-default jump semantics,
`OpFUnordNotEqual` is mapped to the oracle-anchored Cg `!=` VTST profile, and a
float4 `OpSelect` feeding COLOR0 is desugared into the already validated Typed
BR/store CFG. Sony if-converts that ternary to a predicated in-place VMOV; Open
currently favors the longer validated BR form.

The dynamic-loop oracle case also identifies the integer control primitives
around the back-edge. `for (int i=0; i<n; ++i)` uses a signed-32 VTST `<`
profile (`0x48a8068130078000`) followed in the latch by an I32MAD2 update/feed
pair (`0xd08180042020c001`, `0xd09080040000c001`). Differential `i+=2` and
`i+=3` cases change only the immediate source of the update word, anchoring that
field independently. Raw/semantic I32MAD2 and signed-loop VTST codecs reproduce
these words exactly. The loop lowering now represents the float4 and S32 phis as
explicit mutable state, emits the exact Sony counter init/compare/update words,
and keeps the float body on the existing validated V32NMAD path. The oracle also
establishes the GXP literal table as 8-byte `{resource_index,value_bits}` entries:
loop literals `{0,0}`/`{1,1}` occupy container 19 at SA2/SA3 while the S32 `n`
uniform occupies container 14. Open's three positive-step loop GXPs match Sony's
observable metadata exactly; instruction selection differs in the float body and
extra state moves/branches. Decrementing loops use a different source profile and
remain intentionally unsupported.

## Oracle-exact swizzle and constant profiles

Two small fragment probes now reproduce Sony byte-for-byte outside the two GUID
fields. `fp-swizzle-wzyx` anchors F32->F16 VPCK component selectors `3,2,1,0`
to `0x40800d7ea0024083`. `fp-constant-red` anchors the literal-backed F16 VMOV
`0x38800422c5000000` and exposes an additional canonical GXP layout: with no
fragment inputs, primary code begins at `interface+0x18`, so PHAS overlaps the
last qword of the interface record and the zero-length secondary anchor is
`primary-4`. The constant's two literal entries are `{0,0x00003c00}` and
`{1,0x3c000000}` in container 19. Host tests cover the layout independently of
the private oracle.

Across the non-ALU small-feature probes (swizzle, constant, uniform, texture and
basic vertex shapes), OpenShaccCg now compiles 8/8. Five of those probes are
byte-identical to Sony outside the two GUID fields: `fp-swizzle-wzyx`,
`fp-constant-red`, `vp-passthrough`, `vp-uniform-mul` and `vp-varying`.

## Oracle-exact standalone vertex profiles

The three small vertex probes exposed a glslang HLSL compatibility quirk and two
new binary facts. HLSL `: POSITION` can arrive as plain Location 0 with no
UserSemantic/BuiltIn decoration; when a vertex has no BuiltIn Position output,
that Location-0 return is treated as the oracle-backed position slot. Sony SDK
1.6.5 standalone vertex GXPs also reserve one 32-bit word between the 32-byte
interface and primary code (`primary=0xbc`, zero-secondary anchor `0xb8`), unlike
the preserved libvita2d vertex binaries which use the no-gap convention.

The resulting profiles are exact outside GUIDs:

- `vp-passthrough`: PHAS + repeated VMOV `0x3880152183000000` + EMIT;
- `vp-varying`: PHAS + repeat-2 VMOV `0x3880252183000000` + EMIT;
- `vp-uniform-mul`: PHAS + NOP + F32 VPCK `0x40800dbcaf998002` + repeated VMAD
  `0x18903081c011a200` + EMIT.

The last word proves VMAD control bit 53 is not a fixed opcode bit: the public
libvita2d matrix VMADs use 1, while this repeated external-mode mat4 profile uses
0. The raw/semantic codec therefore exposes the observed bit explicitly while
retaining 1 as the default for existing public regressions.

## Multi-input vector arithmetic gate

The 77-case ALU oracle sweep exposed a frontend/profile bottleneck before any
new arithmetic family was needed: Cg probes declare three parameters even when
only one or two survive into the optimized expression graph. Typed arithmetic
previously counted all reflected inputs and therefore rejected every ALU probe.
The lowering now walks dependencies from the stored output and derives the
contiguous reachable homogeneous F32 vector locations only. Standalone no-uniform
arithmetic profiles use the oracle-observed SDK 1.6.5 metadata for one/two/three
inputs, while the existing uniform-arithmetic profile is preserved.

As a result, the `float4`/`half4` slice first improved from 0/22 to 18/22 without
a new USSE opcode. Add/sub/mul/min/max/abs/neg/mad/dot compile end to end in both
source spellings.

`saturate` then raises that slice to 20/22 without adding a new hardware family:
GLSL.std.450 `FClamp` is accepted only for the exact vector constants 0 and 1,
then lowered as the already validated V32NMAD `MAX(x,0)` followed by
`MIN(x,SPECIAL1.yyyy)`. The SPECIAL1.y constant is independently anchored by
the public clear_v homogeneous-position path. Division is the first case in this
slice that genuinely requires VCOMP coverage.

## F32x4 reciprocal/division profile

Differential `a/b` versus `b/a` probes isolate the reciprocal VCOMP source-pair
field from its lane selector. For an even PrimaryAttribute float4 base, the four
F32 reciprocal words use fixed base `0x308008008f800000`, encode `PA/2` in bits
8..15, and use lane signatures `{0x01,0x02,0x84,0x88}` with bit 35 set for odd
lanes. Both PA0 and PA2 variants are covered by raw and semantic round-trip
tests.

The direct `float4 a / float4 b` Sony sequence is reproduced exactly outside
GUIDs: PHAS + NOP, four VCOMP reciprocal operations on `b.xyzw`, F32->F32 VPCK
staging `a` into GPI1 (`TEMP125`), and V16NMAD combine
`0x10a4478600040f7c`.

The same probes expose the general SDK 1.6.5 multi-varying fragment layout:
after the main 32-byte interface, each additional float4 input contributes a
16-byte descriptor (`0f 10/20 c0 0e` plus `0x30` in the second qword), followed
by the usual 8-byte zero-secondary anchor. `fp-add-float4`, `fp-mad-float4`,
`fp-if-big` and `fp-div-float4` independently confirm this structure. The GXP
writer models it explicitly instead of treating those bytes as padding.

With this cut the `float4`/`half4` ALU slice compiles 22/22. F32x4 division is
byte-identical outside GUIDs. `half4` still arrives from glslang HLSL as F32x4,
so it intentionally uses the validated F32 profile until a frontend route that
preserves half precision is available; Sony's half profile remains separately
observable in the oracle corpus.

## Narrow F32 vector arithmetic

The same Typed/Machine path now carries F32x2 and F32x3 without widening the
16-byte Machine instruction. V32NMAD uses destination masks `0x3`/`0x7`; F32x2
values occupy one physical F32 register, while F32x3 values occupy the validated
two-register span. Input Location stride follows the oracle layout: float2 uses
one PA register per location, float3 uses two. Output VPCK uses matching `xy` or
`xyz` masks.

The GXP interface writer also generalizes the additional-input descriptors:
float2 uses component signature `0x40`/tail `0x10`, while float3 shares the
`0xc0`/`0x30` signature observed for float4.

Narrow division and dot are now anchored too. F32x2 division exposes the odd-PA
half-register VCOMP selector (`...81/...82` for PA1.xy); F32x3 reuses the even-PA
VCOMP lane profile. Width-specific numerator staging plus fixed V16NMAD combines
reproduce Sony exactly. Dot-splat float2 uses a semantic V32NMAD multiply/stage
pair plus the observed V16 reduction, while float3 stages both vectors through
VPCK before its V16 reduction. `fp-div-float2`, `fp-dot-float2`, `fp-div-float3`
and `fp-dot-float3` are byte-identical outside GUIDs.

Differential probes therefore compile 33/33 across `float2`, `float3` and
`half2`. Half precision still arrives as F32 from glslang and intentionally uses
the F32 profiles.

## Packed scalar F32 arithmetic

The final 22 ALU probes establish how scalar stage inputs are packed. Sony maps
Location 0/1 to `PA0.x/y` and Location 2 to `PA1.x`; VMOV staging words make the
component selection observable independently. Open keeps `MachineOperand` at four
bytes by storing an optional physical-component selector in otherwise-unused
payload bits. Ordinary Typed `Input<F32>(n)` retains its direct-register meaning;
the SPIR-V adapter explicitly emits component-selected inputs for reflected scalar
locations, so existing Typed/API semantics do not change.

Scalar add/sub/mul/min/max/abs/neg/saturate/mad (and scalar `dot`, which glslang
reduces to FMul) reuse masked V32NMAD plus the normal output pack. Scalar division
adds one oracle-backed VCOMP variant: PA0.x reciprocal is
`0x308008008f800001`, while PA0.y sets bit 35 and becomes
`0x308008088f800001`. The exact F32 scalar division profile is PHAS + NOP + that
Y reciprocal + `0x3880050081f40000` staging + V16NMAD
`0x10a4008600040f7c`.

Sony and Open now compile all 77/77 ALU corpus probes. F32 scalar division plus
F32x2/F32x3/F32x4 division and the narrow F32 dot-splat profiles are byte-identical
outside GUIDs; many other operations intentionally differ in instruction selection
because Sony prefers compact V16NMAD/VMAD2 forms while Open uses its validated
V32NMAD path.

## Scalar S32 uniform data path

The first non-loop integer resource path is now oracle-backed end to end. Sony Cg
1.6.5 accepts `int`/`int2`/`int4`, but not `uint`/`uint4` source type names, so the
clean differential corpus starts with scalar `uniform int` rather than claiming a
source-level unsigned profile. Uniforms avoid the separate float-interpolation to
integer conversion required by integer stage inputs and isolate the actual bitwise
data path.

Six probes are exact outside GUIDs with both compilers: direct `int` uniform output,
register-register OR, XOR with `4660`, AND with `255`, left shift by 3 and arithmetic
right shift by 3. They anchor these existing VBW semantic words respectively:

- uniform copy: `0x5081000ae0000000` (`OR SA0, 0 -> PA0`);
- register OR: `0x5080000aa0000080`;
- XOR immediate: `0x58810002a0090034`;
- AND immediate: `0x50810002a000407f`;
- shift left: `0x60810002a0000003`;
- arithmetic shift right: `0x6881000aa0000003`.

All six terminate through the same oracle VPCK `0x40850946a0000000`, the narrowly
validated scalar S16->F16 COLOR packing form. Its unused second source is canonically
encoded as Immediate0. The OR/XOR/AND/shift profiles place their bitwise instruction
in the fragment secondary stream before that VPCK; this independently extends the
GXP writer's validated secondary layout from one to two qwords. Secondary starts at
`interface+20`; a two-word stream extends four bytes past the 32-byte interface and
primary code resumes at the next 8-byte boundary.

Typed/Machine bitwise lowering now accepts matching scalar S32 as well as U32 while
keeping the same 16-byte MachineInstruction and 4-byte operand handles. Attribute
conversion is now covered separately as well. Sony emits the exact same four-word
primary program for `int main(int a:TEXCOORD0)` and `int main(float a:TEXCOORD0)
{ return (int)a; }`: PHAS, F32->F16 staging VPCK `0x40810d46a0000000`, then fixed
V16NMAD phases `0x10a40084a0042000` and `0x10a400a620041000`. Open reproduces
both GXPs byte-for-byte outside GUIDs. The two V16 phases are intentionally exposed
as one narrow semantic profile rather than guessing the general V16NMAD layout.

Scalar S32->F32 conversion is now exact outside GUIDs as well. Sony's canonical
`uniform int x; return (float)x;` profile uses a 9-word secondary program and a
2-word primary program. The secondary stream is reconstructed from four VBW words,
two existing I32MAD2 forms, fixed U16->F32 VPCK phases
`0x40810786a0c00081` / `0x40810786a0800080`, and the one still-opaque VMAD2 core
`0x00800086a0403042`; the final VBW carries the independently observed END bit.
Its literal table contains `{0,0x477fff00}` and `{1,1}` in container 19, while the
uniform remains container 14 resource 0. The primary stream is PHAS plus
`0x40810d46e0000000`. Vector integer conversions, bitcasts and a source-level U32
profile remain fail-closed until separately derived from oracle or SPIR-V evidence.

The first vector integer slice is now covered without expanding the compact type
handle. `TypedType` already occupies all sixteen encodable 4-bit values; for vector
bitwise work, signedness does not affect the lane bits, so signed SPIR-V `int2` maps
to the existing `U32x2` bit-container slot while scalar `S32` remains distinct for
comparisons/conversions. `uniform int2` passthrough and OR are both exact outside
GUIDs. Pass uses the normal primary uniform copy plus fixed VPCK words
`0x408106caa0000080` and `0x4085094ea0010000`; OR prepends secondary VBW words
`0x5080000aa0200181` and `0x5080000aa0000100`. These probes also extend the
validated fragment-secondary layout to four qwords (and the oracle has separately
observed five/eight-word int4 streams, though int4 lowering remains closed).

## Geometrizer vertex integration

The first production-shaped integration target is a Cg translation of Geometrizer's
`POLY_VS` semantics: float3 position + float4 color inputs, packed float2/scalar
uniforms, two scalar divisions, dynamic `clamp`, `log2`, depth scaling and a
composed POSITION + COLOR output. Sony compiles the probe to 14 primary + 4
secondary instructions. Open now compiles the same source through generic
Typed/Machine vertex lowering using field-level reciprocal/Log2 VCOMP, V32NMAD and
VMOV. Its current schedule is 21 primary instructions and no secondary program;
reflection, flags and the vertex interface match, while secondary-register and
literal counts differ because Open does not yet hoist reciprocals or materialize
Sony's otherwise-unused `-1` literal.

Reaching this shader generalized nested uniform component access chains, F32
component extraction, scalar F32 literals backed by the GXP literal table, dynamic
MAX/MIN clamp lowering and float4 composition via a Typed side table. The hot Typed
and Machine instruction records remain 16 bytes.

`POLY3D_VS` is the second production-shaped target and now compiles through the
same generic path. It expands the interface to five float4 attributes and five
float uniforms, materializes the intermediate `float4(a_pos.xyz,1)` only because
the value feeds three dot products, and preserves Sony's observable resource
metadata: program flags `0x00090004`, PA=20, TEXCOORD0..3 + COLOR0 semantics and
uniform word offsets 0/2/4/6/8. Sony emits 20 primary + 10 secondary instructions;
Open now hoists denominator-only screen reciprocals and emits 39 primary + 2
secondary instructions (41 total, down from the original 43-word primary-only
baseline). Code-size/scheduling differences are tracked separately in
`research/OPTIMIZATION_BACKLOG.md` so they do not obscure correctness coverage.

`CMP_FS` is the first production fragment target and is exact outside GUIDs. Its
`texture2D + (u_force_opaque > 0.5 ? 1 : sample.a)` shape anchors the F32
uniform-vs-special-constant VTST word `0x48898a81d003800c`, a semantic `!P0`
predicated VMOV, and the sampler/control secondary-prefix layout. Open reconstructs
all 7 primary + 1 secondary words from semantic encoders and emits the same 320-byte
GXP layout as Sony.

`TM2_FAST_FS` is the fourth integration target. It adds a direct texture sample,
`floor`, `fmod`, a scalar float select, short-circuit boolean control and two discard
paths. `floor` lowers algebraically to `x-frac(x)`, `fmod` to
`x-floor(x/y)*y`, the scalar select uses initialize + predicated-update VMOV, and
`OpLogicalAnd`/kill blocks lower through the existing compact Typed label table and
BR/KILL Machine path. The direct sample is represented as the PA0/PA1 value supplied
by the iterator/texture machinery, so it adds no fake USSE sample instruction.
Sony emits 27 primary instructions; the generic Open correctness baseline emits 31.
The integration oracle feature is now 4/4 compilable, with CMP byte-identical outside
GUIDs and the remaining three shaders tracked as optimization baselines.

Backend fallback diagnostics now retain the SPIRV-Cross Typed-path failure when
the dependency-free parser also rejects a shader, so future oracle sweeps expose
the actual higher-level coverage gap instead of only the final fallback error.
Sony often selects V16NMAD/VMAD2 and tighter scheduling, whereas Open currently
uses its validated V32NMAD path, so compilation coverage is the milestone here,
not byte identity.

## Real-world compatibility gate

The differential probes above remain the evidence source for individual hardware
facts, but their 115/115 compile result is not a meaningful estimate of general
SceShaccCg compatibility. A separate external-corpus runner now pins real shader
workloads in `research/real_world_corpus.json` and keeps their GPL/LGPL source out
of the repository and runtime.

The initial production-shaped baseline is:

| Project | Passing | Captured Cg | Known targets | Captured compile rate | Target rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| DSVita | 2 | 36 | 36 | 5.6% | 5.6% |
| vitaGL | 2 | 24 | 24 | 8.3% | 8.3% |
| Geometrizer | 4 | 4 | 14 | 100.0% | 28.6% |
| **Overall** | **8** | **64** | **74** | **12.5%** | **10.8%** |

DSVita compile units model the source concatenation and preprocessor variants
actually selected by its Vita runtime rather than merely counting `.cg` files.
vitaGL contributes its four built-in precompiled Cg sources plus a concrete
branch-oriented matrix of its generated fixed-function vertex/fragment templates.
Geometrizer has fourteen Vita GLES shader constants; four currently have clean Cg
fixtures derived from their vitaGL translation and ten remain explicit capture
gaps instead of disappearing from the denominator.

The first sweep immediately found one high-fanout frontend issue: DSVita's global
`TYPE in/out name : SEMANTIC` declarations caused all 36 compile units to fail
before reaching Typed IR. Normalizing that Cg spelling moved the corpus to 2/36
and exposed the deeper distribution: current captured failures are split between
frontend compatibility and Typed/backend coverage rather than one monolithic
parse failure. `tools/real_corpus.py` records the exact diagnostic category for
each case and `research/real_world_baseline.json` protects every case that has
already reached green.

This gate changes prioritization: work that turns a repeated real-world failure
class green is preferred over Sony instruction-count parity unless the codegen
optimization is required for correctness or hardware acceptance.

## Next backend order

1. **Resource/reflection generalization.** Real DSVita/vitaGL failures make uniform blocks, uniform arrays, multiple varyings/samplers and non-COLOR layouts the largest shared production gap. Generic vertex attributes already derive masks, PA counts, semantics and the `0x4` large-input flag from IR; extend the same principle to these resources rather than adding sample-shaped metadata.
2. **Integer/bitcast and texture expansion.** DSVita immediately exercises Cg `short`/`unsigned`, bitcasts, integer texture results and gather operations. Integer oracle coverage is 11/11 exact for the current scalar/int2 slice, but real coverage needs int4/vector conversions, bit containers and the corresponding texture paths.
3. **Finish typed float/composite coverage.** Generalize unresolved vector extracts/shuffles/composite construction, min/max operands, uniform members and other forms already emitted by glslang for real shaders while keeping unsupported encodings fail-closed.
4. **Complete structured control flow.** BR forward/backward offsets, six F32 VTST compares, direct COLOR0 two-way phi merge, output `OpSelect` and positive-step dynamic loops are validated; the control corpus is 12/12. Next generalize remaining phi consumers and derive decrement/other loop-update profiles from oracle probes.
5. **Capture remaining Geometrizer translations.** Record the ten still-missing vitaGL GLSL-to-Cg outputs so all fourteen real Vita renderer shaders participate as compile inputs, then use their failures to drive the same generic backend.
6. **Codegen optimization from real demand.** The 77-case ALU language corpus is complete. Prioritize VMAD2/VDUAL/V16/secondary scheduling only where it closes a real shader constraint or materially improves production code; detailed debt remains in `research/OPTIMIZATION_BACKLOG.md`.
7. **Hardware gate.** Treat a capability as complete only after host regressions plus real-Vita `sceGxmProgramCheck`/render validation where possible.

## Definition of progress

A new family is not considered implemented merely because decode/encode round-trips. The preferred progression is:

1. capture or locate a legal real instruction;
2. decode its fields independently;
3. add raw bit-exact round-trip tests;
4. add a semantic builder that reconstructs the real word;
5. expose it through `ProgramBuilder`;
6. expose the validated form through compact Machine IR;
7. lower a typed Vita IR operation through it;
8. place the resulting stream in a generated GXP;
9. validate on hardware.
