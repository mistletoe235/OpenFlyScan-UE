# NanoGS source provenance

Reviewed September 23, 2026. The renderer derives from
[TimChen1383/NanoGaussianSplatting](https://github.com/TimChen1383/NanoGaussianSplatting),
which publishes its source under the MIT license. The comparison uses upstream
commit `075e7cee956958b22bce2593b03f01bb3961fd8a`; its original license is retained
in [NanoGaussianSplatting-MIT.txt](../LICENSES/NanoGaussianSplatting-MIT.txt)
and `Plugins/NanoGS/LICENSE`.

## File origins

Of the 102 source/shader files flagged by the earlier header check:

- **49 files** have corresponding paths in that upstream renderer, with
  unchanged files and project-specific modifications. The same Epic copyright
  line is already present in the upstream files; it is not evidence that these
  files were extracted from a separately licensed Engine source checkout.
- **43 files** are paging, LOD and related extensions recorded in the
  earlier CarlaGS development history.
- **10 files** are standalone project extensions first recorded in
  import `02fdbeea30afbfe5038848b9de14bfbec791c3aa`, including budget/GPU-tree
  helpers, tests and the editor mouse-input adapter.

[The file record](NANOGS_SOURCE_PROVENANCE_20260923.json) records paths, hashes,
upstream correspondence and known development commits. Existing copyright
headers are preserved. The upstream MIT terms continue to cover the imported
renderer; the root Apache-2.0 license covers original project work.

Unreal Engine is an external build dependency. The source distribution contains
the project and its plugins, not an Unreal Engine source checkout. Linking UE
APIs or including installed Engine headers does not add those external files to
this repository. See [runtime licensing](LICENSING.md) for the packaged product.
