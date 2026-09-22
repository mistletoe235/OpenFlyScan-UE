# Source and runtime licensing

Original OpenFlyScan UE code uses Apache-2.0. AirSim, Spark, Eigen and other
components retain the licenses in `THIRD_PARTY_NOTICES.md` and `LICENSES/`.
Unreal Engine itself is licensed separately by Epic; the project license does
not relicense engine code, binaries or restricted assets.

## Runtime package

The recipe builds a game target and invokes cooking without compiling the editor.
`NanoGSEditor` is an Editor module; the runtime modules do not list UnrealEd as a
dependency. The standalone PLY converter uses Python and the pinned Spark helper.
These implementation facts do not replace a review of the actual shipped files
against Epic's Product and Engine Tools distribution provisions.

New packages include license notices and the exact bundled Eigen source archive;
see [source availability](THIRD_PARTY_SOURCES.md). Preserve the archive and
notices when redistributing. Do not silently replace an existing versioned
package with changed content; follow [release versioning](RELEASE_VERSIONING.md).

## Retained Epic headers

The initial standalone import retained Epic copyright headers in multiple source
and shader files. No header is removed or reassigned in this pass. Their recorded
history is summarized in [source provenance](NANOGS_SOURCE_PROVENANCE.md).
Complete that provenance review before declaring the full source tree available
under the project's Apache license or distributing any actual Engine Code.

## Epic notices

Retain Epic notices that accompany the Licensed Technology. Where product credits
are provided, include the notices required by the applicable Unreal Engine EULA:

OpenFlyScan UE uses Unreal® Engine. Unreal® is a trademark or registered trademark
of Epic Games, Inc. in the United States of America and elsewhere.

Unreal® Engine, Copyright 1998 – 2026, Epic Games, Inc. All rights reserved.

See the [official Unreal Engine EULA](https://www.unrealengine.com/en-US/eula/unreal),
particularly its Product distribution, Engine Tools and ownership provisions.
