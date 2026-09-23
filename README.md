# OpenFlyScan UE

Part of [OpenFlyScan](https://github.com/mistletoe235/OpenFlyScan) ·
[Paper](https://arxiv.org/abs/2609.24253) ·
[Citation](https://github.com/mistletoe235/OpenFlyScan#citation)

[Project home](https://github.com/mistletoe235/OpenFlyScan) · [Source repository](https://github.com/mistletoe235/OpenFlyScan-UE) · [Download simulator](https://huggingface.co/datasets/mistletoe235/openflyscan/tree/main/HIL-simulator)

OpenFlyScan UE is a standalone Unreal Engine 5.5 project for Gaussian-splat
rendering and AirSim-based flight/HIL experiments on Linux. It does not depend
on CARLA and does not include a scene PLY, generated NanoGS pages, or CARLA
environment assets. A separate **Expo East demo package** includes a converted
scene and opens without a PLY conversion step; scene data stays outside this Git
repository.

This is the simulator component of OpenFlyScan. Source stays in this repository;
scene-inclusive binaries and GS assets belong in the OpenFlyScan HF dataset.
Existing project filenames, C++ module names and `OPENFLYSPLAT_UE_ROOT` remain
unchanged for build/package compatibility. See [release versioning](Docs/RELEASE_VERSIONING.md).

## Start here

- **Try Expo East:** extract the scene-inclusive Linux package, open its
  `Linux/OpenFlySplatUE` directory, and run `./run_expo_east.sh`. This opens the
  scene with the HIL monitor, waiting for a phone connection. No phone or aircraft
  is required just to open the scene. Use `--preview` for software-only SimpleFlight.
  The HIL and renderer panels use English labels.
- **Connect DJI hardware:** follow [Android HIL setup](Docs/HIL_QUICKSTART.md)
  ([Chinese guide](Docs/HIL_QUICKSTART_CN.md)), then run `./run_openfly_hil.sh` in the same
  directory, or connect to the HIL session already opened by `run_expo_east.sh`.
  Close any software-only preview before starting HIL.
- **Use your own scene:** run the packaged PLY converter described below.
- **Develop the renderer:** build this source project with UE 5.5.

The downloadable runtime needs Linux x86-64, Vulkan SM6 and a compatible
GPU/driver; it does **not** require an installed UE editor. Python/NumPy are needed
only when converting another PLY. The Expo East demo uses an RTX 4090 as its
reference system; its minimum GPU memory requirement has not been established.

## What is in this repository

- A compilable UE 5.5 C++ project and the NanoGS renderer source.
- The AirSim runtime code and the flight/camera assets needed by the simulator.
- One data-free runtime map with engine-native sky, sunlight, and skylight. It
  contains no point cloud or external environment assets.
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
script. It keeps the complete model resident in one UE asset. Very large inputs
may automatically reduce SH order to remain below UE's 2 GiB per-array limit;
the default 10-million-splat frame budget also prevents a single RHI working
buffer from exceeding UE's 4 GiB limit. Use the paged workflow below when full
SH order, bounded residency, or spatial LOD is required.

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

For a separate scene-inclusive release, see
[Expo East demo assembly](Docs/EXPO_EAST_DEMO.md).

After extracting the Release artifact, the recipient runs:

```bash
./convert_nanogs_ply.sh /path/to/model.ply --crop-mode none
./run_nanogs_profile.sh project-default
```

For Android HIL:

```bash
./run_openfly_hil.sh --settings NanoGSConverter/Config/OpenFlyHil.json
```

The default HIL profile receives Android pose/session messages on UE UDP 30020,
replies to Android UDP 30021, and connects to the Android TCP 30022 listener to
send JPEG frames. It uses 1440x1080 JPEG quality 92 with a 30 FPS producer limit.
The phone supports hotspot discovery or an explicit UE LAN address. Full device
setup, UI steps, port directions, and troubleshooting are in the
[HIL quickstart](Docs/HIL_QUICKSTART.md). No DJI hardware is needed for the
separate Expo East software-only preview.

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

Original OpenFlyScan work is released under the
[Apache License 2.0](LICENSE). Bundled third-party code and assets remain under
the licenses listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

See [source/runtime licensing](Docs/LICENSING.md) for the separate Unreal Engine
terms and the retained-header provenance review. New Linux packages include
[Eigen source and license notices](Docs/THIRD_PARTY_SOURCES.md).
