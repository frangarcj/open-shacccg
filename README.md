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
- dependency-free SPIR-V reader with entry-point/stage validation and a first vertex lowering subset
- frontend -> SPIR-V and SPIR-V -> Vita IR -> USSE/GXP boundaries
- independent structured GXP reader for real Vita program images
- canonical GXP serializer for interface data, primary/secondary code, parameter containers and reflection names
- coarse USSE2 major-opcode classifier, verified against known-good public GXP programs
- seven known-good public libvita2d shader/source pairs retained under their upstream MIT license for regression research
- Vita `exports.yml` target skeleton and firmware NID reference manifest
- dependency-free Vita ELF inspection, module import/export parsing, and PT_LOAD extraction tools
- local oracle import-stub patcher and Unicorn runner that convert the module's 25 imports into ARM `SVC` traps
- expanded differential shader corpus generator (ALU, constants, uniforms, control flow, texture, matrix and interface cases)
- external oracle corpus runner protocol
- GXP structural inspector, semantic comparator, USSE-family inspector and raw binary diff tools
- host tests for ABI lifecycle, source compilation failure behavior, direct SPIR-V lowering, public GXP parsing, GXP writer round-trip and USSE family classification

Not implemented yet:

- pinned Cg/HLSL frontend adapter
- broaden SPIR-V instruction selection beyond the validated vertex subset and add fragment lowering
- bank-aware USSE register allocation
- field-level USSE instruction encoding
- exotic GXP auxiliary tables (literal/uniform-buffer/dependent-sampler tables)
- full `SceShaccCg` reflection export surface
- real-Vita validation of GXP produced by the canonical writer
- execution of the private Sony oracle in this development environment (no ARM emulator dependency is installed here)

## Build on host

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

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

The mechanical raw bit layouts are declared in `tools/usse_encodings.py` as
64-bit patterns. Fixed `0`/`1` tokens generate matcher masks/expected values,
`x` marks don't-care bits, and `name:width` declares a field. Running
`python3 tools/gen_usse_raw.py` regenerates `src/usse/usse_raw.generated.inc`;
host `ctest` checks that the generated file is not stale.

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
`VTST` + `KILL` and reproduces observed instruction words exactly. Physical
value allocation remains deliberately fail-closed until the typed value
allocator is introduced.

## Vita IR lowering milestone

A first stage-specific backend IR now sits above the semantic USSE assembler.
`backend::VertexIr` describes attributes, a `mat4` uniform and high-level
operations (`TransformPosition`, `CopyVarying`) without exposing USSE words or
register-bank selectors to the caller. The initial lowering is deliberately
strict and fail-closed while its allocation rules are being validated.

For the public libvita2d `texture_v` shape, the IR lowering now performs the
first deterministic Vita register assignment, stages GPI inputs, selects the
validated `VMOV`/`VPCK`/`VMAD` sequence, builds reflection/container metadata,
and serializes the final GXP. A regression describes only the shader-level IR
(two attributes, one matrix, transform-position and copy-varying operations)
and requires the resulting 344-byte GXP to be byte-for-byte identical to the
known-good `texture_v.gxp` sample.

The matrix lowering also captures an observed paired-output convention: the
final VMAD reuses the Z-lane GPI0 source while GPI1 swizzling supplies the Z/W
pair. This convention is regression-backed rather than inferred from a generic
matrix model.

## Vertex IR generalization milestone

The strict Vita vertex IR lowering now reproduces three public MIT libvita2d
vertex shaders byte-for-byte through one backend path:

- `texture_v`: float3 position + float2 texcoord + mat4 transform
- `color_v`: float3 position + float4 color + mat4 transform
- `clear_v`: float2 position expanded to `float4(x, y, 1, 1)` without uniforms

Evidence-backed lowering rules added in this milestone:

- logical attribute width is distinct from GXP reflection width; these public
  vertex attributes are reflected as 4 components even for Cg float2/float3,
  while USSE repeat/staging behavior follows the logical width;
- a float4 passthrough varying uses VMOV repeat count 1, while float2 uses 0;
- COLOR and TEXCOORD varyings use distinct 32-byte interface descriptors;
- the no-uniform clear path uses PHAS/NOP/V32NMAD/VMOV/VMOV/EMIT, with the
  homogeneous constant sourced from SPECIAL[1];
- clear_v uses PA=4, SA=2, one container (19), compiler-version field 0,
  whereas the matrix path uses PA=8, SA=18, containers 14+19 and compiler 16.

All three IR-generated binaries are regression-tested by exact byte comparison
against the preserved public libvita2d GXP corpus. Unsupported graph shapes
continue to fail closed.


## SPIR-V to Vita IR milestone

`vsc_compile_spirv()` now lowers a real, deliberately small SPIR-V vertex subset
instead of stopping after structural validation. The dependency-free recognizer
tracks `OpName`, `Location` and `BuiltIn Position` decorations, F32/vector/matrix/
pointer types, global Input/Output/Uniform variables, `OpConstant 1.0`, `OpLoad`,
`OpCompositeConstruct`, `OpMatrixTimesVector`, and `OpStore` inside the selected
entry function.

Three validated graph shapes lower into the existing `backend::VertexIr`:

- float2 position -> `float4(x,y,1,1)` (`clear_v` shape);
- float3 position + mat4 + float2 texture-coordinate passthrough (`texture_v`);
- float3 position + mat4 + float4 color passthrough (`color_v`).

The first allocator maps input `Location N` to the validated PA resource convention
`N*4`; varying kind is accepted only when its name gives an evidence-backed COLOR
or TEXCOORD/UV interpretation. Unsupported variables, graph shapes, multiple
uniforms/varyings, or unknown semantics fail closed with diagnostic `0x2210`.
Fragment SPIR-V currently fails explicitly with `0x2202`.

Regression tests build standards-shaped SPIR-V modules for all three vertex cases,
compile them through the public `vsc_compile_spirv()` API, and require byte-for-byte
identity with the GXP produced by the equivalent direct Vita IR. The older IR tests
continue to require byte-for-byte identity against the public MIT libvita2d samples,
so the chain is now covered as `SPIR-V -> Vita IR -> semantic USSE -> GXP`.

## First fragment SPIR-V milestone

The fragment backend is no longer a blanket `0x2202` placeholder. A first
fail-closed fragment IR and SPIR-V recognizer now support the validated
libvita2d `clear_f` shape: one F32 `float4` uniform is loaded and written
directly to fragment color output Location 0.

This path lowers as `SPIR-V -> FragmentIr -> semantic USSE2 -> GXP`. The
fragment IR emits a primary `PHAS + VMOV` stream and a one-word secondary
`VPCK` stream. The secondary pack is represented as a real semantic
instruction, not copied opaque bytes. Its validated settings include F32->F16,
PA sources/destination, XYZW selectors, END set and NOSCHED clear.

The GXP writer now also models the fragment-specific secondary-code layout
observed in `clear_f`: the single secondary qword lives at interface-record
offset `+0x14` (`0xac` in that sample), while the primary stream begins at
`0xb8`. This overlapping layout is deliberately limited to one secondary
fragment instruction until more public samples establish a broader rule.

A direct `FragmentIr` regression reproduces the complete 244-byte public MIT
`clear_f.gxp` sample byte-for-byte, including header, embedded secondary code,
primary code, containers, reflection metadata and names. A separate SPIR-V
regression compiles the same shader graph through `vsc_compile_spirv()` and
requires exact identity with direct fragment IR output. Unsupported fragment
SPIR-V graphs fail closed with diagnostic `0x2220`; fragment IR failures use
`0x2221`.

### Fragment varying + texture milestone

The validated fragment subset now includes three end-to-end shapes:

- `float4 uniform -> COLOR` (`clear_f`)
- `COLOR varying -> COLOR` (`color_f`)
- `tex2D(sampler2D, TEXCOORD0) -> COLOR` (`texture_f`)

`color_f` and `texture_f` are independently regenerated byte-for-byte from
`FragmentIr` and the MIT libvita2d public samples.  The texture path records an
important SGX/Vita convention: this simple dependent texture sample has no SMP
instruction in the primary USSE stream (PHAS is the only primary instruction).
The sample is represented by texture-unit/interface metadata, including an
8-byte post-interface dependent-sampler record and sampler reflection semantic
2.  The GXP writer models that layout explicitly.

The dependency-free SPIR-V subset recognizes direct varying color stores and
`OpImageSampleImplicitLod` from one combined sampled-image variable plus a
`Location 0` float2 coordinate input. Unsupported texture graphs still fail
closed.

### Fragment texture-tint milestone

The validated fragment subset now includes `texture(sampled_image, texcoord) * float4_uniform`.
`texture_tint_f.gxp` is reconstructed byte-for-byte from `FragmentIr::TextureTint2D`, using semantic
PHAS/NOP/VPCK/V32NMAD/VPCK emission and the observed dependent-sampler metadata. The SPIR-V subset
recognizes `OpImageSampleImplicitLod` followed by `OpFMul` with a Location-0 float2 coordinate and a
single float4 uniform, then lowers through the same IR path. Unsupported variants still fail closed.

## Generic SPIR-V arithmetic DAG milestone

The fragment SPIR-V lowerer now has a generic expression-DAG fallback in addition to the seven canonical vita2d shape paths. The canonical paths are intentionally preserved so their byte-identical public-corpus regressions do not change.

The initial DAG supports float4 Location 0 varying leaves, multiple float4 uniform leaves, `OpFMul`, `OpFAdd`, `OpDot`, and scalar splats represented by `OpCompositeConstruct`. Binary arithmetic nodes are assigned temporary USSE registers deterministically and lowered through semantic V32NMAD operations. A multiply followed by add therefore works as a general MAD expression today, but is deliberately emitted as separate MUL + ADD instructions; it is not fused to VMAD until GPI allocation/scheduling rules are independently validated.

New non-corpus regressions compile `vColor * uScale + uBias` and `dot(vColor, uWeights).xxxx` through the public SPIR-V API and compare the result against direct DAG IR lowering. This proves the fallback is not another sample-name/shape matcher. These generic arithmetic GXPs are structurally generated and regression-tested on host, but unlike the seven public vita2d samples they do not yet have byte-identical Sony outputs or real-Vita `sceGxmProgramCheck` validation. Unsupported graphs still fail closed.


### Generic arithmetic DAG checkpoint

The generic fragment fallback now has liveness-based TEMP reuse instead of
monotonically consuming temporary register pairs. DAG use counts pin values
until their last consumer and then recycle TEMP pairs deterministically.

The arithmetic IR also includes `Sub`, `Neg`, `Min`, `Max`, and `Abs` nodes.
The SPIR-V subset currently recognizes standard `OpFSub` and `OpFNegate`
directly; `Min`/`Max`/`Abs` are available at the IR/USSE lowering boundary
pending GLSL.std.450 extended-instruction parsing. Subtraction lowers through
V32NMAD ADD with a negated source, while unary negate/absolute use the same
validated V32NMAD source-modifier encoding and an immediate-zero add.

A new non-corpus regression compiles `vColor - (-uBias)` from SPIR-V through
the generic DAG. Existing seven vita2d canonical paths remain unchanged and
continue to use their byte-identical regressions.
