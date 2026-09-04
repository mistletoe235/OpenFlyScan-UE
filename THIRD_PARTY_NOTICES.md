# Third-party notices

The repository-wide Apache License 2.0 applies only to original OpenFlySplat
work. Bundled third-party components remain under their own licenses.

- **AirSim / AirLib / MavLinkCom and AirSim runtime assets**: MIT. The retained
  retained AirSim/Colosseum license text is in
  `LICENSES/AirSim-Colosseum-MIT.txt`.
- **rpclib**: MIT. The retained license text is in `LICENSES/rpclib-MIT.md`.
- **msgpack-c headers bundled by rpclib**: Boost Software License 1.0. Copyright
  notices are retained in the source headers; the license text is in
  `LICENSES/Boost-1.0.txt`.
- **Eigen 3 headers bundled by AirLib**: Mozilla Public License 2.0, with files
  explicitly identified as MPL-2.0 in their headers. See
  <https://www.mozilla.org/MPL/2.0/>.
- **Spark `build-lod`**: MIT. It is built from the pinned upstream revision and
  patched at release-build time; its source is not vendored here. The license
  text is in `LICENSES/Spark-MIT.txt`; its revision is recorded in
  `Tools/NanoGSTreeBuilder/spark_builder.lock.json`.
- **Unreal Engine**: not distributed in the source repository. A packaged build
  is subject to Epic's Unreal Engine license and redistribution terms.

This inventory is technical release documentation, not legal advice. Verify
third-party versions and asset provenance again before a public release.
