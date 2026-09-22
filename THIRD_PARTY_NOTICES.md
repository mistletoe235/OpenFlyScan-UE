# Third-party notices

The repository-wide Apache License 2.0 applies only to original OpenFlyScan
work. Bundled third-party components remain under their own licenses.

- **AirSim / AirLib / MavLinkCom and AirSim runtime assets**: MIT. The retained
  AirSim/Colosseum license text is in
  `LICENSES/AirSim-Colosseum-MIT.txt`.
- **rpclib**: MIT. The retained license text is in `LICENSES/rpclib-MIT.md`.
- **msgpack-c headers bundled by rpclib**: Boost Software License 1.0. Copyright
  notices are retained in the source headers; the license text is in
  `LICENSES/Boost-1.0.txt`.
- **Eigen 3 headers bundled by AirLib**: Mozilla Public License 2.0, with files
  explicitly identified as MPL-2.0 in their headers. The full text is in
  `LICENSES/Mozilla-Public-License-2.0.txt`. New runtime packages carry the exact
  bundled headers in `THIRD_PARTY_SOURCES/eigen3.tar.gz`, with extraction and
  source-availability information in `THIRD_PARTY_SOURCES/README.md`.
- **Spark `build-lod`**: MIT. It is built from the pinned upstream revision and
  patched at release-build time; its source is not vendored here. The license
  text is in `LICENSES/Spark-MIT.txt`; its revision is recorded in
  `Tools/NanoGSTreeBuilder/spark_builder.lock.json`.
- **Unreal Engine**: installed and licensed separately from original project code.
  Source and packaged distributions must respect Epic's license. Retained Epic
  copyright headers have not been reassigned; see `Docs/NANOGS_SOURCE_PROVENANCE.md`
  for the remaining source-origin review and `Docs/LICENSING.md` for package scope.

This inventory is technical release documentation, not legal advice. Verify
third-party versions and asset provenance again before a public release.
