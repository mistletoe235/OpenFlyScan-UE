# Source and simulator versions

The public component name is **OpenFlyScan UE**. The source project filename
`OpenFlySplatUE.uproject`, C++ module identifiers and package directory
`Linux/OpenFlySplatUE` remain stable compatibility identifiers.

## Expo East Linux package

The HF distribution remains `OpenFlyScan-HIL-ExpoEast-Linux-v0.1.0.tar.zst`
under `HIL-simulator/linux/v0.1.0/`. Its archive, executable, scene and checksum
are unchanged. Updated license notices and the Eigen source archive are provided
alongside the existing package in HF; no replacement runtime download is needed.

The runtime's original manifest records source base
`66506d2637480aff6324bc7d9d5989bca122f883` plus the English-UI build updates.
That binary provenance is retained. A later source or documentation commit does
not become the source revision of the existing executable.

## Future releases

Use a new version whenever runtime archive contents change, and publish its
manifest and checksum together. Documentation and license attachments can be
updated separately without changing a versioned runtime archive.
