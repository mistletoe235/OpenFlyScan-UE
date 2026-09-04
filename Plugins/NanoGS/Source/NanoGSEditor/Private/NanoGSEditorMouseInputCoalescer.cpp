// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGSEditorMouseInputCoalescer.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"

namespace
{
	TAutoConsoleVariable<int32> CVarNanoGSEditorCoalesceMouseInput(
		TEXT("nanogs.Editor.CoalesceMouseInput"),
#if PLATFORM_LINUX
		1,
#else
		0,
#endif
		TEXT("Coalesce RMB level-viewport mouse moves to one accumulated Slate event per frame.\n")
		TEXT("0: disabled, 1: enabled (Linux editor default)."),
		ECVF_Default);
}

bool FNanoGSEditorMouseInputCoalescer::IsLevelViewportNavigation(
	const FPointerEvent& MouseEvent) const
{
	if (CVarNanoGSEditorCoalesceMouseInput.GetValueOnGameThread() == 0 ||
		MouseEvent.IsTouchEvent() ||
		!MouseEvent.IsMouseButtonDown(EKeys::RightMouseButton) ||
		GEditor == nullptr)
	{
		return false;
	}

	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	if (ActiveViewport == nullptr)
	{
		return false;
	}

	FViewportClient* ActiveClient = ActiveViewport->GetClient();
	for (FLevelEditorViewportClient* LevelClient : GEditor->GetLevelViewportClients())
	{
		if (LevelClient == ActiveClient)
		{
			return true;
		}
	}
	return false;
}

bool FNanoGSEditorMouseInputCoalescer::HandleMouseMoveEvent(
	FSlateApplication& SlateApp,
	const FPointerEvent& MouseEvent)
{
	if (bDispatchingCoalescedEvent || !IsLevelViewportNavigation(MouseEvent))
	{
		return false;
	}

	AccumulatedDelta += FVector2D(MouseEvent.GetCursorDelta());
	ScreenPosition = FVector2D(MouseEvent.GetScreenSpacePosition());
	LastScreenPosition = FVector2D(MouseEvent.GetLastScreenSpacePosition());
	PressedButtons = MouseEvent.GetPressedButtons();
	ModifierKeys = MouseEvent.GetModifierKeys();
	UserIndex = MouseEvent.GetUserIndex();
	PointerIndex = MouseEvent.GetPointerIndex();
	++PendingSamples;
	return true;
}

bool FNanoGSEditorMouseInputCoalescer::HandleMouseButtonDownEvent(
	FSlateApplication& SlateApp,
	const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		Reset();
	}
	return false;
}

bool FNanoGSEditorMouseInputCoalescer::HandleMouseButtonUpEvent(
	FSlateApplication& SlateApp,
	const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		// Preserve the final movement accumulated before RMB release.
		Flush(SlateApp);
	}
	return false;
}

void FNanoGSEditorMouseInputCoalescer::Tick(
	float DeltaTime,
	FSlateApplication& SlateApp,
	TSharedRef<ICursor> Cursor)
{
	Flush(SlateApp);
}

void FNanoGSEditorMouseInputCoalescer::Flush(FSlateApplication& SlateApp)
{
	if (PendingSamples == 0)
	{
		return;
	}

	const FPointerEvent CoalescedEvent(
		UserIndex,
		PointerIndex,
		ScreenPosition,
		LastScreenPosition,
		AccumulatedDelta,
		PressedButtons,
		ModifierKeys);

	TGuardValue<bool> DispatchGuard(bDispatchingCoalescedEvent, true);
	SlateApp.ProcessMouseMoveEvent(CoalescedEvent, false);
	Reset();
}

void FNanoGSEditorMouseInputCoalescer::Reset()
{
	AccumulatedDelta = FVector2D::ZeroVector;
	PressedButtons.Reset();
	PendingSamples = 0;
}
