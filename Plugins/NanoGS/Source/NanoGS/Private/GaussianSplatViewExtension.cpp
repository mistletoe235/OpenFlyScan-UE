// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatViewExtension.h"
#include "GaussianSplatSceneProxy.h"
#include "Paged/NanoGSCaptureReadiness.h"
#include "RenderGraphBuilder.h"

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Instance = nullptr;

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	Instance = this;
}

FGaussianSplatViewExtension::~FGaussianSplatViewExtension()
{
	if (Instance == this)
	{
		Instance = nullptr;
	}
}

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Get()
{
	return Instance;
}

void FGaussianSplatViewExtension::GetRegisteredProxies(TArray<FGaussianSplatSceneProxy*>& OutProxies) const
{
	FScopeLock Lock(&ProxyLock);
	OutProxies = RegisteredProxies;
}

void FGaussianSplatViewExtension::BuildPagedCaptureReadinessSnapshot(
	const FSceneInterface* Scene,
	NanoGS::Paged::FCaptureReadinessSnapshot& OutSnapshot) const
{
	FScopeLock Lock(&ProxyLock);
	OutSnapshot.Sources.Reset();
	for (const FGaussianSplatSceneProxy* Proxy : RegisteredProxies)
	{
		if (Proxy == nullptr || &Proxy->GetScene() != Scene)
		{
			continue;
		}

		NanoGS::Paged::FCaptureSourceSnapshot Source;
		if (Proxy->GetPagedCaptureSourceSnapshot(Source))
		{
			OutSnapshot.Sources.Add(MoveTemp(Source));
		}
	}
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FScopeLock Lock(&ProxyLock);
	return RegisteredProxies.Num() > 0;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::SetupView(
	FSceneViewFamily& InViewFamily,
	FSceneView& InView)
{
	NanoGS::Paged::BindCaptureDrawTicketView_GameThread(InViewFamily, InView);
}

void FGaussianSplatViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	NanoGS::Paged::ArmCaptureDrawTicketForViewFamily_GameThread(InViewFamily);
}

void FGaussianSplatViewExtension::RegisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.AddUnique(Proxy);
	}
}

void FGaussianSplatViewExtension::UnregisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.Remove(Proxy);
	}
}

void FGaussianSplatViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// Rendering is handled by PostOpaqueRenderDelegate in FGaussianSplattingModule
	// This hook is too early in the pipeline (before lighting) for Gaussian splats
}

void FGaussianSplatViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
}
