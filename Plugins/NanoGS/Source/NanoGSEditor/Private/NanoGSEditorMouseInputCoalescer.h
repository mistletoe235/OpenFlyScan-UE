// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Input/Events.h"

/**
 * Coalesces high-frequency raw mouse events during RMB level-viewport
 * navigation.  FSceneViewport already applies accumulated deltas once per
 * frame; this preprocessor additionally avoids routing hundreds of equivalent
 * Slate events before that accumulation point.
 */
class FNanoGSEditorMouseInputCoalescer final : public IInputProcessor
{
public:
	virtual void Tick(
		float DeltaTime,
		FSlateApplication& SlateApp,
		TSharedRef<ICursor> Cursor) override;
	virtual bool HandleMouseMoveEvent(
		FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(
		FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(
		FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("NanoGSEditorMouseInputCoalescer");
	}

private:
	bool IsLevelViewportNavigation(const FPointerEvent& MouseEvent) const;
	void Flush(FSlateApplication& SlateApp);
	void Reset();

	FVector2D AccumulatedDelta = FVector2D::ZeroVector;
	FVector2D ScreenPosition = FVector2D::ZeroVector;
	FVector2D LastScreenPosition = FVector2D::ZeroVector;
	TSet<FKey> PressedButtons;
	FModifierKeysState ModifierKeys;
	uint32 UserIndex = 0;
	uint32 PointerIndex = 0;
	uint32 PendingSamples = 0;
	bool bDispatchingCoalescedEvent = false;
};
