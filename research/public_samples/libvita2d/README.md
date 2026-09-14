# libvita2d public shader samples

These files are a host-side regression/research corpus for `open-shacccg`.
They originate from the public `xerpi/libvita2d` repository, which is licensed
under the MIT License. The upstream license text is preserved as
`LICENSE.libvita2d`.

Upstream repository:

- https://github.com/xerpi/libvita2d

For each shader we keep the Cg source and the raw GXP program image used by the
tests. The raw `.gxp` data is the shader program payload from the corresponding
upstream compiled object. It is used only as known-good input for the independent
GXP reader and differential tooling; runtime compiler code does not link or
embed these samples.

The Sony `libshacccg` module supplied privately for oracle research is **not**
part of this directory or repository and must never be redistributed.
