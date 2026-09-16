# Optimization backlog

This file tracks code-generation work that is intentionally **not** required for
language/backend correctness. The current fidelity reference is Sony
SceShaccCg SDK 3.0.0; older 1.6.5 counts below are explicitly historical.
OpenShaccCg may emit a longer semantically equivalent program where
byte fidelity has not yet been made a target, until a transformation is
independently validated.

Keep these items separate from `BACKEND_COVERAGE.md`: a shader compiling through
Typed IR -> Machine IR -> legal USSE/GXP is a coverage milestone; matching Sony's
instruction count/layout is an optimization milestone.

## Generic lowering first

Do not add whole-shader schedules selected by resource shapes or opcode counts.
The POLY matcher and unpublished POLY3D experiment emitted the same GXP after
changing the live depth factor from `1.998` to `1.25`: their matchers did not
verify constants or operand edges. Both schedules have been removed from runtime
code; their validated ISA encoders remain available for local instruction selection.
POLY/POLY3D now use generic Typed -> Machine lowering again, not fixture detection.

New optimizations must operate on subexpressions with explicit operands and preserve
unmatched code. Tests must include changed constants/operand edges and renamed
identifiers, not only the original golden shader. The fixed16 and smooth paths
now share a calculation-independent output ABI; remaining vitaGL interface
selectors and smaller codegen patterns still need the same mutation audit.
The 24/24 captured result does not prove general compilation or numerical
correctness for nearby source variants.

### Fixed16 and the shared vertex output ABI

- Removed the complete fixed16 schedule and its opcode-count matcher. Changing
  `1/65536` to `1/32768` now changes the literal table rather than being ignored.
- The shared POSITION/COLOR0/TEXCOORD0/PSIZE route accepts unrelated calculations,
  added/removed uniform and matrix resources, and renamed identifiers. It also
  replaces smooth lighting's named-resource guard; unsupported output layouts or
  stores outside the unconditional final block remain fail-closed.
- The first local fixed16 peephole is now active. It recognizes only the isolated
  scalar sub-DAG `S16(high) + U16(low) * (1/65536)` with matching source bits,
  single-use feeders and the exact scale constant. It emits one component stage,
  scaled U16 VPCK, S16 VPCK and VADD; a dynamic or changed scale stays on the
  generic path. The vitaGL case falls from 72 to 48 primary words and from 956 to
  764 bytes without a whole-shader matcher. The now-dead `1/65536` literal still
  remains in metadata and is separate DCE work.
- Generic `TransformMat4` now selects the already oracle-anchored VPCK + repeated
  VMAD pair when the matrix starts at SA0. This is a local matrix-base decision,
  not a fixed16 rule; non-SA0 matrices still use the four-DOT fallback. On the
  fixed16 corpus case this removes two more primary words: 48 -> 46 and
  764 -> 748 bytes.
- Literal binding now consumes the same fixed16 local plan. The `1/65536`
  constant is omitted only when every use is absorbed by canonical unpack folds;
  a live extra use keeps it. This trims the current image to 740 bytes and
  SA=36 without changing USSE, matching Sony's observed secondary-register count.
- Sony SDK 3.0 remains 21 primary + 3 secondary instructions, 568 bytes,
  PA=12/SA=36. Next optimizations are validating non-SA0 matrix VMAD selection,
  cross-component unpack batching/VDUAL and point-size hoisting,
  retaining actual operands and constants.
  Do not restore the complete schedule to recover byte equality.
- Continue numeric validation of mixed integer/F32 register addressing, register
  allocation and TEMP accounting; mutation tests alone are not execution tests.
  `sceGxmProgramCheck` and render validation remain a separate hardware gate.

## Geometrizer integration baselines

### `POLY_VS`

Current integration fixture: `oracle_corpus_v2/vp-geometrizer-poly.cg`.

- Sony 1.6.5 (historical): 14 primary + 4 secondary instructions, 480-byte GXP.
- Sony 3.0.0 reference: 13 primary + 4 secondary instructions, 476-byte v1.5
  GXP, PA=8/SA=8. This is an optimization target, not a runtime template.
- Open after reciprocal hoisting: 19 primary + 2 secondary instructions,
  496-byte GXP. The two secondary words are byte-identical to Sony's first
  reciprocal pair: `0x3080000a80000002`, `0x3080000280000001`.
- Reflection/interface is compatible; Open intentionally uses validated
  VCOMP/V32NMAD/VMOV forms instead of Sony's tighter VMAD2 schedule.

Pending optimizations:

1. ~~Hoist reciprocal computations of screen-size uniforms into the secondary
   program when the denominator is draw-constant.~~ Done for denominator-only
   uniform components; the analysis is use-driven and not shader-name-specific.
2. Fold `x * reciprocal`, scale-by-two and +/-1 projection chains into the
   VMAD2 forms selected by Sony.
3. Canonicalize the literal set/order to Sony's `{depth_scale, -1, 2, 1}`
   profile where doing so reduces code or improves scheduling.
4. Coalesce the four scalar POSITION writes into the compact vector move/pack
   sequence used by Sony.
5. Reproduce Sony NOSCHED/END placement only after the dependency rules are
   independently validated.

### `POLY3D_VS`

Current integration fixture: `oracle_corpus_v2/vp-geometrizer-poly3d.cg`.

- Sony 1.6.5 (historical): 20 primary + 10 secondary instructions, logical size 701 bytes
  (704 bytes on disk), PA=20, SA=14.
- Sony 3.0.0 reference: 18 primary + 11 secondary instructions, 697 logical
  bytes (700 on disk), PA=20/SA=15 and flags `0x00190000`.
- Open generic lowering: 37 primary + 2 secondary instructions, logical size
  765 bytes (768 on disk), PA=20/SA=13. Denominator-only reciprocal hoisting
  previously gave 39+2 / 784 bytes. Generic float composition now groups the
  three `a_pos.xyz` lane writes into one masked VMOV, followed by the `1` write.
  This saves two primary instructions without changing arithmetic or using a
  POLY3D matcher. The generic writer still uses its validated v1.4 metadata.
- Sony uses four literals: `0.0602059935`, `-1`, `1`, `2`; Open currently
  omits the standalone `-1` literal and uses three.
- Attribute/uniform reflection, GXP semantics, PA count and program flag
  `0x00090004` match the oracle baseline.

Pending optimizations:

1. The `1/u_screen.x` / `1/u_screen.y` denominator hoist is complete. Continue
   with the remaining draw-constant projection work in Sony 3.0's 11-word secondary
   evidence set; unlike POLY, Sony relocates one reciprocal result instead of
   overwriting both uniform slots in place.
2. Generic composition is now two masked VMOVs rather than four scalar VMOVs.
   Derive the remaining VPCK/GPI staging optimization as a local transformation.
3. Lower the three `dot(matrix_row, xyz1)` operations through the compact
   VMAD/VDP sequence instead of generic V32NMAD + temporary materialization.
4. Fuse `ax*vz + bx*vx`, `ay*vz + by*vy`, and depth projection into the
   observed VMAD2 forms.
5. Coalesce POSITION output writes and remove dead/intermediate temporaries.
6. Match Sony 3.0's literal canonicalization and SA=15 only as a consequence of
   useful scheduling/selection; do not add dummy literals merely for byte
   similarity.
7. Revisit register allocation after the above fusions; avoid tuning allocator
   heuristics around a single captured shader.

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
