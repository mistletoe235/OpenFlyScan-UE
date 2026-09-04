// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Paged/NanoGSPageFormat.h"
#include "Paged/NanoGSTreeFormat.h"

namespace NanoGS::Tree
{
	class NANOGS_API FReader
	{
	public:
		static bool ReadManifest(const FString& Filename, FManifest& OutManifest, FString& OutError);
		static bool ValidatePageMapping(const FManifest& TreeManifest, const NanoGS::Paged::FManifest& PageManifest, FString& OutError);
	};
}
