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
python3 unicorn_oracle.py /private/libshacccg.elf --probe
```

The current development container does not have Unicorn or QEMU installed, so this runner is syntax/build checked here but ARM execution must be exercised on another host.

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
