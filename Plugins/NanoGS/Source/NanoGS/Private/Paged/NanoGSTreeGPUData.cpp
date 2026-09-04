// Copyright Epic Games, Inc. All Rights Reserved.

#include "Paged/NanoGSTreeGPUData.h"

namespace NanoGS::Tree
{
bool BuildGPUNodeTable(const FManifest& Manifest, TArray<FGPUNodeRecord>& OutNodes, FString& OutError)
{
	OutNodes.Reset();
	OutError.Reset();
	if (Manifest.Nodes.IsEmpty() || Manifest.Header.NodeCount != static_cast<uint64>(Manifest.Nodes.Num()) ||
		Manifest.Header.RootNode >= Manifest.Header.NodeCount || Manifest.Header.PagePoints > 65536u)
	{
		OutError = TEXT("Tree manifest is not valid for GPU node packing");
		return false;
	}

	OutNodes.SetNumUninitialized(Manifest.Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Manifest.Nodes.Num(); ++NodeIndex)
	{
		const FNode& Source = Manifest.Nodes[NodeIndex];
		FGPUNodeRecord& Target = OutNodes[NodeIndex];
		Target.CenterAndSupportRadius = FVector4f(Source.CenterCm, Source.SupportRadiusCm);
		Target.FirstChild = Source.FirstChild;
		Target.ChildCountAndFlags = static_cast<uint32>(Source.ChildCount);
		if (Source.IsInternal())
			Target.ChildCountAndFlags |= GPUNodeFlagInternal;
		if (Source.IsExactLeaf())
			Target.ChildCountAndFlags |= GPUNodeFlagExactLeaf;
		FMemory::Memcpy(&Target.FeatureRadiusBits, &Source.FeatureRadiusCm, sizeof(Target.FeatureRadiusBits));
		if (Source.PageId > 0xffffu || Source.PageLocal > 0xffffu)
		{
			OutNodes.Reset();
			OutError = FString::Printf(TEXT("Tree node %d exceeds packed GPU page addressing"), NodeIndex);
			return false;
		}
		Target.PackedPageAddress = Source.PageId << 16u | Source.PageLocal;
	}
	return true;
}
}
