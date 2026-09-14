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
compact Typed Vita IR. The resulting GXP images match the preserved public
samples byte-for-byte after offset `0x14`, including all USSE words, interface
records, containers, reflection descriptors and strings. The only difference is
the two Sony GUID fields at `0x0c..0x13`; their generation algorithm is still
unknown and open-shacccg currently emits zero there.

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
instruction construction in `shader_profiles.cpp`. The public clear/color/texture vertex
and fragment GXPs remain byte-identical after the migration.

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
P1/!P1 BR forms, float VTST predicates, phi values and general loops remain
fail-closed until their oracle evidence/lowering rules are added.

## Next backend order

1. **Finish typed float/conversion coverage.** Derive additional swizzle encodings and F16->F32/other conversion forms from real words, keeping the single Typed -> Machine lowering path fail-closed.
2. **Complete structured control flow.** BR forward/backward offsets are now oracle-validated. Add float VTST predicate forms and phi/merge lowering, then generalize loops beyond the current label/branch substrate.
3. **Integer data movement/conversion.** Cover the VMOV/VPCK integer forms and bitcasts required to connect U32 computations to actual shader resources.
4. **Texture expansion.** Move beyond the validated dependent-sampler texture shape: SMP, integer texture results, gather and multiple samplers.
5. **Common missing ALU families.** Prioritize VCOMP, VMAD2 and VDUAL based on real traces, then remaining instruction families by corpus frequency.
6. **Resource/reflection generalization.** Derive register counts, parameter types, containers, uniform buffers, literals and dependent samplers from IR instead of current sample-shaped layouts.
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
