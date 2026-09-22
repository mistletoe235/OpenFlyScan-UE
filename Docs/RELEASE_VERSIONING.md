# Source and simulator versions

The public component name is **OpenFlyScan UE**. The source project filename
`OpenFlySplatUE.uproject`, C++ module identifiers, package directory
`Linux/OpenFlySplatUE` and build variable `OPENFLYSPLAT_UE_ROOT` are compatibility
identifiers, not separate products. Keep them stable when updating documentation.

## Existing Expo East package

The prepared Linux package is named
`OpenFlyScan-HIL-ExpoEast-Linux-v0.1.0.tar.zst` in the private OpenFlyScan HF dataset,
under `HIL-simulator/linux/v0.1.0/`. Expo East's historical asset alias is
`expo_north`. Its default launch opens the English HIL monitor;
`./run_expo_east.sh --preview` selects the software-only preview.

The package's manifest records source base commit
`66506d2637480aff6324bc7d9d5989bca122f883` **plus uncommitted updates**, including
the English UI build. That base commit alone does not reproduce the released
binary. Do not rewrite the existing manifest to claim otherwise. Public naming
changes in the current source have not rebuilt that package.

## Before a public release

1. Finish reviewing and commit the source, including HIL examples and documents.
2. Record the exact commit, dependency locks and any remaining dirty-tree patch.
3. Build/package from that source; keep generated scenes and engine binaries out
   of the source repository.
4. Publish the archive, checksum and manifest together in the HF dataset.
5. Link that dataset path from the source release and the main repository.

Use a new release version if rebuilding changes an already distributed archive.
Do not silently replace a versioned archive while retaining its old checksum.
