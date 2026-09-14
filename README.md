# open-shacccg

MIT-licensed, clean implementation of a runtime shader compiler for PlayStation Vita with a compatibility layer for the public `SceShaccCg` API.

## Status

The project is a backend-development workbench with a real GXP read/write path. It deliberately does **not** emit guessed shader machine code: unsupported USSE lowering fails with a diagnostic until the relevant instruction family is independently validated.

Implemented now:

- host-buildable `SceShaccCg` core API shim (7 functions used by vitaShaRK)
- observed `sceShaccCgInitializeCompileOptions` defaults/lifetime behavior covered by tests
- allocator and compile-output ownership semantics
- callback-based source acquisition, including stable diagnostic filename ownership after `releaseFile`
- public compiler-core API independent of Sony ABI
- direct `vsc_compile_spirv()` path so USSE/GXP development does not depend on the Cg frontend
- experimental glslang HLSL frontend for the Cg-compatible subset, enabled with `OPENSHACCG_ENABLE_GLSLANG`
- dependency-free SPIR-V reader/lowerer remains the portable fallback
- optional SPIRV-Tools `-O`-style optimization and SPIRV-Cross parsing/reflection before Vita lowering
- SPIRV-Cross -> compact Typed Vita IR for scalar U32 control work plus F32/F16 vector resources,
  dependent texture sampling, float arithmetic/negate, homogeneous position construction and mat4 transforms
- shared bank-aware Machine IR live-range allocation for scalar TEMP values, F32/F16 TEMP spans,
  validated GPI aliases and the public VMAD accumulator convention
- compact Machine IR emission for PHAS/NOP/EMIT, VMOV, VPCK, V32NMAD and the validated VMAD matrix form,
  including a zero-word dependent-sample pseudo-op that keeps texture-result lifetimes explicit
- direct Typed IR -> Machine IR lowering for F32x2/F32x3/F32x4 multiply/add/sub/min/max,
  negate/absolute/saturate, width-aware dot-splat profiles, scalar/X splats and the validated F32x4 -> F16x4 VPCK conversion
- generic fragment arithmetic now tracks only input locations reachable from the output expression,
  so one/two/three-input homogeneous F32 vector graphs lower without being blocked by unused Cg/HLSL parameters
- F32 vector `saturate` lowers fail-closed from GLSL.std.450 FClamp with exact 0/1 bounds to
  validated masked `MAX(x,0)` + `MIN(x,SPECIAL1.yyyy)` V32NMAD forms
- the oracle ALU corpus compiles 77/77 cases across scalar, float2/float3/float4 and
  their HLSL `half` spellings; packed scalar inputs use explicit PA component selectors
  inside the existing 4-byte Machine operand handle
- oracle-exact direct scalar/F32x2/F32x3/F32x4 division and narrow F32 dot-splat profiles use
  width-aware VCOMP/V32NMAD/VPCK staging plus validated V16NMAD reductions; half vectors compile
  through the F32 path
  because the glslang HLSL frontend does not preserve half precision in SPIR-V
- oracle-exact fragment profiles for `wzyx` and the constant `float4(1,0,0,1)`,
  including swizzled VPCK, literal-table placement and the primary/interface overlap convention
- oracle-exact standalone vertex profiles for float4 position passthrough,
  float4 position + float2 TEXCOORD passthrough, and `mat4 * float4`, including the SDK 1.6.5
  vertex padding-word layout and the repeated external-mode VMAD form
- SPIRV-Cross lowering for `GLSL.std.450` FAbs/FMin/FMax, validated X-splat shuffles and `OpFConvert`
- oracle-validated USSE BR encoding with signed 20-bit forward/backward offsets,
  compact Machine/Typed label tables and structured U32 conditional/unconditional branches
- oracle-validated F32 VTST compare profiles for `==`, `!=`, `<`, `<=`, `>` and `>=`,
  exposed through Machine/Typed IR and the scalar control-flow SPIR-V adapter
- oracle-validated dynamic loops end to end for positive steps 1/2/3: mutable
  Typed/Machine loop state, signed-32 `i<n` VTST, I32MAD2 counter update/feed-through and BR back-edges
- structured fragment `if/else` from glslang: two-way `OpPhi` values that feed COLOR0 are
  sunk into branch-local output packs, so real Cg conditionals reach Machine BR/GXP without a physical phi opcode
- all seven public libvita2d Cg shaders now pass through the Typed Vita IR path; generated GXPs are
  byte-identical to the preserved public samples except for Sony's two 32-bit GUID fields
- frontend -> SPIR-V and SPIR-V -> Vita IR -> USSE/GXP boundaries
- independent structured GXP reader for real Vita program images
- canonical GXP serializer for interface data, primary/secondary code, parameter containers,
  oracle-derived 8-byte literal tables and reflection names
- coarse USSE2 major-opcode classifier, verified against known-good public GXP programs
- seven known-good public libvita2d shader/source pairs retained under their upstream MIT license for regression research
- Vita `exports.yml` target skeleton and firmware NID reference manifest
- dependency-free Vita ELF inspection, module import/export parsing, and PT_LOAD extraction tools
- local Cortex-A9/Thumb/VFP/TLS Unicorn oracle runner for a user-supplied original SceShaccCg module,
  with in-memory import traps and direct `CompileProgram` GXP capture
- differential `oracle_diff.py` workflow that runs clean probes against Sony and compares observable GXP metadata/USSE against OpenShaccCg without retaining Sony GXP files
- expanded differential shader corpus generator (ALU, constants, uniforms, control flow, texture, matrix and interface cases)
- external oracle corpus runner protocol
- GXP structural inspector, semantic comparator, USSE-family inspector and raw binary diff tools
- host tests for ABI lifecycle, source compilation failure behavior, direct SPIR-V lowering, public GXP parsing, GXP writer round-trip and USSE family classification

Not implemented yet:

- complete Cg compatibility beyond the glslang HLSL-compatible subset (callback includes, option defines, profile quirks and remaining Cg-only syntax)
- derive Sony-compatible binary/source GUIDs instead of currently emitting zero for newly compiled shaders
- field-level USSE instruction encoding
- remaining exotic GXP auxiliary tables (uniform-buffer/dependent-sampler tables)
- full `SceShaccCg` reflection export surface
- real-Vita validation of GXP produced by the canonical writer

## Build on host

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The external SPIR-V pipeline is optional. When SPIRV-Tools and SPIRV-Cross are
installed on the host, it can be exercised with:

```sh
cmake -S . -B build-spv-pipeline \
  -DOPENSHACCG_ENABLE_SPIRV_TOOLS=ON \
  -DOPENSHACCG_ENABLE_SPIRV_CROSS=ON
cmake --build build-spv-pipeline
ctest --test-dir build-spv-pipeline --output-on-failure
```

SPIRV-Tools runs performance passes while preserving interfaces, bindings and
specialization constants. SPIRV-Cross then parses/reflection-checks the selected
module. The compact Typed Vita IR is now the preferred lowering path when
SPIRV-Cross is enabled; unsupported shapes still fall back to the dependency-free
lowerer while coverage grows. The default Vita static build therefore does not
require host SPIR-V libraries.

The experimental Cg-compatible frontend can be enabled independently or as part
of that full host pipeline:

```sh
cmake -S . -B build-glslang \
  -DOPENSHACCG_ENABLE_GLSLANG=ON \
  -DOPENSHACCG_ENABLE_SPIRV_TOOLS=ON \
  -DOPENSHACCG_ENABLE_SPIRV_CROSS=ON
cmake --build build-glslang
ctest --test-dir build-glslang --output-on-failure
```

It feeds source to glslang's HLSL/DX9-compatible parser with glslang's internal
optimizer disabled; SPIRV-Tools remains the explicit optimization stage. The
current Cg shim only strips a UTF-8 BOM and rewrites the Cg parameter spelling
`TYPE out name` to HLSL `out TYPE name`.

As an end-to-end regression, the seven preserved libvita2d shaders are compiled
from their original Cg source through glslang, SPIRV-Tools, SPIRV-Cross and Typed
Vita IR. Every byte after offset `0x14` matches the public GXP corpus. Offsets
`0x0c..0x13` are the Sony binary/source GUIDs; their generation algorithm is not
yet derived, so open-shacccg currently leaves those fields at zero.

## GXP / USSE workbench

Inspect a known GXP structurally:

```sh
python3 tools/gxp_info.py research/public_samples/libvita2d/clear_f.gxp
```

Compare two programs at the GXP/code level:

```sh
python3 tools/gxp_compare.py \
  research/public_samples/libvita2d/color_v.gxp \
  research/public_samples/libvita2d/texture_v.gxp
```

Classify top-level USSE2 instruction families without pretending to have a full decoder:

```sh
python3 tools/usse_info.py research/public_samples/libvita2d/color_v.gxp
```

The C++ GXP writer is intentionally canonical rather than Sony-byte-identical. It gives the backend a clean target format while keeping hardware acceptance as an explicit test milestone.

## Backend-first workflow

The backend can be exercised independently of Cg parsing:

```c
VscSpirvRequest request = {0};
request.words = spirv_words;
request.word_count = spirv_word_count;
request.entrypoint = "main";
request.stage = VSC_STAGE_VERTEX;

VscCompileResult result = {0};
vsc_compile_spirv(&request, &result);
```

This lets `SPIR-V -> USSE -> GXP` progress before complete Cg compatibility.

## Public regression corpus

`research/public_samples/libvita2d/` contains Cg sources and known-good GXP payloads from the public MIT-licensed `xerpi/libvita2d` repository. Its MIT license and a SHA-256 manifest are retained next to the samples. See `research/PROVENANCE.md` for the clean implementation boundary.

## Oracle research workflow

Generate the current differential corpus:

```sh
python3 tools/oracle/corpus_gen.py oracle_corpus_v2
```

Inspect a privately supplied Vita `libshacccg.elf` and its imports:

```sh
python3 tools/oracle/vita_imports.py /private/libshacccg.elf
```

Run it locally with Unicorn when that Python dependency is available:

```sh
python3 tools/oracle/unicorn_oracle.py /private/libshacccg.elf --probe
```

Or create a local-only import-trapped copy for another ARM emulator:

```sh
python3 tools/oracle/patch_import_stubs.py /private/libshacccg.elf imports.json \
  /private/libshacccg.oracle.elf trap_map.json
```

Never commit or redistribute the Sony input or a patched copy.

## Vita static library

The Vita target is a normal static library rather than a SUPRX. Cross-compile it
with VitaSDK:

```sh
cmake -S . -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DOPENSHACCG_BUILD_VITA_STATIC=ON \
  -DOPENSHACCG_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita
```

This produces `build-vita/libshacccg.a`. The archive contains both the compiler
core and the seven `sceShaccCg*` compatibility entry points. It does not use a
module entry point or `-nostdlib`; the final Vita application link uses the
normal VitaSDK C/C++ runtime (`newlib`, `libstdc++` and `libgcc`). The actual Vita
ABI uses VitaSDK's `psp2/shacccg.h`; host declarations exist only for
differential/unit testing on non-Vita machines.

## Clean implementation rule

Runtime source must remain MIT-compatible. GPL projects may be studied or executed externally to cross-check factual behavior, but GPL source is not copied or linked into the runtime. Unofficial/leaked proprietary PowerVR compiler sources are not used. The proprietary Sony module is a local behavioral oracle only and is never redistributed.

The current corpus-driven backend priorities and evidence-backed USSE coverage are tracked in `research/BACKEND_COVERAGE.md`.

## Next vertical slice

1. Execute the private compiler with the existing ARM oracle harness on a host with Unicorn/QEMU available and collect controlled reference GXP.
2. Use the public MIT corpus plus oracle output to independently derive field-level `VMOV`, `VPCK`, vector arithmetic and control encodings.
3. Implement those encoders and a simple bank-aware virtual-register mapping.
4. Feed the resulting instruction streams through the canonical GXP writer.
5. Validate generated programs on real Vita with `sceGxmProgramCheck` / shader-patcher registration.
6. Grow instruction coverage from the generated differential corpus, then integrate the Cg/HLSL frontend.

## Oracle scope discovered from the supplied module

The supplied module exposes the expected seven `SceShaccCg` functions and imports only 25 functions total: 20 from `SceLibKernel` and 5 from `SceRtcUser`. All but two imported NIDs are currently identified in the harness. See `tools/oracle/ORACLE.md`.

### Current USSE milestone

The USSE layer now includes a validated raw-field VMOV codec. It decodes and
re-encodes all VMOV bit fields (predicate/control bits, raw bank selectors,
data type, swizzle, mask, and four register-number fields) and round-trips a
known-good public Vita instruction exactly. Semantic names for bank/data-type
values remain intentionally unassigned until independently validated.

Host regression suite: 100% passing after the VMOV codec addition.

## USSE2 field-decoder milestone

The backend workbench now has independently tested raw field codecs for
`VMOV`, `VPCK`, `V32NMAD`, the `VMAD` variant of major class `0x03`, `VTST`,
`VBW`, and `KILL`.
Each codec has bit-perfect decode/encode round-trip tests against known-good
Vita GXP machine words. Semantic names for register banks, numeric formats and
swizzle enums remain intentionally unassigned until independently validated.

Mechanical raw bit layouts now use C++17 compile-time descriptors in
`src/usse/raw_encodings.hpp`. `BitField<&Struct::member, offset, width>` plus
`Encoding<mask, expected, ...>` generates matching, field extraction, width
validation and packing without generated source files or a Python build step.
Only genuinely semantic restrictions remain handwritten.

`tools/usse_fields.py <program.gxp>` prints the validated raw fields from a GXP
so new corpus samples can immediately contribute differential evidence.

## USSE2 semantic-emitter milestone

The raw codecs now have a first semantic layer. Register-bank selectors are
represented as context-aware register classes (`Temp`, `PrimaryAttribute`,
`Output`, `SecondaryAttribute`, `Special`, `Immediate`, and indexed banks),
and the validated numeric data-type mapping includes F16/F32. `VPCK` has its
own explicit pack-format namespace.

Two operand-bearing instructions can now be constructed from semantic operands
and reproduce known-good public Vita machine words exactly:

- F32 `VMOV OUTPUT[2] <- PRIMARY_ATTRIBUTE[2]`, XY mask, identity swizzle ->
  `0x38800d2183080080`.
- `VPCK` F32->F16, `PRIMARY_ATTRIBUTE[0]` from PA source registers 0/1,
  identity XYZW selection -> `0x40800d7ea0198002`.

The semantic layer is intentionally narrower than the raw decoder: unsupported
integer VPCK source forms are rejected until their aliased register/component
numbering is validated. This keeps code generation fail-closed instead of
inventing encodings.

### USSE2 semantic arithmetic milestone

The USSE2 backend now has semantic builders for the operand-bearing instruction
families used by the public libvita2d corpus:

- `VMOV`: semantic register banks, F16/F32 type, mask, swizzle and repeat.
- `VPCK`: semantic source/destination banks, pack formats and component selectors.
- `V32NMAD`: semantic F32 vector operation (`mul/add/frc/ddx/ddy/min/max/dot`),
  register banks, modifiers, masks and four-channel swizzles.
- `VMAD`: semantic F32 FMA over unified source + GPI0/GPI1 inputs, with
  register banks, masks, repeat mode and standard vec4 swizzles.

Regression tests reconstruct both public V32NMAD examples and all four matrix
VMAD instructions bit-for-bit. The complete nine-instruction `texture_v`
primary USSE stream is also rebuilt entirely through semantic instruction
builders plus PHAS/EMIT control encoders and compared word-for-word against the
known-good public GXP. The VPCK builder canonicalizes masked-off component
selectors to zero, matching the public compiler output.

`tools/usse_fields.py` was repaired to use the current `gxp_info.parse()` API.

## Full semantic GXP reconstruction milestone

The backend now has a small `backend::ProgramBuilder` that appends only
evidence-backed semantic USSE instructions (`PHAS`, `NOP`, `EMIT`, `VMOV`,
`VPCK`, `V32NMAD`, `VMAD`, `VTST`, `VBW`, and `KILL`). Unsupported forms still
fail instead of guessing instruction words.

`ProgramBuilder` itself is type-driven: all operand-bearing families go through
one `instruction<T>()` template and overload resolution selects the semantic
USSE encoder. Adding another semantic instruction family therefore does not
require adding another builder method.

The backend follows the same compact-description rule above USSE. Machine IR
operands are 4-byte tagged handles and instructions remain 16 bytes; one
`machine_ops.inc` list generates both the opcode enum and operand-role table.
Typed factories such as `physical<MachineType::U32>()` and
`emit<MachineOpcode::Compare>()` keep call sites checked without expanding the
runtime representation. GXP serialization similarly uses reusable member-field
descriptors for scalar, array and packed-nibble wire fields; relative offsets
and the few layout rules that cannot be derived stay explicit.

A compact typed Vita IR now sits above Machine IR with the same storage model:
4-byte typed handles, 16-byte instructions and one `typed_ir_ops.inc` opcode/
operand-role list. The first generic lowering covers U32 input/uniform values,
VBW bitwise operations, comparisons and discard. It allocates TEMP values and
predicate registers from lifetimes instead of exposing physical registers in
the typed IR. A `U32 OR -> compare -> discard` regression exercises the full
Typed IR -> Machine IR -> semantic USSE path and verifies TEMP reuse.

As a full-path regression, `texture_v.gxp` from the MIT-licensed libvita2d
corpus is reconstructed from structured GXP metadata plus semantic USSE
operands. The generated image is byte-for-byte identical to the public sample,
including the v1.4 header, interface block, 9-instruction primary program,
parameter containers, reflection descriptors, and strings. No operand-bearing
instruction qwords are copied into that reconstruction test.

The GXP writer also now follows two zero-count pointer conventions observed
consistently in the public corpus: the uniform-buffer table anchor points at the
parameter-table start, and a zero-length secondary program uses the word just
before the primary instruction stream as its anchor.

## Compact Machine IR milestone

The backend now has a compact machine-facing IR between typed Vita IR and the
semantic USSE builders. Operands are 32-bit tagged handles and every machine
instruction is a generic 16-byte record (`opcode/subop/control + dst/src0/src1`),
so adding instruction families does not require another parallel instruction
class hierarchy. A small opcode descriptor table declares value/predicate
use/def roles and drives validation plus allocation.

Predicate registers are allocated dynamically from virtual lifetimes. Reads and
writes use separate positions inside an instruction, allowing the real USSE
pattern `!p0 CMP -> p0` to reuse the same hardware predicate when the old value
dies at that instruction. The first machine path lowers U32 compare + discard to
`VTST` + `KILL` and reproduces observed instruction words exactly.

Value allocation uses compact 4-byte value descriptors and 8-byte live ranges.
The allocator knows the validated scalar TEMP set, paired float TEMP range,
TEMP124..127 GPI aliases and the public VMAD accumulator slot, with deterministic
low/high preferences and overlap checks.

The same 16-byte instruction record now carries the validated float backend too:
`VMOV`, `VPCK` and `V32NMAD` pack their small semantic controls into `subop` plus
the otherwise-free control bits, while VMAD uses a compact virtual-pair operand
for its two GPI inputs. A zero-word dependent-sample pseudo-op connects the
coordinate GPI lifetime to the asynchronously produced texture TEMP. As a
result, `shader_profiles.cpp` no longer constructs USSE semantic instruction structs or
performs a second lifetime pass. The seven public libvita2d GXP regressions
remain byte-identical.

Typed IR can also lower validated F32x4 arithmetic directly to Machine IR:
multiply, add, subtract-as-negated-add, min/max, dot, negate and absolute. The
SPIRV-Cross adapter recognizes `OpFNegate`, `GLSL.std.450` FAbs/FMin/FMax,
identity/X-splat `OpVectorShuffle`, scalar splats and F32x4 -> F16x4 `OpFConvert`.
The conversion uses a dedicated compact `PackValue` machine opcode so a logical
float4 source contributes both consecutive F32 registers to one validated
F32->F16 VPCK. Other vector widths, shuffle patterns and float conversions still
fail closed.

## Direct Typed/Machine shader lowering

The old stage-specific `VertexIr` / `FragmentIr` bridge has been removed. Both
SPIRV-Cross and the dependency-free SPIR-V recognizer now produce compact
`TypedShader`/`TypedProgram` directly, and all runtime compilation continues as:

`SPIR-V -> Typed IR -> Machine IR -> semantic USSE -> GXP`.

The validated vertex profiles are still deliberately narrow and fail closed:

- `clear_v`: float2 position -> `float4(x,y,1,1)`;
- `texture_v`: float3 position + mat4 + float2 TEXCOORD passthrough;
- `color_v`: float3 position + mat4 + float4 COLOR passthrough.

Machine-profile helpers own the evidence-backed GXP/interface details for those
three forms. They preserve the observed PA/SA counts, matrix GPI staging,
VMAD accumulator convention, varying descriptors, containers and compiler
version fields. Direct regressions require byte-for-byte identity with all three
public vertex GXPs.

The validated fragment profiles are likewise emitted directly from Typed/Machine
IR: uniform color (`clear_f`), varying color (`color_f`), dependent texture
(`texture_f`) and texture multiplied by a float4 tint (`texture_tint_f`). The
dependent texture form intentionally emits no SMP instruction because the
public shader contains none; texture-unit/interface metadata represents that
handoff, and the Machine IR `DependentSample` pseudo-op exists only to model the
value lifetime. The `clear_f` secondary F32->F16 VPCK keeps its observed
overlapping secondary-code layout.

Generic fragment arithmetic also stays in Typed/Machine IR rather than building
a second expression DAG. The current subset includes F32x4 multiply/add/sub,
min/max, dot, negate/absolute and scalar splat. Subtraction uses the validated
negated-add V32NMAD form; unary negate/absolute use V32NMAD source modifiers.
SPIRV-Cross additionally recognizes `GLSL.std.450` FMin/FMax/FAbs, validated
X-splats and F32x4->F16x4 conversion. Unsupported graph shapes, vector widths,
swizzles and conversions continue to fail closed.

The dependency-free recognizer covers the same public vertex/fragment shapes
plus the existing generic arithmetic subset and now feeds `TypedShader`
directly. Diagnostic families remain `0x2210/0x2211` for vertex recognition /
lowering and `0x2220/0x2221` for fragment recognition / lowering.
