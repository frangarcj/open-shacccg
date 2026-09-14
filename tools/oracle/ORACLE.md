# Sony oracle harness notes

This directory contains only clean host-side tooling. The proprietary module is never part of this repository.

## Current observations

The examined Vita ELF exports the seven public `SceShaccCg` functions used by vitaShaRK. Its exported library NID is `0xA05BBEBB`.

The module imports only two libraries:

- `SceLibKernel`: 20 functions
- `SceRtcUser`: 5 functions

Most kernel imports are ordinary `sceClib*` operations, two are lightweight-mutex lifecycle calls, one is process-time retrieval, and only two NIDs remain unresolved (`0x120AFC8C` and `0x46E7BE7B`). The five RTC imports are identified.

That is small enough for an ARM CPU oracle without emulating the Vita OS.

## Direct Unicorn runner

`unicorn_oracle.py` maps the module's PT_LOAD regions, creates synthetic heap/stack/trampoline regions, patches import stubs **in emulator memory only**, bridges allocator/source callbacks and can call exported functions.

When Unicorn is available:

```sh
python3 unicorn_oracle.py /private/libshacccg.elf \
  --imports ../../oracle_imports.json --traps ../../oracle_trap_map.json \
  --probe version
```

The runner now initializes the module's Thumb entry points, Cortex-A9 VFP state,
synthetic TPIDRURO/TLS and the lightweight-mutex imports required by the 1.6.5
module. With Unicorn installed it can call `CompileProgram` directly on the
user-provided ELF and write the resulting GXP without modifying the module.

For example, the clean corpus contains two control-flow cases that deliberately
force real BR instructions (a large `if/else` and a dynamic loop):

```sh
python3 unicorn_oracle.py /private/libshacccg.elf \
  --imports ../../oracle_imports.json --traps ../../oracle_trap_map.json \
  --probe compile --source ../../oracle_corpus_v2/fp-if-big.cg \
  --stage fragment --out /tmp/fp-if-big.gxp

python3 unicorn_oracle.py /private/libshacccg.elf \
  --imports ../../oracle_imports.json --traps ../../oracle_trap_map.json \
  --probe compile --source ../../oracle_corpus_v2/fp-loop.cg \
  --stage fragment --out /tmp/fp-loop.gxp
```

Do not commit or redistribute the resulting proprietary-compiler artifacts;
record only independently derived instruction facts/regressions in this repo.

## Trap-patching strategy

Vita import stubs in this module are 16-byte ARM placeholders. For a local CPU emulator the first two ARM instructions can be replaced with:

```text
SVC #trap_id
BX  LR
```

The emulator catches `SVC`, dispatches by `trap_id`, implements the host-side function, writes the ARM return value/registers, and execution returns through `BX LR`.

Generate metadata:

```sh
python3 vita_imports.py /private/libshacccg.elf --json > imports.json
```

Create a *local-only* patched oracle for a separate emulator:

```sh
python3 patch_import_stubs.py /private/libshacccg.elf imports.json \
  /private/libshacccg.oracle.elf trap_map.json
```

Do not commit or redistribute the input or patched ELF.

## SVC handler order

1. Memory/string functions (`memcpy`, `memset`, `memcmp`, `memmove`, etc.).
2. `printf`/`vprintf`/`vsnprintf` logging helpers.
3. Lightweight mutex handlers (a single-threaded oracle can initially model these as successful lock objects).
4. Process-time and RTC calls with deterministic synthetic time.
5. Trace the two unresolved imports and identify them from call behavior.

Once `sceShaccCgInitializeCompileOptions` and `sceShaccCgGetVersionString` execute, move to `sceShaccCgCompileProgram` with the callback bridge and capture GXP bytes.

## Differential runner

`oracle_diff.py` is the normal backend-research entry point now that the local
Unicorn oracle can execute the original compiler. It runs selected clean Cg
probes against Sony, optionally compiles the same source with OpenShaccCg, and
compares observable GXP metadata plus primary/secondary USSE words and coarse
instruction families. Sony GXP bytes are held in memory and temporary storage
only; the repository stores the clean probe sources and derived facts.

Build the OpenShaccCg host compiler with the complete frontend first:

```sh
cmake -S . -B build-full \
  -DOPENSHACCG_ENABLE_GLSLANG=ON \
  -DOPENSHACCG_ENABLE_SPIRV_TOOLS=ON \
  -DOPENSHACCG_ENABLE_SPIRV_CROSS=ON
cmake --build build-full --target openshacccg_compile
```

Then run focused differentials from a Python environment with Unicorn installed:

```sh
export OPENSHACCG_ORACLE_ELF=/private/libshacccg.elf
python3 tools/oracle/oracle_diff.py oracle_corpus_v2 \
  --name 'fp-cmp-*-big' --open-compiler build-full/openshacccg_compile

python3 tools/oracle/oracle_diff.py oracle_corpus_v2 \
  --name fp-loop --report /tmp/fp-loop-report.json
```

Use `--feature control`, repeated `--name` filters and `--limit` for quick
experiments. `--strict-open` makes OpenShaccCg compile failures or any byte
difference outside Sony's two GUID fields fail the command, which is useful for
regressions that are expected to be byte-identical. Without it, backend gaps are
reported but do not make a successful Sony-oracle run fail.

The dynamic-loop probes also anchor one GXP auxiliary-table format. The v1.4
literal table is an array of 8-byte `{uint32 resource_index, uint32 value_bits}`
entries. For `fp-loop`, Sony emits `{0,0}` and `{1,1}` in container 19 at SA2/SA3;
the header's literal-data pointer targets the byte immediately after those
entries. This observable layout is covered by the canonical writer tests; Sony
GXP files themselves remain temporary/local-only.

Multi-input fragment ALU/control probes anchor the interface extension layout as
well: each homogeneous F32 vector input beyond Location 0 adds one 16-byte
descriptor immediately after the main 32-byte interface, then Sony keeps the
normal 8-byte no-secondary anchor before primary code. Two-input and three-input
probes independently confirm the Location-1 (`0x10`) and Location-2 (`0x20`)
records. The width signature is `0x40`/`0x10` for float2 and `0xc0`/`0x30` for
float3/float4.
