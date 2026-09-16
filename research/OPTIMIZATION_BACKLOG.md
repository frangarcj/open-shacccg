# Optimization backlog

This file tracks code-generation work that is intentionally **not** required for
language/backend correctness. The current vitaGL fidelity reference is Sony
SceShaccCg SDK 3.0.0; older Geometrizer baselines below were captured with the
1.6.5 oracle and remain historical optimization targets until they are explicitly
recaptured. OpenShaccCg may emit a longer semantically equivalent program where
byte fidelity has not yet been made a target, until a transformation is
independently validated.

Keep these items separate from `BACKEND_COVERAGE.md`: a shader compiling through
Typed IR -> Machine IR -> legal USSE/GXP is a coverage milestone; matching Sony's
instruction count/layout is an optimization milestone.

## Geometrizer integration baselines

### `POLY_VS`

Current integration fixture: `oracle_corpus_v2/vp-geometrizer-poly.cg`.

- Recaptured against SDK 3.0.0: Sony emits a 476-byte v1.5 image with 13 primary
  + 4 secondary instructions, PA=8/SA=8, four literal slots and compiler version
  `0x00033a90`.
- Open now reproduces that image byte-for-byte. A structural Typed matcher
  recognizes the two screen-space divisions, clamp + Log2 depth path and direct
  COLOR passthrough, then selects the semantic SDK 3.0 schedule instead of the
  former 19+2 generic expansion.
- The schedule uses the three observed VMAD2 fusions and secondary reciprocal /
  projection setup directly. Its only new VMAD encoding is the fail-closed
  VMAD3 GPI0 extended selector 5 (`x10`) paired with the already validated
  GPI1=`000` selector.
- The old 1.6.5 counts above are superseded for this fixture; no code-generation
  optimization debt remains for captured `POLY_VS`.

### `POLY3D_VS`

Current integration fixture: `oracle_corpus_v2/vp-geometrizer-poly3d.cg`.

- Sony: 20 primary + 10 secondary instructions, logical size 701 bytes
  (704 bytes on disk), PA=20, SA=14.
- Open after denominator-only reciprocal hoisting: 39 primary + 2 secondary
  instructions, logical size 781 bytes (784 bytes on disk), PA=20, SA=13.
  This reduces total USSE words from 43 to 41 and moves both screen-size
  reciprocals out of primary code.
- Sony uses four literals: `0.0602059935`, `-1`, `1`, `2`; Open currently
  omits the standalone `-1` literal and uses three.
- Attribute/uniform reflection, GXP semantics, PA count and program flag
  `0x00090004` match the oracle baseline.

Pending optimizations:

1. The `1/u_screen.x` / `1/u_screen.y` denominator hoist is complete. Continue
   with the remaining draw-constant projection work in Sony's 10-word secondary
   evidence set; unlike POLY, Sony relocates one reciprocal result instead of
   overwriting both uniform slots in place.
2. Replace the generic four-VMOV materialization of `float4(a_pos.xyz, 1)` with
   the VPCK/GPI staging used by Sony.
3. Lower the three `dot(matrix_row, xyz1)` operations through the compact
   VMAD/VDP sequence instead of generic V32NMAD + temporary materialization.
4. Fuse `ax*vz + bx*vx`, `ay*vz + by*vy`, and depth projection into the
   observed VMAD2 forms.
5. Coalesce POSITION output writes and remove dead/intermediate temporaries.
6. Match Sony's literal canonicalization and SA=14 only as a consequence of
   useful scheduling/selection; do not add dummy literals merely for byte
   similarity.
7. Revisit register allocation after the above fusions; avoid tuning allocator
   heuristics around the current deliberately verbose 43-word program.

### `CMP_FS`

Current integration fixture: `oracle_corpus_v2/fp-geometrizer-cmp.cg`.

- The structural profile `texture2D + (uniform > 0.5 ? 1 : sample.a)` is already
  byte-identical to Sony outside GUIDs: 7 primary + 1 secondary instructions,
  320-byte GXP on disk.
- The stream is reconstructed from semantic VTST/VMOV/VPCK/VBW builders; no raw
  opaque instruction word is injected by the shader profile.
- There is therefore no instruction-selection optimization debt for this shape.

Related texture cleanup:

1. The standalone `Texture2D` profile now follows the newer SDK 3.0.0 oracle:
   GXP v1.5, flags `0x00180801`, SA=0, no data/container padding, and the
   16-entry sampler-query table (`0x0302` for a direct read). The older public
   libvita2d GXP remains a historical v1.4 fixture rather than the codegen target.
2. `TextureTint2D` and its `SPRITECOORD` variant now also follow SDK 3.0.0
   byte-for-byte: sampler query `0x0301`, SA=4, one uniform container, and the
   VPCK/VPCK/V16NMAD multiply-pack stream selected by the newer compiler.
3. The alpha-test texture/tint profile now follows SDK 3.0.0 as well. The
   control stream is unchanged except for Sony's opaque KILL payload `0x306`;
   v1.5 metadata adds the `0x0301` sampler-query table and compiler version
   `0x00033a90`.
4. Generalize predicated scalar/component selection only when another real
   shader needs it; do not replace the exact CMP shape with branch-heavy generic
   select lowering.

### vitaGL fog fidelity

- Linear fog now reproduces SDK 3.0.0 byte-for-byte: GXP v1.5, 11 primary +
  3 secondary instructions, PA=8/SA=7, one `1.0f` literal, WPOS additional-input
  metadata, and the compact VCOMP/VMAD2/VMAD mix sequence.
- Exponential-squared fog now reproduces SDK 3.0.0 byte-for-byte as a separate
  structural profile: 12 primary + 3 secondary instructions, PA=8/SA=9, two
  literals (`1.0f` and `LOG2E^2`), and the observed VMAD3 + VDUAL + base-selector
  Exp2 sequence feeding the same clamp/mix tail as linear fog.
- The sRGB output conversion now reproduces SDK 3.0.0 byte-for-byte as a
  20-instruction v1.5 profile. The remaining sRGB work is generalization only:
  preserve this exact path while accepting equivalent front-end DAG orderings
  if a second real shader requires them.

### `TM2_FAST_FS`

Current integration fixture: `oracle_corpus_v2/fp-geometrizer-tm2-fast.cg`.

- Sony: 27 primary instructions, 560-byte GXP, PA=4, SA=9, two primary phases.
- Open correctness baseline: 31 primary instructions, 572-byte GXP, PA=4, SA=7,
  one primary phase.
- Both use the same two scalar uniforms plus sampler0 resource shape. Open lowers
  `floor` as `x-frac(x)`, `fmod` as `x-floor(x/y)*y`, boolean `&&` into validated
  BR control flow, and the final `float4(sample.rgb,1)` through generic scalar
  composition + VPCK.

Pending optimizations:

1. Derive the VMAD2 form Sony uses for `sample.a*255 + 0.5` and other fused
   meta arithmetic instead of the current multi-V32 sequence.
2. Canonicalize `floor`/`fmod` to Sony's shorter V16/V32 selection where it is
   independently validated; keep the generic algebraic lowering as fallback.
3. Fold the two discard conditions into Sony's VTST/BR/KILL schedule and derive
   the second-PHAS requirement rather than emitting the current generic CFG.
4. Coalesce the final sampled RGB + constant-one alpha into the observed
   V32NMAD + VPCK pair instead of four scalar VMOV materialization steps.
5. Revisit literal-table canonicalization after fusion: Sony uses seven SA
   literal words (including `1/170`), while the generic baseline needs five.
6. Match PA/SA temp/phase metadata only as a consequence of those transformations;
   do not pad resources merely for byte similarity.

## General USSE optimization work

### High value

- **Secondary-program hoisting.** Denominator-only scalar reciprocals are now
  hoisted generically: POLY 21+0 -> 19+2 and POLY3D 43+0 -> 39+2. Continue with
  uniform-only add/mul/VMAD2 chains proven phase-safe by the Sony streams.
- **VMAD2 selection.** Derive semantic VMAD2 forms used by real Geometrizer
  projection shaders instead of keeping them as opaque one-off words.
- **VMAD/VDP dot selection.** Prefer GPI staging + vector dot/FMA when it removes
  scalar composition temporaries.
- **V16NMAD selection.** Sony frequently chooses V16 forms for final combine and
  half/output work; Open currently prefers already-validated V32 forms.
- **Output coalescing.** Recognize scalar component writes feeding one POSITION or
  COLOR and emit vector masks/repeats where legal.

### Medium value

- **Literal canonicalization.** Deduplicate and order literal-table entries in the
  same useful form as Sony, without adding unused constants just for byte identity.
- **Scheduling flags.** Derive NOSCHED, sync-start and END placement from actual
  data dependencies, then compare against Sony.
- **Register-pressure cleanup.** Re-run allocation after instruction fusion and
  remove temporary spans created only by generic FloatCompose lowering.
- **Half precision preservation.** The current glslang HLSL path often promotes
  `half` to F32 SPIR-V; preserve F16 only when the frontend can prove it.

### Later / evidence driven

- VDUAL pairing and other dual-issue selection.
- Exact Sony instruction ordering where multiple schedules are semantically
  equivalent.
- Byte-identical GXP layout for complex shaders beyond GUIDs, after codegen and
  reflection are stable.

## Rule for closing an optimization

An optimization is complete only when:

1. a minimal differential oracle probe isolates the relevant field/selection;
2. raw/semantic encoding has a bit-exact regression where new hardware fields
   are involved;
3. Machine/Typed lowering uses the optimized form without a shader-name special
   case;
4. the real integration shader still passes host/cross/Linux CI; and
5. the Sony/Open differential demonstrates the intended reduction or match.
