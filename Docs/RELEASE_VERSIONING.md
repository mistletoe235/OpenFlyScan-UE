# Source and simulator versions

The public component name is **OpenFlyScan UE**. The source project filename
`OpenFlySplatUE.uproject`, C++ module identifiers and package directory
`Linux/OpenFlySplatUE` remain stable compatibility identifiers.

## Expo East Linux package

The current HF distribution is `OpenFlyScan-HIL-ExpoEast-Linux-v0.1.1.tar.zst`
under `HIL-simulator/linux/v0.1.1/`. It adds complete dependency notices, Eigen
source availability and the scene license to the existing Expo East runtime.
The executable and scene data are unchanged from v0.1.0; this is a packaging
update, not a new renderer build.

The runtime's original manifest records source base
`66506d2637480aff6324bc7d9d5989bca122f883` plus the English-UI build updates.
The new distribution manifest preserves that binary provenance and records the
separate packaging source commit, archive size and checksum. Do not attribute
the existing binary to a later documentation commit.

## Future releases

Build new runtime versions from a recorded source revision, keep engine binaries
and large scenes outside source Git, and publish the archive with its manifest
and checksum. Use a new version whenever archive contents change; retain the
checksums of older versions.
