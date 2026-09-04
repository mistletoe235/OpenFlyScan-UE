// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Misc/EngineVersionComparison.h"
#include "RHICommandList.h"
#include "RHIResources.h"

namespace NanoGS::RHICompat
{
	inline FBufferRHIRef CreateBuffer(
		FRHICommandListBase& RHICmdList,
		const TCHAR* DebugName,
		uint32 Size,
		uint32 Stride,
		EBufferUsageFlags Usage,
		ERHIAccess InitialState)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		FRHIResourceCreateInfo CreateInfo(DebugName);
		return RHICmdList.CreateBuffer(Size, Usage, Stride, InitialState, CreateInfo);
#else
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			DebugName,
			Size,
			Stride,
			Usage)
			.SetInitialState(InitialState);
		return RHICmdList.CreateBuffer(Desc);
#endif
	}
}
