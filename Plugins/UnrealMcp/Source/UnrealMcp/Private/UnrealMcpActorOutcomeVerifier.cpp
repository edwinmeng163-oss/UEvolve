#include "UnrealMcpToolOutcomeVerifiers.h"

#include "UnrealMcpModule.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> ActorMakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TSharedPtr<FJsonObject> ActorMakeVerifierResult(const FString& ToolName)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), ToolName);
			Object->SetStringField(TEXT("category"), TEXT("actors"));
			Object->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_state"));
			Object->SetBoolField(TEXT("toolSpecificVerifierAvailable"), true);
			return Object;
		}

		void ActorFinishVerifier(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Evidence,
			const TArray<FString>& Failures)
		{
			Object->SetBoolField(TEXT("verified"), Failures.Num() == 0);
			Object->SetArrayField(TEXT("evidence"), ActorMakeStringValues(Evidence));
			Object->SetArrayField(TEXT("failures"), ActorMakeStringValues(Failures));
			Object->SetStringField(TEXT("summary"), Failures.Num() == 0
				? TEXT("Actor verifier confirmed the current editor world state.")
				: TEXT("Actor verifier found mismatches; inspect failures for details."));
		}

		UWorld* GetEditorWorld()
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}

		AActor* FindActorByPathOrLabel(const FString& Path, const FString& Label)
		{
			UWorld* World = GetEditorWorld();
			if (!World)
			{
				return nullptr;
			}

			const FString TrimmedPath = Path.TrimStartAndEnd();
			const FString TrimmedLabel = Label.TrimStartAndEnd();
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor)
				{
					continue;
				}
				if (!TrimmedPath.IsEmpty() && Actor->GetPathName().Equals(TrimmedPath, ESearchCase::CaseSensitive))
				{
					return Actor;
				}
				if (!TrimmedLabel.IsEmpty() && Actor->GetActorLabel().Equals(TrimmedLabel, ESearchCase::CaseSensitive))
				{
					return Actor;
				}
			}
			return nullptr;
		}

		FVector ReadVectorObject(const TSharedPtr<FJsonObject>& Object, const FVector& DefaultValue)
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			double X = DefaultValue.X;
			double Y = DefaultValue.Y;
			double Z = DefaultValue.Z;
			Object->TryGetNumberField(TEXT("x"), X);
			Object->TryGetNumberField(TEXT("y"), Y);
			Object->TryGetNumberField(TEXT("z"), Z);
			return FVector(X, Y, Z);
		}

		FRotator ReadRotatorObject(const TSharedPtr<FJsonObject>& Object, const FRotator& DefaultValue)
		{
			if (!Object.IsValid())
			{
				return DefaultValue;
			}
			double Pitch = DefaultValue.Pitch;
			double Yaw = DefaultValue.Yaw;
			double Roll = DefaultValue.Roll;
			Object->TryGetNumberField(TEXT("pitch"), Pitch);
			Object->TryGetNumberField(TEXT("yaw"), Yaw);
			Object->TryGetNumberField(TEXT("roll"), Roll);
			return FRotator(Pitch, Yaw, Roll);
		}

		void VerifyActorObject(
			const TSharedPtr<FJsonObject>& ActorObject,
			TArray<FString>& Evidence,
			TArray<FString>& Failures)
		{
			if (!ActorObject.IsValid())
			{
				return;
			}

			FString Path;
			FString Label;
			ActorObject->TryGetStringField(TEXT("path"), Path);
			ActorObject->TryGetStringField(TEXT("label"), Label);
			AActor* Actor = FindActorByPathOrLabel(Path, Label);
			if (!Actor)
			{
				Failures.Add(FString::Printf(TEXT("Actor '%s' was not found."), Path.IsEmpty() ? *Label : *Path));
				return;
			}
			Evidence.Add(FString::Printf(TEXT("Actor exists: %s."), *Actor->GetPathName()));

			const TSharedPtr<FJsonObject>* AfterLocationObject = nullptr;
			if (ActorObject->TryGetObjectField(TEXT("afterLocation"), AfterLocationObject) && AfterLocationObject && (*AfterLocationObject).IsValid())
			{
				const FVector ExpectedLocation = ReadVectorObject(*AfterLocationObject, Actor->GetActorLocation());
				if (Actor->GetActorLocation().Equals(ExpectedLocation, KINDA_SMALL_NUMBER))
				{
					Evidence.Add(FString::Printf(TEXT("Actor '%s' location matches reported afterLocation."), *Actor->GetActorLabel()));
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("Actor '%s' location does not match reported afterLocation."), *Actor->GetActorLabel()));
				}
			}

			const TSharedPtr<FJsonObject>* AfterRotationObject = nullptr;
			if (ActorObject->TryGetObjectField(TEXT("afterRotation"), AfterRotationObject) && AfterRotationObject && (*AfterRotationObject).IsValid())
			{
				const FRotator ExpectedRotation = ReadRotatorObject(*AfterRotationObject, Actor->GetActorRotation());
				if (Actor->GetActorRotation().Equals(ExpectedRotation, KINDA_SMALL_NUMBER))
				{
					Evidence.Add(FString::Printf(TEXT("Actor '%s' rotation matches reported afterRotation."), *Actor->GetActorLabel()));
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("Actor '%s' rotation does not match reported afterRotation."), *Actor->GetActorLabel()));
				}
			}

			const TSharedPtr<FJsonObject>* AfterScaleObject = nullptr;
			if (ActorObject->TryGetObjectField(TEXT("afterScale"), AfterScaleObject) && AfterScaleObject && (*AfterScaleObject).IsValid())
			{
				const FVector ExpectedScale = ReadVectorObject(*AfterScaleObject, Actor->GetActorScale3D());
				if (Actor->GetActorScale3D().Equals(ExpectedScale, KINDA_SMALL_NUMBER))
				{
					Evidence.Add(FString::Printf(TEXT("Actor '%s' scale matches reported afterScale."), *Actor->GetActorLabel()));
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("Actor '%s' scale does not match reported afterScale."), *Actor->GetActorLabel()));
				}
			}
		}

		void VerifyActorArray(
			const TSharedPtr<FJsonObject>& StructuredContent,
			TArray<FString>& Evidence,
			TArray<FString>& Failures)
		{
			if (!StructuredContent.IsValid())
			{
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
			if (!StructuredContent->TryGetArrayField(TEXT("actors"), ActorsArray) || ActorsArray == nullptr)
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArray)
			{
				if (ActorValue.IsValid() && ActorValue->Type == EJson::Object)
				{
					VerifyActorObject(ActorValue->AsObject(), Evidence, Failures);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BuildActorToolPreflight(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& GenericPreflight)
	{
		const bool bIsActorTool =
			ToolName.Contains(TEXT("actor"))
			|| ToolName.Contains(TEXT("spawn"))
			|| ToolName.Contains(TEXT("layout"))
			|| ToolName == TEXT("unreal.clear_level_environment");
		if (!bIsActorTool || !GenericPreflight.IsValid())
		{
			return nullptr;
		}

		TArray<FString> Evidence;
		TArray<FString> Failures;
		GenericPreflight->SetBoolField(TEXT("toolSpecificPreflightAvailable"), true);
		GenericPreflight->SetStringField(TEXT("category"), TEXT("actors"));
		GenericPreflight->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_preflight"));

		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Failures.Add(TEXT("No editor world is available for actor tool execution."));
		}
		else
		{
			Evidence.Add(FString::Printf(TEXT("Editor world is available: %s."), *World->GetName()));
		}

		if (ToolName == TEXT("unreal.destroy_selected_actors"))
		{
			const int32 SelectedActorCount = GEditor ? GEditor->GetSelectedActorCount() : 0;
			if (SelectedActorCount > 0)
			{
				Evidence.Add(FString::Printf(TEXT("%d actors are selected before destroy."), SelectedActorCount));
			}
			else
			{
				Failures.Add(TEXT("No actors are selected before destroy_selected_actors."));
			}
		}
		else if (ToolName == TEXT("unreal.spawn_static_mesh_actor"))
		{
			FString StaticMeshPath;
			Arguments.TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath);
			if (StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				Failures.Add(TEXT("staticMeshPath is required for spawn_static_mesh_actor."));
			}
			else if (StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *StaticMeshPath))
			{
				Evidence.Add(FString::Printf(TEXT("Static mesh asset is loadable before spawn: %s."), *StaticMeshPath));
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("Static mesh asset could not be loaded before spawn: %s."), *StaticMeshPath));
			}
		}
		else if (ToolName == TEXT("unreal.spawn_actor_batch") || ToolName == TEXT("unreal.spawn_actor_batch_basic"))
		{
			const TArray<TSharedPtr<FJsonValue>>* ItemsArray = nullptr;
			if (Arguments.TryGetArrayField(TEXT("items"), ItemsArray) && ItemsArray)
			{
				Evidence.Add(FString::Printf(TEXT("Batch spawn request contains %d items."), ItemsArray->Num()));
			}
			else
			{
				Failures.Add(TEXT("Batch spawn request does not contain an items array."));
			}
		}
		else if (ToolName.StartsWith(TEXT("unreal.layout_")) || ToolName.StartsWith(TEXT("unreal.batch_set_")))
		{
			const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
			if (Arguments.TryGetArrayField(TEXT("paths"), PathsArray) && PathsArray)
			{
				Evidence.Add(FString::Printf(TEXT("Actor path request contains %d paths."), PathsArray->Num()));
			}
			else
			{
				Evidence.Add(TEXT("No explicit actor paths supplied; tool may use current selection or filter arguments."));
			}
		}

		GenericPreflight->SetBoolField(TEXT("ready"), Failures.Num() == 0);
		GenericPreflight->SetArrayField(TEXT("evidence"), ActorMakeStringValues(Evidence));
		GenericPreflight->SetArrayField(TEXT("failures"), ActorMakeStringValues(Failures));
		GenericPreflight->SetStringField(TEXT("summary"), Failures.Num() == 0
			? TEXT("Actor preflight confirmed editor world, selection, asset, or request shape before execution.")
			: TEXT("Actor preflight found missing state; inspect failures before applying."));
		return GenericPreflight;
	}

	TSharedPtr<FJsonObject> VerifyActorToolOutcome(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const FUnrealMcpExecutionResult& Result)
	{
		const bool bIsActorTool =
			ToolName.Contains(TEXT("actor"))
			|| ToolName.Contains(TEXT("spawn"))
			|| ToolName.Contains(TEXT("layout"))
			|| ToolName == TEXT("unreal.clear_level_environment");
		if (!bIsActorTool)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Verifier = ActorMakeVerifierResult(ToolName);
		Verifier->SetBoolField(TEXT("toolReturnedError"), Result.bIsError);
		Verifier->SetBoolField(TEXT("genericResultSucceeded"), !Result.bIsError);
		TArray<FString> Evidence;
		TArray<FString> Failures;

		if (Result.bIsError)
		{
			Failures.Add(TEXT("Tool returned an error; actor success state was not verified."));
			ActorFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (!Result.StructuredContent.IsValid())
		{
			Failures.Add(TEXT("Actor tool did not return structured content to verify."));
			ActorFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.destroy_selected_actors"))
		{
			bool bDestroyed = false;
			Result.StructuredContent->TryGetBoolField(TEXT("destroyed"), bDestroyed);
			if (bDestroyed)
			{
				Evidence.Add(TEXT("Destroy selected actors reported successful destruction."));
			}
			else
			{
				Failures.Add(TEXT("Destroy selected actors did not report successful destruction."));
			}
			ActorFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.clear_level_environment"))
		{
			bool bDryRun = false;
			Result.StructuredContent->TryGetBoolField(TEXT("dryRun"), bDryRun);
			if (bDryRun)
			{
				Evidence.Add(TEXT("Dry-run clear verified by structured result; no destructive state check required."));
			}
			else
			{
				double RemainingTargetCount = 0.0;
				Result.StructuredContent->TryGetNumberField(TEXT("remainingTargetCount"), RemainingTargetCount);
				if (FMath::IsNearlyZero(RemainingTargetCount))
				{
					Evidence.Add(TEXT("No matching target actors remain after clear."));
				}
				else
				{
					Failures.Add(FString::Printf(TEXT("%.0f matching target actors remain after clear."), RemainingTargetCount));
				}
			}
			ActorFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.spawn_actor_batch"))
		{
			const TArray<TSharedPtr<FJsonValue>>* ItemsArray = nullptr;
			if (Result.StructuredContent->TryGetArrayField(TEXT("items"), ItemsArray) && ItemsArray)
			{
				for (const TSharedPtr<FJsonValue>& ItemValue : *ItemsArray)
				{
					if (ItemValue.IsValid() && ItemValue->Type == EJson::Object)
					{
						VerifyActorObject(ItemValue->AsObject(), Evidence, Failures);
					}
				}
			}
			ActorFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		VerifyActorObject(Result.StructuredContent, Evidence, Failures);
		VerifyActorArray(Result.StructuredContent, Evidence, Failures);

		if (ToolName == TEXT("unreal.spawn_static_mesh_actor"))
		{
			FString Path;
			FString Label;
			Result.StructuredContent->TryGetStringField(TEXT("path"), Path);
			Result.StructuredContent->TryGetStringField(TEXT("label"), Label);
			AActor* Actor = FindActorByPathOrLabel(Path, Label);
			UStaticMeshComponent* StaticMeshComponent = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
			FString ExpectedMeshPath;
			Result.StructuredContent->TryGetStringField(TEXT("staticMeshPath"), ExpectedMeshPath);
			const FString ActualMeshPath = StaticMeshComponent && StaticMeshComponent->GetStaticMesh()
				? StaticMeshComponent->GetStaticMesh()->GetPathName()
				: FString();
			if (!ExpectedMeshPath.IsEmpty() && ActualMeshPath.Equals(ExpectedMeshPath, ESearchCase::CaseSensitive))
			{
				Evidence.Add(FString::Printf(TEXT("StaticMeshComponent mesh matches %s."), *ExpectedMeshPath));
			}
			else
			{
				Failures.Add(TEXT("StaticMeshComponent mesh does not match reported staticMeshPath."));
			}
		}

		if (Evidence.Num() == 0 && Failures.Num() == 0)
		{
			Evidence.Add(TEXT("Structured actor result was present; no deeper verifier was available for this actor tool."));
		}

		ActorFinishVerifier(Verifier, Evidence, Failures);
		return Verifier;
	}
}
