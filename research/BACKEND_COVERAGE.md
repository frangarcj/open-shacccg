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

## Declarative raw codec generation

Raw field packing is now generated from a project-owned compact encoding spec
instead of being repeated by hand in `src/usse/usse.cpp`.

- `tools/usse_encodings.py` is the source of truth for mechanical bit layouts.
- Patterns are exactly 64 bits from MSB to LSB.
- binary `0`/`1` tokens are fixed opcode bits and produce `mask`/`expected`;
- `x` tokens are ignored/don't-care bits and therefore do not enter the mask;
- `field_name:width` tokens generate extraction, width validation and packing;
- optional field constraints cover encodings such as the multi-major VBW family;
- `tools/gen_usse_raw.py --check` verifies that the checked-in generated file is current.

The generated output is `src/usse/usse_raw.generated.inc`. Semantic meaning is
deliberately not generated: register-bank mappings, compare operations,
immediate representation, predicates and fail-closed restrictions remain in
handwritten project code. This mirrors the useful separation in compact ISA
decoders such as Dynarmic (declarative match layout versus semantic visitor)
without importing Vita3K's GPL implementation.

## Next backend order

1. **Typed Vita IR.** Replace the float4-only assumption in the generic fragment DAG with explicit scalar/vector types. Add U32 values and integer leaves without exposing physical USSE registers to the frontend, then lower them into the compact Machine IR.
2. **Virtual value allocation.** Extend the descriptor-driven Machine IR allocator from predicates to typed value lifetimes/banks, keeping physical register choices out of Vita IR.
3. **Predicate values and control statements.** Route Vita IR compares and `discard` through the now-tested Machine IR VTST/KILL path instead of emitting semantic USSE directly.
4. **BR control flow.** Add raw and semantic BR only once branch offset/direction semantics are anchored by real words. Then lower structured `if/else`; loops come after branch back-edges are independently validated.
5. **Integer data movement/conversion.** Cover the VMOV/VPCK integer forms and bitcasts required to connect U32 computations to actual shader resources.
6. **Texture expansion.** Move beyond the validated dependent-sampler texture shape: SMP, integer texture results, gather and multiple samplers.
7. **Common missing ALU families.** Prioritize VCOMP, VMAD2 and VDUAL based on real traces, then remaining instruction families by corpus frequency.
8. **Resource/reflection generalization.** Derive register counts, parameter types, containers, uniform buffers, literals and dependent samplers from IR instead of current sample-shaped layouts.
9. **Hardware gate.** Treat a capability as complete only after host regressions plus real-Vita `sceGxmProgramCheck`/render validation where possible.

## Definition of progress

A new family is not considered implemented merely because decode/encode round-trips. The preferred progression is:

1. capture or locate a legal real instruction;
2. decode its fields independently;
3. add raw bit-exact round-trip tests;
4. add a semantic builder that reconstructs the real word;
5. expose it through `ProgramBuilder`;
6. lower a typed Vita IR operation through it;
7. place the resulting stream in a generated GXP;
8. validate on hardware.
