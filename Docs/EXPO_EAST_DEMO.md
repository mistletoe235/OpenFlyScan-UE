# Expo East scene-inclusive package

The source repository stays data-free. The demo is a separate Linux runtime
distribution with a converted Expo East scene, uploaded to the
[OpenFlyScan HF dataset](https://huggingface.co/datasets/IPEC-COMMUNITY/openflyscan).
Public package access is currently blocked by the HF organization storage quota.
The scene uses CC BY 4.0; runtime components retain their own licenses.

## Assemble from the scene-free runtime

1. Copy the complete scene-free `Linux` package to a new directory. Do not
   overwrite the original runtime or a working experiment installation.
2. In `Linux/OpenFlySplatUE`, convert the Expo East PLY with
   `./convert_nanogs_ply.sh /path/to/expo_east.ply --crop-mode none --spawn-height 0.5`.
   Historical experiment paths use `expo_north` for this same scene.
3. Copy `Examples/ExpoEast/run_expo_east.sh` beside the runtime launchers and
   `Examples/ExpoEast/ExpoEastPreview.json` into `NanoGSConverter/Config/`.
   Copy the HIL guides from `Docs/` into the package's `Docs/` directory.
4. Run `./run_expo_east.sh --preview` and inspect the actual ground-level start and
   third-person camera. This is a software-only preview, not an HIL test.
5. Record any reviewed start-position adjustment in the release manifest.
   Store the final runtime scene descriptor and a screenshot with the release.
6. Archive the complete `Linux` directory, excluding `Saved`, intermediate
   `NanoGSConverterCache`, and obsolete package checksum lists. Keep all
   `Content/NanoGSData/RuntimeActive` resources and the sibling `Engine` directory.

The package contains the converted scene, not the source PLY. A source PLY can
be distributed separately without duplicating it inside every executable
download. Track its identity with the source hash already produced by the
converter. Keep the runtime package's provenance separate from the current
source commit: adding documentation does not rebuild an existing binary.

## First-run design

- `run_expo_east.sh`: visible desktop window with the HIL monitor, waiting for
  phone connection; no automatic takeoff or aircraft-side simulator start.
- `run_expo_east.sh --preview`: software-only SimpleFlight, stationary start.
- `run_openfly_hil.sh`: the same scene/origin with the DJI HIL backend; requires
  the Android companion and the setup described in `HIL_QUICKSTART.md`.
- The native GS color settings remain unchanged; do not brighten the scene as
  part of packaging.
- The converter checks sampled body/camera clearance. Inspect the rendered
  result as well; the generated floor is not a building collision mesh.
