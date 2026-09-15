# Optimization backlog

This file tracks code-generation work that is intentionally **not** required for
language/backend correctness.  The Sony SceShaccCg 1.6.5 oracle is the reference
for observable instruction selection and scheduling, but OpenShaccCg may emit a
longer semantically equivalent program until a transformation is independently
validated.

Keep these items separate from `BACKEND_COVERAGE.md`: a shader compiling through
Typed IR -> Machine IR -> legal USSE/GXP is a coverage milestone; matching Sony's
instruction count/layout is an optimization milestone.

## Geometrizer integration baselines

### `POLY_VS`

Current integration fixture: `oracle_corpus_v2/vp-geometrizer-poly.cg`.

- Sony: 14 primary + 4 secondary instructions, 480-byte GXP.
- Open: 21 primary + 0 secondary instructions, 496-byte GXP at the current
  generic-vertex baseline.
- Reflection/interface is compatible; Open intentionally uses validated
  VCOMP/V32NMAD/VMOV forms instead of Sony's tighter VMAD2 schedule.

Pending optimizations:

1. Hoist reciprocal computations of screen-size uniforms into the secondary
   program when the denominator is draw-constant.
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

- Sony: 20 primary + 10 secondary instructions, logical size 701 bytes
  (704 bytes on disk), PA=20, SA=14.
- Open baseline after generic-resource support: 43 primary + 0 secondary
  instructions, logical size 797 bytes (800 bytes on disk), PA=20, SA=13.
- Sony uses four literals: `0.0602059935`, `-1`, `1`, `2`; Open currently
  omits the standalone `-1` literal and uses three.
- Attribute/uniform reflection, GXP semantics, PA count and program flag
  `0x00090004` match the oracle baseline.

Pending optimizations:

1. Hoist both `1/u_screen.x` and `1/u_screen.y` plus draw-constant projection
   work into secondary code.  Sony's 10-word secondary program is the target
   evidence set.
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

## General USSE optimization work

### High value

- **Secondary-program hoisting.** Move uniform-only reciprocal/arithmetic out of
  primary code when oracle evidence proves the value is phase-safe.
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
