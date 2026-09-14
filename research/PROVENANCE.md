# Research provenance policy

`open-shacccg` runtime code is intended to remain MIT-compatible.

## Permitted runtime inputs

- code written specifically for this project;
- files with an explicit permissive license compatible with MIT;
- public VitaSDK ABI declarations/NID data where their license permits reuse;
- independently recorded facts such as binary field offsets, instruction
  behavior, and differential test observations.

## Reference/test only

GPL projects may be read to cross-check factual behavior or run as separate
host-side tools, but GPL source is not copied into or linked with the runtime.
The implementation in `src/` is independently expressed.

## Prohibited inputs

- leaked or unofficial proprietary PowerVR DDK/compiler source;
- copied/decompiled Sony compiler implementation code;
- redistribution of the privately supplied Sony `libshacccg` ELF.

The private Sony module may be executed locally as a behavioral oracle. Corpus
outputs record observable compiler results, not Sony implementation code.
