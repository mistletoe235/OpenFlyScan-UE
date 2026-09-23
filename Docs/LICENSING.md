# Source and runtime licensing

Original OpenFlyScan UE work uses Apache-2.0. The imported NanoGaussianSplatting
renderer uses MIT; AirSim, Spark, Eigen and other dependencies retain the terms
listed in [third-party notices](../THIRD_PARTY_NOTICES.md). Unreal Engine is
installed and licensed separately under Epic's EULA.

## Source distribution

This repository contains project/plugin source, not an Unreal Engine checkout.
The NanoGS upstream license and file correspondence are recorded in
[source provenance](NANOGS_SOURCE_PROVENANCE.md). Original upstream headers are
preserved; the root project license does not replace their MIT terms.

## Linux runtime

The downloadable Expo East package is a cooked game target. It does not include
the Unreal Editor or the project's `NanoGSEditor` module. UE runtime binaries
remain subject to Epic's terms; the project's source license does not relicense
them. The standalone PLY converter uses Python and the Spark helper.

Dependency notices and the exact Eigen headers are provided in the source
repository and alongside the existing HF package; see [source availability](THIRD_PARTY_SOURCES.md).
The Expo East scene has its own CC BY 4.0 license in the HF release. Neither
that scene license nor Apache-2.0 is a blanket license for the compiled runtime.

## Epic attribution

OpenFlyScan UE uses Unreal® Engine. Unreal® is a trademark or registered trademark
of Epic Games, Inc. in the United States of America and elsewhere.

Unreal® Engine, Copyright 1998 – 2026, Epic Games, Inc. All rights reserved.

[Unreal Engine EULA](https://www.unrealengine.com/en-US/eula/unreal)
