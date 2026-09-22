# NanoGS source provenance review — 2026-09-21

The standalone project began with snapshot commit
`02fdbeea30afbfe5038848b9de14bfbec791c3aa` on September 4, 2026.
It is not the earliest history of the renderer.

A read-only comparison against the earlier CarlaGS development repository found
102 current source/shader files containing an Epic copyright header:

- 50 paths already occur in the earliest CarlaGS snapshot,
  `b685b0b381ef798432e21e28689b9f126e551c0e`.
- 42 paths first appear in subsequent development commits: 32 in `2404e66`,
  seven in `ae33ddd`, and three in `3bbf33b`.
- Ten current paths have no same-path addition in that legacy history; renamed
  project files need separate tracing.

The complete per-file record is in `NANOGS_SOURCE_PROVENANCE_20260921.json`.
The listed commits belong to the legacy repository, not this repository's Git
history. Later local additions with the same header are evidence that templates
may have propagated it, but do not by themselves establish copyright ownership
or exclude copied code. Likewise, the header alone does not establish that a
whole file was copied from Epic.

No copyright header was removed. The remaining step is to confirm the origins of
the initial imported renderer and the renamed files, distinguishing original
implementation, Epic Examples, Engine Code and any other upstream material.
Record applicable notices and distribution conditions for each category rather
than applying the root license to everything.
