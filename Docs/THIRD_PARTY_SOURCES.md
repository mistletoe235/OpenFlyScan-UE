# Source availability in the Linux package

The packaged runtime includes `THIRD_PARTY_SOURCES/eigen3.tar.gz`, containing the
exact Eigen headers copied from this project's AirLib dependency when the package
was assembled. Extract the archive to obtain that source, including its original
copyright and license notices. The MPL-2.0 text is provided in
[LICENSES/Mozilla-Public-License-2.0.txt](../LICENSES/Mozilla-Public-License-2.0.txt).

The source repository also carries these headers at
`Plugins/AirSim/Source/AirLib/deps/eigen3`. The archive is a source distribution,
not a separately licensed binary or an assertion that all project code is MPL.

Other bundled third-party license notices are in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The package's `SHA256SUMS`
records its delivered files. This packaging change applies to newly assembled
packages; it does not update previously uploaded archives.
