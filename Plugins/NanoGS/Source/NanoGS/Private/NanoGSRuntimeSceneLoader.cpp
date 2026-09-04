#include "NanoGSRuntimeSceneLoader.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Paged/NanoGSTreeSourceAsset.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const FName ActiveSceneTag(TEXT("NanoGS.ActiveScene"));
	const FName PlayerStartTag(TEXT("NanoGS.PlayerStart"));
	const FName CollisionFloorTag(TEXT("NanoGS.CollisionFloor"));
	const TCHAR DefaultRuntimeScene[] = TEXT("Content/NanoGSData/RuntimeActive/active_scene.runtime.json");

	bool ReadObject(
		const TSharedPtr<FJsonObject>& Parent,
		const TCHAR* Field,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (!Parent.IsValid() || !Parent->TryGetObjectField(Field, ObjectValue) ||
			ObjectValue == nullptr || !ObjectValue->IsValid())
		{
			OutError = FString::Printf(TEXT("missing JSON object: %s"), Field);
			return false;
		}
		OutObject = *ObjectValue;
		return true;
	}

	bool ReadVector(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FVector& OutValue,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || Values == nullptr || Values->Num() != 3)
		{
			OutError = FString::Printf(TEXT("%s must contain three numbers"), Field);
			return false;
		}
		OutValue = FVector(
			(*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		if (OutValue.ContainsNaN())
		{
			OutError = FString::Printf(TEXT("%s contains a non-finite value"), Field);
			return false;
		}
		return true;
	}

	bool ReadRotator(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		FRotator& OutValue,
		FString& OutError)
	{
		FVector Values;
		if (!ReadVector(Object, Field, Values, OutError))
			return false;
		OutValue = FRotator(Values.X, Values.Y, Values.Z);
		return true;
	}

	template <typename ActorType>
	ActorType* FindTaggedOrUnique(UWorld* World, const FName Tag)
	{
		ActorType* Unique = nullptr;
		int32 Count = 0;
		for (TActorIterator<ActorType> It(World); It; ++It)
		{
			ActorType* Actor = *It;
			if (Actor->ActorHasTag(Tag))
				return Actor;
			Unique = Actor;
			++Count;
		}
		return Count == 1 ? Unique : nullptr;
	}

	FString ResolveRuntimeConfig()
	{
		FString ConfigPath;
		if (!FParse::Value(FCommandLine::Get(), TEXT("NanoGSRuntimeScene="), ConfigPath))
			ConfigPath = DefaultRuntimeScene;
		const FString FullPath = FPaths::IsRelative(ConfigPath)
			? FPaths::Combine(FPaths::ProjectDir(), ConfigPath)
			: ConfigPath;
		return FPaths::ConvertRelativePathToFull(FullPath);
	}

	bool ApplyScene(UWorld* World, const TSharedPtr<FJsonObject>& Root, FString& OutError)
	{
		FString Schema;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("schema"), Schema) ||
			Schema != TEXT("nanogs.runtime_scene.v1"))
		{
			OutError = TEXT("unsupported runtime scene schema");
			return false;
		}

		FString TreeDirectory;
		if (!Root->TryGetStringField(TEXT("tree_directory"), TreeDirectory))
		{
			OutError = TEXT("missing tree_directory");
			return false;
		}

		TSharedPtr<FJsonObject> Transform;
		TSharedPtr<FJsonObject> ComponentSettings;
		TSharedPtr<FJsonObject> PlayerSettings;
		TSharedPtr<FJsonObject> FloorSettings;
		if (!ReadObject(Root, TEXT("transform"), Transform, OutError) ||
			!ReadObject(Root, TEXT("component"), ComponentSettings, OutError) ||
			!ReadObject(Root, TEXT("player_start"), PlayerSettings, OutError) ||
			!ReadObject(Root, TEXT("collision_floor"), FloorSettings, OutError))
		{
			return false;
		}

		AGaussianSplatActor* GaussianActor =
			FindTaggedOrUnique<AGaussianSplatActor>(World, ActiveSceneTag);
		if (GaussianActor == nullptr)
			GaussianActor = World->SpawnActor<AGaussianSplatActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (GaussianActor == nullptr || GaussianActor->GaussianSplatComponent == nullptr)
		{
			OutError = TEXT("could not create runtime GaussianSplatActor");
			return false;
		}

		FVector Location;
		FVector Scale;
		FRotator Rotation;
		if (!ReadVector(Transform, TEXT("location"), Location, OutError) ||
			!ReadRotator(Transform, TEXT("rotation"), Rotation, OutError) ||
			!ReadVector(Transform, TEXT("scale"), Scale, OutError))
		{
			return false;
		}
		GaussianActor->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
		GaussianActor->SetActorScale3D(Scale);
		GaussianActor->Tags.AddUnique(ActiveSceneTag);

		UNanoGSTreeSourceAsset* RuntimeSource =
			NewObject<UNanoGSTreeSourceAsset>(GaussianActor->GaussianSplatComponent, NAME_None, RF_Transient);
		if (!RuntimeSource->InitializeFromDirectory(TreeDirectory, OutError))
			return false;

		UGaussianSplatComponent* Component = GaussianActor->GaussianSplatComponent;
		Component->SHOrder = FMath::Clamp(
			static_cast<int32>(ComponentSettings->GetNumberField(TEXT("sh_order"))), 0, 3);
		Component->SplatScale = FMath::Clamp(
			static_cast<float>(ComponentSettings->GetNumberField(TEXT("splat_scale"))), 0.1f, 10.0f);
		Component->SetOpacityScale(
			static_cast<float>(ComponentSettings->GetNumberField(TEXT("opacity_scale"))));
		Component->SetTreeLODSplatBudget(
			static_cast<int32>(ComponentSettings->GetNumberField(TEXT("tree_lod_splat_budget"))));
		Component->SetTreeLODProgressiveSplatBudget(
			static_cast<int32>(ComponentSettings->GetNumberField(TEXT("tree_lod_progressive_splat_budget"))));
		Component->SetTreeLODDetailScale(
			static_cast<float>(ComponentSettings->GetNumberField(TEXT("tree_lod_detail_scale"))));
		Component->SetForceFullDetailLOD0(
			ComponentSettings->GetBoolField(TEXT("force_full_detail_lod0")));
		Component->SetTreeSourceAsset(RuntimeSource);

		FVector PlayerLocation;
		FRotator PlayerRotation;
		if (!ReadVector(PlayerSettings, TEXT("location"), PlayerLocation, OutError) ||
			!ReadRotator(PlayerSettings, TEXT("rotation"), PlayerRotation, OutError))
		{
			return false;
		}
		APlayerStart* PlayerStart = FindTaggedOrUnique<APlayerStart>(World, PlayerStartTag);
		if (PlayerStart == nullptr)
			PlayerStart = World->SpawnActor<APlayerStart>(PlayerLocation, PlayerRotation);
		if (PlayerStart == nullptr)
		{
			OutError = TEXT("could not create runtime PlayerStart");
			return false;
		}
		if (USceneComponent* Root = PlayerStart->GetRootComponent())
			Root->SetMobility(EComponentMobility::Movable);
		PlayerStart->SetActorLocationAndRotation(
			PlayerLocation, PlayerRotation, false, nullptr, ETeleportType::TeleportPhysics);
		// SetActorLocationAndRotation may report false when the actor already has
		// the requested transform. The observable postcondition is authoritative.
		if (!PlayerStart->GetActorLocation().Equals(PlayerLocation, 0.1))
		{
			OutError = FString::Printf(
				TEXT("could not move runtime PlayerStart to (%.1f,%.1f,%.1f); actual=(%.1f,%.1f,%.1f)"),
				PlayerLocation.X, PlayerLocation.Y, PlayerLocation.Z,
				PlayerStart->GetActorLocation().X,
				PlayerStart->GetActorLocation().Y,
				PlayerStart->GetActorLocation().Z);
			return false;
		}
		PlayerStart->Tags.AddUnique(PlayerStartTag);

		FVector FloorLocation;
		FVector FloorScale;
		if (!ReadVector(FloorSettings, TEXT("location"), FloorLocation, OutError) ||
			!ReadVector(FloorSettings, TEXT("scale"), FloorScale, OutError))
		{
			return false;
		}
		AStaticMeshActor* Floor = FindTaggedOrUnique<AStaticMeshActor>(World, CollisionFloorTag);
		if (Floor == nullptr)
			Floor = World->SpawnActor<AStaticMeshActor>(FloorLocation, FRotator::ZeroRotator);
		if (Floor == nullptr || Floor->GetStaticMeshComponent() == nullptr)
		{
			OutError = TEXT("could not create runtime collision floor");
			return false;
		}
		UStaticMeshComponent* FloorComponent = Floor->GetStaticMeshComponent();
		if (FloorComponent->GetStaticMesh() == nullptr)
		{
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (Cube == nullptr)
			{
				OutError = TEXT("runtime collision cube is not cooked");
				return false;
			}
			FloorComponent->SetStaticMesh(Cube);
		}
		FloorComponent->SetMobility(EComponentMobility::Movable);
		Floor->SetActorLocation(
			FloorLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Floor->SetActorScale3D(FloorScale);
		// Moving to the current location is a valid no-op even when UE returns
		// false; only fail if the resulting transform is actually wrong.
		if (!Floor->GetActorLocation().Equals(FloorLocation, 0.1))
		{
			OutError = FString::Printf(
				TEXT("could not move runtime collision floor to (%.1f,%.1f,%.1f); actual=(%.1f,%.1f,%.1f)"),
				FloorLocation.X, FloorLocation.Y, FloorLocation.Z,
				Floor->GetActorLocation().X,
				Floor->GetActorLocation().Y,
				Floor->GetActorLocation().Z);
			return false;
		}
		Floor->SetActorEnableCollision(true);
		Floor->SetActorHiddenInGame(true);
		Floor->Tags.AddUnique(CollisionFloorTag);
		FloorComponent->SetCollisionProfileName(TEXT("BlockAll"));
		FloorComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		FloorComponent->SetGenerateOverlapEvents(false);
		FloorComponent->SetVisibility(false, true);

		UE_LOG(LogTemp, Display,
			TEXT("NANOGS_RUNTIME_SCENE_OK tree=%s nodes=%lld player=(%.1f,%.1f,%.1f) floor=(%.1f,%.1f,%.1f)"),
			*TreeDirectory, RuntimeSource->GetNodeCount(),
			PlayerStart->GetActorLocation().X,
			PlayerStart->GetActorLocation().Y,
			PlayerStart->GetActorLocation().Z,
			Floor->GetActorLocation().X,
			Floor->GetActorLocation().Y,
			Floor->GetActorLocation().Z);
		return true;
	}
}

bool NanoGS::RuntimeScene::ApplyConfiguredScene(UWorld* World)
{
	if (World == nullptr || !World->IsGameWorld())
		return false;

	const FString ConfigPath = ResolveRuntimeConfig();
	if (!IFileManager::Get().FileExists(*ConfigPath))
	{
		UE_LOG(LogTemp, Log,
			TEXT("NanoGS runtime scene override not present; using cooked scene: %s"), *ConfigPath);
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ConfigPath))
	{
		UE_LOG(LogTemp, Error, TEXT("NANOGS_RUNTIME_SCENE_FAILED could not read %s"), *ConfigPath);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("NANOGS_RUNTIME_SCENE_FAILED invalid JSON: %s"), *ConfigPath);
		return false;
	}
	FString Error;
	if (!ApplyScene(World, Root, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("NANOGS_RUNTIME_SCENE_FAILED %s (%s)"), *Error, *ConfigPath);
		return false;
	}
	return true;
}
