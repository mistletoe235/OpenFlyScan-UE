# OpenFlySplat UE

OpenFlySplat UE is a standalone Unreal Engine 5.5 project for Gaussian-splat
rendering and AirSim-based flight/HIL experiments on Linux. It does not depend
on CARLA and does not include a scene PLY, generated NanoGS pages, or a default
environment.

## What is in this repository

- A compilable UE 5.5 C++ project and the NanoGS renderer source.
- The AirSim runtime code and the flight/camera assets needed by the simulator.
- One 7 KB, data-free runtime map. It contains no point cloud or environment.
- Two PLY workflows: direct editor import and scalable paged Tree conversion.
- Reproducible scripts for the Linux editor build and downloadable package.

The repository is the source distribution. A Linux packaged build is produced
from the same revision with `Scripts/package_linux.sh` and should be uploaded as
a Release artifact; it is deliberately not committed to Git.

## Requirements

- Linux x86-64.
- An Unreal Engine **5.5 source build**. NanoGS integrates with renderer-private
  headers, so an Epic Launcher binary is not sufficient.
- Vulkan SM6 and a current GPU driver.
- Python 3 and NumPy for scalable conversion.
- Git and Rust 1.82+ only when building the pinned Spark helper.

Set the engine location and build:

```bash
export OPENFLYSPLAT_UE_ROOT=/path/to/UnrealEngine-5.5
./Scripts/build_editor.sh -MaxParallelActions=8
```

Open the editor with:

```bash
"$OPENFLYSPLAT_UE_ROOT/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/OpenFlySplatUE.uproject"
```

## Import a PLY in the editor

For a small or medium model, use Unreal's normal UI:

1. Open the Content Browser and click **Import**.
2. Select a binary little-endian 3D Gaussian Splatting `.ply` file.
3. Choose the SH bands in the NanoGS import dialog.
4. Drag the resulting Gaussian Splat asset into the runtime map.

This path is implemented by the NanoGS UE asset factory and does not invoke a
script. It keeps the complete model in one UE asset, so it is intended for
interactive preview rather than multi-gigabyte scenes.

For a large model, first build the pinned Spark helper and then run the scalable
import. It builds a content-addressed paged Tree, imports only its lightweight
descriptor into UE, and binds it to the runtime map:

```bash
./Scripts/bootstrap_spark_builder.sh
./Scripts/import_scene.sh /path/to/model.ply
```

Neither workflow copies the source PLY into Git. Generated data is written to
ignored `Saved/NanoGSCache` and `Content/NanoGSData` directories.

## Build the downloadable Linux package

The packaged build is also data-free. It contains the executable, required
AirSim runtime assets, and a converter that lets the recipient install their own
PLY beside the package:

```bash
./Scripts/bootstrap_spark_builder.sh
./Scripts/package_linux.sh /path/to/empty/output
```

After extracting the Release artifact, the recipient runs:

```bash
./convert_nanogs_ply.sh /path/to/model.ply --crop-mode none
./run_nanogs_profile.sh project-default
```

For Android HIL:

```bash
./run_openfly_hil.sh --settings NanoGSConverter/Config/OpenFlyHil.json
```

The default HIL profile uses UDP ports 30020/30021 for control and telemetry,
TCP port 30022 for JPEG frames, 1440x1080 JPEG quality 92, and a 30 FPS producer
limit. Change the bundled JSON for another host or port allocation.

The retained upstream AirSim `OpticalFlow`/`OpticalFlowVis` post-process
materials are not compatible with UE 5.5 Vulkan SM6 and fall back to the
default material. They are not used by the supported NanoGS RGB/HIL path.

## Content policy

Project `Content/` contains only the empty runtime map. A generated Tree source
asset may appear there after an editor import but is ignored and must not be
published. `Plugins/AirSim/Content/` is retained because it supplies the drone,
camera, HUD, sensor materials, and collision effects used at runtime; AirSim's
car, weather, demo-map, and weather-menu content and all CARLA environments are
excluded. Packaging starts from the required AirSim Blueprints and follows only
their referenced dependencies. The hidden collision floor uses UE's built-in
`/Engine/BasicShapes/Cube`, which the package recipe explicitly cooks.

Run the source-distribution guard before publishing:

```bash
./Scripts/verify_release_tree.sh
```

See [Docs/OPEN_SOURCE_DISTRIBUTION_CN.md](Docs/OPEN_SOURCE_DISTRIBUTION_CN.md)
for the Chinese release notes, [Docs/HIL_PERFORMANCE_CN.md](Docs/HIL_PERFORMANCE_CN.md)
for the final measured HIL snapshot, and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled dependencies.

## License

Original OpenFlySplat work is released under the
[Apache License 2.0](LICENSE). Bundled third-party code and assets remain under
the licenses listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
