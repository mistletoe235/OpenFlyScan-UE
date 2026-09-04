#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "DjiHilState.h"

class IDjiHilStateProvider
{
public:
    virtual ~IDjiHilStateProvider() = default;
    virtual void Reset() = 0;
    virtual bool GetLatestState(msr::airlib::djihil::State& OutState) const = 0;
    virtual const TCHAR* GetBackendName() const = 0;
};

class FDjiHilReplayStateProvider final : public IDjiHilStateProvider
{
public:
    bool LoadJsonLines(const FString& ReplayPath, FString& OutError);
    virtual void Reset() override;
    bool AdvanceToSourceTime(uint64 TargetSourceTimeNs);
    virtual bool GetLatestState(msr::airlib::djihil::State& OutState) const override;
    virtual const TCHAR* GetBackendName() const override { return TEXT("replay"); }
    bool SampleRenderState(uint64 TargetSourceTimeNs, msr::airlib::djihil::State& OutState) const;
    bool GetSourceTimeRange(uint64& OutFirstSourceTimeNs, uint64& OutLastSourceTimeNs) const;
    int32 GetFrameCount() const;

private:
    static bool ParseStateLine(const FString& Line, int32 LineNumber,
        msr::airlib::djihil::State& OutState, FString& OutError);

    mutable FCriticalSection Mutex;
    TArray<msr::airlib::djihil::State> Frames;
    int32 LatestFrameIndex = INDEX_NONE;
};
