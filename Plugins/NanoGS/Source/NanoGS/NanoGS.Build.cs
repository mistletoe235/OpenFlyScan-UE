// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NanoGS : ModuleRules
{
	public NanoGS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				// Access Renderer private headers for FViewInfo::ViewRect (screen percentage support)
				System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
				System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"),
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"Renderer",
				"Projects",
				"Json"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"InputCore",
				"UMG",
				"Slate",
				"SlateCore"
			}
		);

		// The replaceable-scene package stages only RuntimeActive here, then its
		// packaging script copies the one Tree referenced by the runtime contract.
		// Normal builds retain the legacy sidecars so existing package entry points
		// keep working.
		bool bRuntimeSceneOnly = System.Environment.GetEnvironmentVariable(
			"NANOGS_RUNTIME_SCENE_ONLY") == "1";
		if (!bRuntimeSceneOnly)
		{
			// External NanoGS containers are range-read at runtime. Keep them loose and
			// seekable beside packaged Content rather than in Pak/IoStore blocks.
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/SJTU_SpatialLOD/...*.ngsp",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/SJTU_SpatialLOD/nodes.bin",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/SJTU_SpatialLOD/node_lod_ranges.bin",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/SJTU_SpatialLOD/node_lod_errors.bin",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/SJTU_SpatialLOD/...manifest.json",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/TreeV2/...*.ngst",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/TreeV2/...*.ngsp",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/TreeV2/...manifest.json",
			StagedFileType.NonUFS
			);
			RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/TreeV2/...tree_v3_*.bin",
			StagedFileType.NonUFS
			);
		}
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/RuntimeActive/...*.json",
			StagedFileType.NonUFS
		);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/RuntimeActive/...*.ngst",
			StagedFileType.NonUFS
		);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/RuntimeActive/...*.ngsp",
			StagedFileType.NonUFS
		);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Content/NanoGSData/RuntimeActive/...tree_v3_*.bin",
			StagedFileType.NonUFS
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
