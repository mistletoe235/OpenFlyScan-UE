// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSSpatialLODFormat.h"

namespace NanoGS::SpatialLOD
{
	class NANOGS_API FReader
	{
	public:
		static bool ReadManifest(
			const FString& RootDirectory,
			FManifest& OutManifest,
			FString& OutError);

		static bool ReadRuntimeManifest(
			const FString& RootDirectory,
			FManifest& OutManifest,
			FString& OutError);

		static bool ReadNodeLodPayload(
			const FManifest& Manifest,
			uint32 NodeIndex,
			uint32 Level,
			TArray64<uint8>& OutBytes,
			FString& OutError);
	};
}
