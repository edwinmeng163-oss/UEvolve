#include "UnrealMcpActorTools.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorScriptingHelpers.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UnrealMcpModule.h"
#include "UObject/UnrealType.h"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem);
	bool ApplyPropertyMapToActor(
		AActor* Actor,
		const FJsonObject& PropertyValues,
		TArray<TSharedPtr<FJsonValue>>& OutEditResults,
		int32& OutSuccessCount,
		int32& OutFailureCount);
	bool ResolveObjectPropertyPath(
		UObject* RootObject,
		const FString& PropertyPath,
		UObject*& OutOwnerObject,
		FProperty*& OutLeafProperty,
		FProperty*& OutNotifyProperty,
		void*& OutValuePtr,
		FString& OutFailureReason);
	TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr);

	namespace
	{
		static constexpr int32 ActorToolDefaultListLimit = 200;

		struct FActorToolQueryResult
		{
			TArray<AActor*> Actors;
			TArray<FString> RequestedPaths;
			FString FilterText;
			FString ClassPathFilter;
			int32 MatchCount = 0;
			int32 Limit = ActorToolDefaultListLimit;
			bool bSelectedOnly = false;
			bool bTruncated = false;
		};

		FUnrealMcpExecutionResult ActorToolMakeExecutionResult(
			const FString& Text,
			const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
			bool bIsError = false)
		{
			FUnrealMcpExecutionResult Result;
			Result.Text = Text;
			Result.StructuredContent = StructuredContent;
			Result.bIsError = bIsError;
			return Result;
		}

		int32 ActorToolGetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue)
		{
			double Value = static_cast<double>(DefaultValue);
			if (Arguments.TryGetNumberField(FieldName, Value))
			{
				return FMath::Max(1, static_cast<int32>(Value));
			}

			return DefaultValue;
		}

		bool ActorToolTryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Arguments.TryGetArrayField(FieldName, JsonArray) || !JsonArray)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
			{
				FString StringValue;
				if (Value.IsValid() && Value->TryGetString(StringValue))
				{
					OutValues.Add(StringValue);
				}
			}

			return true;
		}

		bool ActorToolTryGetObjectArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<TSharedPtr<FJsonObject>>& OutValues)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Arguments.TryGetArrayField(FieldName, JsonArray) || !JsonArray)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
				{
					return false;
				}

				OutValues.Add(Value->AsObject());
			}

			return true;
		}

		void ActorToolCopyNumberFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			double Value = 0.0;
			if (Destination.IsValid() && Source.TryGetNumberField(FieldName, Value))
			{
				Destination->SetNumberField(FieldName, Value);
			}
		}

		void ActorToolCopyStringFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			FString Value;
			if (Destination.IsValid() && Source.TryGetStringField(FieldName, Value))
			{
				Destination->SetStringField(FieldName, Value);
			}
		}

		TArray<TSharedPtr<FJsonValue>> ActorToolMakeJsonStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TSharedPtr<FJsonObject> ActorToolMakeVectorObject(const FVector& Vector)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("x"), Vector.X);
			Object->SetNumberField(TEXT("y"), Vector.Y);
			Object->SetNumberField(TEXT("z"), Vector.Z);
			return Object;
		}

		TSharedPtr<FJsonObject> ActorToolMakeRotatorObject(const FRotator& Rotator)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
			Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
			Object->SetNumberField(TEXT("roll"), Rotator.Roll);
			return Object;
		}

		TSharedPtr<FJsonObject> ActorToolMakeActorObject(AActor* Actor)
		{
			TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
			ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
			ActorObject->SetStringField(TEXT("name"), Actor->GetName());
			ActorObject->SetStringField(TEXT("classPath"), Actor->GetClass()->GetPathName());
			ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());
			ActorObject->SetObjectField(TEXT("location"), ActorToolMakeVectorObject(Actor->GetActorLocation()));
			ActorObject->SetObjectField(TEXT("rotation"), ActorToolMakeRotatorObject(Actor->GetActorRotation()));
			return ActorObject;
		}

		void ActorToolAttachError(TSharedPtr<FJsonObject> StructuredContent, const FString& Code, const FString& Message)
		{
			if (!StructuredContent.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
			ErrorObject->SetStringField(TEXT("code"), Code);
			ErrorObject->SetStringField(TEXT("message"), Message);
			StructuredContent->SetObjectField(TEXT("error"), ErrorObject);
		}

		bool ActorToolIsContainerProperty(const FProperty* Property)
		{
			return Property
				&& (Property->IsA<FArrayProperty>()
					|| Property->IsA<FSetProperty>()
					|| Property->IsA<FMapProperty>());
		}

		FString ActorToolGetPropertyTypeName(const FProperty* Property)
		{
			if (!Property)
			{
				return FString();
			}

			const FString CppType = Property->GetCPPType();
			return CppType.IsEmpty() ? Property->GetClass()->GetName() : CppType;
		}

		AActor* ActorToolResolveActorPathOrLabel(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FString& ActorPathOrLabel,
			FString& OutFailureReason)
		{
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return nullptr;
			}

			const FString RequestedActor = ActorPathOrLabel.TrimStartAndEnd();
			if (RequestedActor.IsEmpty())
			{
				OutFailureReason = TEXT("actorPath is required.");
				return nullptr;
			}

			const TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();
			for (AActor* Actor : AllActors)
			{
				if (Actor && Actor->GetPathName().Equals(RequestedActor, ESearchCase::IgnoreCase))
				{
					return Actor;
				}
			}

			AActor* LabelMatch = nullptr;
			for (AActor* Actor : AllActors)
			{
				if (!Actor || !Actor->GetActorLabel().Equals(RequestedActor, ESearchCase::IgnoreCase))
				{
					continue;
				}

				if (LabelMatch)
				{
					OutFailureReason = FString::Printf(TEXT("Multiple actors matched actorPath label '%s'. Use the unique actor path instead."), *RequestedActor);
					return nullptr;
				}

				LabelMatch = Actor;
			}

			if (LabelMatch)
			{
				return LabelMatch;
			}

			AActor* NameMatch = nullptr;
			for (AActor* Actor : AllActors)
			{
				if (!Actor || !Actor->GetName().Equals(RequestedActor, ESearchCase::IgnoreCase))
				{
					continue;
				}

				if (NameMatch)
				{
					OutFailureReason = FString::Printf(TEXT("Multiple actors matched actorPath name '%s'. Use the unique actor path instead."), *RequestedActor);
					return nullptr;
				}

				NameMatch = Actor;
			}

			if (NameMatch)
			{
				return NameMatch;
			}

			OutFailureReason = FString::Printf(TEXT("No actor matched actorPath '%s'."), *RequestedActor);
			return nullptr;
		}

		FUnrealMcpExecutionResult ActorToolMakeReadbackError(
			const FString& Text,
			const FString& Code,
			const FString& Message,
			const FString& ActorPath,
			const FString& PropertyName = FString())
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("actorPath"), ActorPath);
			if (!PropertyName.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("propertyName"), PropertyName);
			}
			ActorToolAttachError(StructuredContent, Code, Message);
			return ActorToolMakeExecutionResult(Text, StructuredContent, true);
		}

		FString ActorToolDescribeActor(AActor* Actor)
		{
			const FVector Location = Actor->GetActorLocation();
			return FString::Printf(
				TEXT("%s (%s) @ [%.1f, %.1f, %.1f]"),
				*Actor->GetActorLabel(),
				*Actor->GetClass()->GetName(),
				Location.X,
				Location.Y,
				Location.Z);
		}

		bool ActorToolMatchesActorFilters(
			AActor* Actor,
			const FString& FilterText,
			const FString& ClassPathFilter,
			const TSet<FString>& ExplicitPaths)
		{
			const FString ActorLabel = Actor->GetActorLabel();
			const FString ActorName = Actor->GetName();
			const FString ActorClassPath = Actor->GetClass()->GetPathName();
			const FString ActorPath = Actor->GetPathName();

			if (ExplicitPaths.Num() > 0 && !ExplicitPaths.Contains(ActorPath))
			{
				return false;
			}

			if (!FilterText.IsEmpty()
				&& !ActorLabel.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorName.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorPath.Contains(FilterText, ESearchCase::IgnoreCase))
			{
				return false;
			}

			if (!ClassPathFilter.IsEmpty()
				&& !ActorClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
			{
				return false;
			}

			return true;
		}

		UEditorActorSubsystem* ActorToolGetEditorActorSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
		}

		UEditorAssetSubsystem* ActorToolGetEditorAssetSubsystem()
		{
			return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		}

		UObject* ActorToolLoadAssetFromAnyPath(
			UEditorAssetSubsystem* EditorAssetSubsystem,
			const FString& AnyAssetPath,
			FString& OutObjectPath,
			FString& OutFailureReason)
		{
			if (!EditorAssetSubsystem)
			{
				OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
				return nullptr;
			}

			OutObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AnyAssetPath, OutFailureReason);
			if (OutObjectPath.IsEmpty())
			{
				return nullptr;
			}

			UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(OutObjectPath);
			if (!LoadedAsset)
			{
				OutFailureReason = FString::Printf(TEXT("Failed to load asset '%s'."), *OutObjectPath);
			}

			return LoadedAsset;
		}

		AActor* ActorToolResolveActorReference(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FString& ActorPath,
			const FString& ActorLabel,
			FString& OutFailureReason)
		{
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return nullptr;
			}

			const TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();

			if (!ActorPath.TrimStartAndEnd().IsEmpty())
			{
				for (AActor* Actor : AllActors)
				{
					if (Actor && Actor->GetPathName().Equals(ActorPath, ESearchCase::IgnoreCase))
					{
						return Actor;
					}
				}

				OutFailureReason = FString::Printf(TEXT("No actor matched actorPath '%s'."), *ActorPath);
				return nullptr;
			}

			if (!ActorLabel.TrimStartAndEnd().IsEmpty())
			{
				AActor* MatchedActor = nullptr;
				for (AActor* Actor : AllActors)
				{
					if (Actor && Actor->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase))
					{
						if (MatchedActor)
						{
							OutFailureReason = FString::Printf(TEXT("Multiple actors matched actorLabel '%s'. Use actorPath instead."), *ActorLabel);
							return nullptr;
						}

						MatchedActor = Actor;
					}
				}

				if (MatchedActor)
				{
					return MatchedActor;
				}

				OutFailureReason = FString::Printf(TEXT("No actor matched actorLabel '%s'."), *ActorLabel);
				return nullptr;
			}

			const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
			if (SelectedActors.Num() == 1 && SelectedActors[0])
			{
				return SelectedActors[0];
			}

			OutFailureReason = SelectedActors.Num() > 1
				? TEXT("Multiple actors are selected. Provide actorPath or actorLabel.")
				: TEXT("No actor reference was provided, and there is no single selected actor.");
			return nullptr;
		}

		bool ActorToolResolveActorsFromArguments(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FJsonObject& Arguments,
			FActorToolQueryResult& OutQuery,
			FString& OutFailureReason)
		{
			OutQuery = FActorToolQueryResult();
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return false;
			}

			Arguments.TryGetStringField(TEXT("filter"), OutQuery.FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), OutQuery.ClassPathFilter);
			Arguments.TryGetBoolField(TEXT("selectedOnly"), OutQuery.bSelectedOnly);
			ActorToolTryGetStringArrayField(Arguments, TEXT("paths"), OutQuery.RequestedPaths);
			OutQuery.Limit = FMath::Min(ActorToolGetPositiveIntArgument(Arguments, TEXT("limit"), ActorToolDefaultListLimit), 1000);

			TSet<FString> ExplicitPaths;
			for (const FString& RequestedPath : OutQuery.RequestedPaths)
			{
				const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
				if (!TrimmedPath.IsEmpty())
				{
					ExplicitPaths.Add(TrimmedPath);
				}
			}

			const bool bHasSelectors = !OutQuery.FilterText.TrimStartAndEnd().IsEmpty()
				|| !OutQuery.ClassPathFilter.TrimStartAndEnd().IsEmpty()
				|| ExplicitPaths.Num() > 0;

			if (OutQuery.bSelectedOnly || !bHasSelectors)
			{
				OutQuery.Actors = EditorActorSubsystem->GetSelectedLevelActors();
				OutQuery.Actors.RemoveAll([](AActor* Actor) { return Actor == nullptr; });
				OutQuery.MatchCount = OutQuery.Actors.Num();

				if (OutQuery.Actors.Num() == 0)
				{
					OutFailureReason = OutQuery.bSelectedOnly
						? TEXT("No actors are currently selected.")
						: TEXT("No actor selectors were provided, and there are no selected actors to act on.");
					return false;
				}

				OutQuery.Actors.Sort([](const AActor& A, const AActor& B)
				{
					return A.GetActorLabel() < B.GetActorLabel();
				});

				if (OutQuery.Actors.Num() > OutQuery.Limit)
				{
					OutQuery.Actors.SetNum(OutQuery.Limit);
					OutQuery.bTruncated = true;
				}

				return true;
			}

			TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();
			AllActors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel() < B.GetActorLabel();
			});

			for (AActor* Actor : AllActors)
			{
				if (!Actor || !ActorToolMatchesActorFilters(Actor, OutQuery.FilterText, OutQuery.ClassPathFilter, ExplicitPaths))
				{
					continue;
				}

				++OutQuery.MatchCount;
				if (OutQuery.Actors.Num() >= OutQuery.Limit)
				{
					OutQuery.bTruncated = true;
					continue;
				}

				OutQuery.Actors.Add(Actor);
			}

			if (OutQuery.MatchCount == 0)
			{
				OutFailureReason = TEXT("No actors matched the provided criteria.");
				return false;
			}

			return true;
		}

		FUnrealMcpExecutionResult ExecuteListLevelActors(const FJsonObject& Arguments)
		{
			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString FilterText;
			FString ClassPathFilter;
			Arguments.TryGetStringField(TEXT("filter"), FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
			const int32 Limit = ActorToolGetPositiveIntArgument(Arguments, TEXT("limit"), ActorToolDefaultListLimit);

			TArray<AActor*> SortedActors = EditorActorSubsystem->GetAllLevelActors();
			SortedActors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel() < B.GetActorLabel();
			});

			int32 TotalMatches = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> ActorsArray;
			TArray<FString> TextLines;

			for (AActor* Actor : SortedActors)
			{
				if (!Actor)
				{
					continue;
				}

				const FString ActorLabel = Actor->GetActorLabel();
				const FString ActorName = Actor->GetName();
				const FString ActorClassPath = Actor->GetClass()->GetPathName();

				if (!FilterText.IsEmpty()
					&& !ActorLabel.Contains(FilterText, ESearchCase::IgnoreCase)
					&& !ActorName.Contains(FilterText, ESearchCase::IgnoreCase)
					&& !ActorClassPath.Contains(FilterText, ESearchCase::IgnoreCase))
				{
					continue;
				}

				if (!ClassPathFilter.IsEmpty()
					&& !ActorClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
					&& !ActorClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}

				++TotalMatches;
				if (ActorsArray.Num() >= Limit)
				{
					bTruncated = true;
					continue;
				}

				ActorsArray.Add(MakeShared<FJsonValueObject>(ActorToolMakeActorObject(Actor)));
				TextLines.Add(ActorToolDescribeActor(Actor));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
			StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
			StructuredContent->SetNumberField(TEXT("returnedCount"), ActorsArray.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

			FString Text;
			if (TextLines.Num() > 0)
			{
				Text = FString::Printf(TEXT("Found %d actors"), TotalMatches);
				if (!FilterText.IsEmpty())
				{
					Text += FString::Printf(TEXT(" matching '%s'"), *FilterText);
				}
				if (!ClassPathFilter.IsEmpty())
				{
					Text += FString::Printf(TEXT(" in class '%s'"), *ClassPathFilter);
				}
				if (bTruncated)
				{
					Text += FString::Printf(TEXT(" (showing first %d)"), ActorsArray.Num());
				}
				Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
			}
			else
			{
				Text = TEXT("Found 0 matching actors in the current level.");
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteListSelectedActors()
		{
			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
			TArray<TSharedPtr<FJsonValue>> ActorsArray;
			TArray<FString> TextLines;

			for (AActor* Actor : SelectedActors)
			{
				if (!Actor)
				{
					continue;
				}

				ActorsArray.Add(MakeShared<FJsonValueObject>(ActorToolMakeActorObject(Actor)));
				TextLines.Add(ActorToolDescribeActor(Actor));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("count"), ActorsArray.Num());
			StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

			const FString Text = TextLines.Num() > 0
				? FString::Printf(TEXT("Selected actors (%d):\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
				: TEXT("No actors are currently selected in the level editor.");

			return ActorToolMakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteSelectActors(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString FilterText;
			FString ClassPathFilter;
			bool bClearSelection = true;
			TArray<FString> RequestedPaths;
			const int32 Limit = ActorToolGetPositiveIntArgument(Arguments, TEXT("limit"), ActorToolDefaultListLimit);

			Arguments.TryGetStringField(TEXT("filter"), FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
			Arguments.TryGetBoolField(TEXT("clearSelection"), bClearSelection);
			ActorToolTryGetStringArrayField(Arguments, TEXT("paths"), RequestedPaths);

			TSet<FString> ExplicitPaths;
			for (const FString& RequestedPath : RequestedPaths)
			{
				const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
				if (!TrimmedPath.IsEmpty())
				{
					ExplicitPaths.Add(TrimmedPath);
				}
			}

			if (FilterText.TrimStartAndEnd().IsEmpty() && ClassPathFilter.TrimStartAndEnd().IsEmpty() && ExplicitPaths.Num() == 0)
			{
				return ActorToolMakeExecutionResult(TEXT("Provide at least one of filter, classPath, or paths to select actors."), nullptr, true);
			}

			if (bClearSelection)
			{
				EditorActorSubsystem->SelectNothing();
			}

			TArray<AActor*> SortedActors = EditorActorSubsystem->GetAllLevelActors();
			SortedActors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel() < B.GetActorLabel();
			});

			int32 TotalMatches = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> ActorsArray;
			TArray<FString> TextLines;

			for (AActor* Actor : SortedActors)
			{
				if (!Actor || !ActorToolMatchesActorFilters(Actor, FilterText, ClassPathFilter, ExplicitPaths))
				{
					continue;
				}

				++TotalMatches;
				if (ActorsArray.Num() >= Limit)
				{
					bTruncated = true;
					continue;
				}

				EditorActorSubsystem->SetActorSelectionState(Actor, true);
				ActorsArray.Add(MakeShared<FJsonValueObject>(ActorToolMakeActorObject(Actor)));
				TextLines.Add(ActorToolDescribeActor(Actor));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
			StructuredContent->SetBoolField(TEXT("clearSelection"), bClearSelection);
			StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
			StructuredContent->SetNumberField(TEXT("returnedCount"), ActorsArray.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

			FString Text;
			if (TextLines.Num() > 0)
			{
				Text = FString::Printf(TEXT("Selected %d actors"), TotalMatches);
				if (!FilterText.IsEmpty())
				{
					Text += FString::Printf(TEXT(" matching '%s'"), *FilterText);
				}
				if (!ClassPathFilter.IsEmpty())
				{
					Text += FString::Printf(TEXT(" in class '%s'"), *ClassPathFilter);
				}
				if (bTruncated)
				{
					Text += FString::Printf(TEXT(" (showing first %d)"), ActorsArray.Num());
				}
				Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
			}
			else
			{
				Text = TEXT("Selected 0 actors because nothing matched the provided criteria.");
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteActorGetProperty(const FJsonObject& Arguments)
		{
			FString ActorPath;
			FString PropertyName;
			Arguments.TryGetStringField(TEXT("actorPath"), ActorPath);
			Arguments.TryGetStringField(TEXT("propertyName"), PropertyName);
			ActorPath = ActorPath.TrimStartAndEnd();
			PropertyName = PropertyName.TrimStartAndEnd();

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			FString FailureReason;
			AActor* Actor = ActorToolResolveActorPathOrLabel(EditorActorSubsystem, ActorPath, FailureReason);
			if (!Actor)
			{
				const FString Message = FailureReason.IsEmpty() ? TEXT("Actor was not found.") : FailureReason;
				return ActorToolMakeReadbackError(Message, TEXT("ACTOR_NOT_FOUND"), Message, ActorPath, PropertyName);
			}

			if (PropertyName.IsEmpty())
			{
				const FString Message = TEXT("propertyName is required.");
				return ActorToolMakeReadbackError(Message, TEXT("PROPERTY_NOT_FOUND"), Message, ActorPath, PropertyName);
			}

			UObject* OwnerObject = nullptr;
			FProperty* LeafProperty = nullptr;
			FProperty* NotifyProperty = nullptr;
			void* ValuePtr = nullptr;
			if (!ResolveObjectPropertyPath(Actor, PropertyName, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, FailureReason))
			{
				const FString Message = FailureReason.IsEmpty()
					? FString::Printf(TEXT("Property '%s' was not found on actor '%s'."), *PropertyName, *Actor->GetActorLabel())
					: FailureReason;
				return ActorToolMakeReadbackError(Message, TEXT("PROPERTY_NOT_FOUND"), Message, ActorPath, PropertyName);
			}

			const TSharedPtr<FJsonValue> JsonValue = PropertyValueToJson(LeafProperty, ValuePtr);
			if (!JsonValue.IsValid())
			{
				const FString Message = FString::Printf(TEXT("Property '%s' could not be serialized."), *PropertyName);
				return ActorToolMakeReadbackError(Message, TEXT("UNSUPPORTED_PROPERTY_TYPE"), Message, ActorPath, PropertyName);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("actorPath"), ActorPath);
			StructuredContent->SetStringField(TEXT("propertyName"), PropertyName);
			StructuredContent->SetStringField(TEXT("propertyType"), ActorToolGetPropertyTypeName(LeafProperty));
			StructuredContent->SetField(TEXT("value"), JsonValue);
			StructuredContent->SetBoolField(TEXT("isContainer"), ActorToolIsContainerProperty(LeafProperty));

			return ActorToolMakeExecutionResult(
				FString::Printf(TEXT("Read property '%s' from actor '%s'."), *PropertyName, *Actor->GetActorLabel()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteActorGetTransform(const FJsonObject& Arguments)
		{
			FString ActorPath;
			FString Space = TEXT("world");
			Arguments.TryGetStringField(TEXT("actorPath"), ActorPath);
			Arguments.TryGetStringField(TEXT("space"), Space);
			ActorPath = ActorPath.TrimStartAndEnd();
			Space = Space.TrimStartAndEnd().ToLower();
			if (Space.IsEmpty())
			{
				Space = TEXT("world");
			}

			if (Space != TEXT("world") && Space != TEXT("relative"))
			{
				const FString Message = FString::Printf(TEXT("Invalid transform space '%s'. Use 'world' or 'relative'."), *Space);
				return ActorToolMakeReadbackError(Message, TEXT("INVALID_SPACE"), Message, ActorPath);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			FString FailureReason;
			AActor* Actor = ActorToolResolveActorPathOrLabel(EditorActorSubsystem, ActorPath, FailureReason);
			if (!Actor)
			{
				const FString Message = FailureReason.IsEmpty() ? TEXT("Actor was not found.") : FailureReason;
				return ActorToolMakeReadbackError(Message, TEXT("ACTOR_NOT_FOUND"), Message, ActorPath);
			}

			const bool bRelative = Space == TEXT("relative");
			const FTransform Transform = bRelative && Actor->GetRootComponent()
				? Actor->GetRootComponent()->GetRelativeTransform()
				: Actor->GetActorTransform();
			const FRotator Rotation = Transform.GetRotation().Rotator();
			AActor* ParentActor = Actor->GetAttachParentActor();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("actorPath"), ActorPath);
			StructuredContent->SetStringField(TEXT("space"), Space);
			StructuredContent->SetObjectField(TEXT("location"), ActorToolMakeVectorObject(Transform.GetLocation()));
			StructuredContent->SetObjectField(TEXT("rotation"), ActorToolMakeRotatorObject(Rotation));
			StructuredContent->SetObjectField(TEXT("scale"), ActorToolMakeVectorObject(Transform.GetScale3D()));
			StructuredContent->SetStringField(TEXT("parentActorPath"), ParentActor ? ParentActor->GetPathName() : FString());

			return ActorToolMakeExecutionResult(
				FString::Printf(TEXT("Read %s transform for actor '%s'."), *Space, *Actor->GetActorLabel()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteSetActorTransform(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString ActorPath;
			FString ActorLabel;
			Arguments.TryGetStringField(TEXT("actorPath"), ActorPath);
			Arguments.TryGetStringField(TEXT("actorLabel"), ActorLabel);

			FString FailureReason;
			AActor* Actor = ActorToolResolveActorReference(EditorActorSubsystem, ActorPath, ActorLabel, FailureReason);
			if (!Actor)
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			const FVector OriginalLocation = Actor->GetActorLocation();
			const FRotator OriginalRotation = Actor->GetActorRotation();
			FVector NewLocation = OriginalLocation;
			FRotator NewRotation = OriginalRotation;
			bool bHasTransformField = false;

			double Value = 0.0;
			if (Arguments.TryGetNumberField(TEXT("x"), Value))
			{
				NewLocation.X = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("y"), Value))
			{
				NewLocation.Y = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("z"), Value))
			{
				NewLocation.Z = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("pitch"), Value))
			{
				NewRotation.Pitch = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("yaw"), Value))
			{
				NewRotation.Yaw = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("roll"), Value))
			{
				NewRotation.Roll = Value;
				bHasTransformField = true;
			}

			if (!bHasTransformField)
			{
				return ActorToolMakeExecutionResult(TEXT("Provide at least one of x, y, z, pitch, yaw, or roll."), nullptr, true);
			}

			const bool bMoved = EditorActorSubsystem->SetActorTransform(
				Actor,
				FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

			TSharedPtr<FJsonObject> StructuredContent = ActorToolMakeActorObject(Actor);
			StructuredContent->SetObjectField(TEXT("beforeLocation"), ActorToolMakeVectorObject(OriginalLocation));
			StructuredContent->SetObjectField(TEXT("beforeRotation"), ActorToolMakeRotatorObject(OriginalRotation));
			StructuredContent->SetObjectField(TEXT("afterLocation"), ActorToolMakeVectorObject(Actor->GetActorLocation()));
			StructuredContent->SetObjectField(TEXT("afterRotation"), ActorToolMakeRotatorObject(Actor->GetActorRotation()));

			return ActorToolMakeExecutionResult(
				FString::Printf(TEXT("Set transform on actor %s. success=%s"), *Actor->GetActorLabel(), bMoved ? TEXT("true") : TEXT("false")),
				StructuredContent,
				!bMoved);
		}

		FUnrealMcpExecutionResult ExecuteDestroySelectedActors(const FString& ToolName)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
			if (SelectedActors.Num() == 0)
			{
				return ActorToolMakeExecutionResult(TEXT("No selected actors to destroy."), nullptr, false);
			}

			const int32 ActorCount = SelectedActors.Num();
			const bool bDestroyed = EditorActorSubsystem->DestroyActors(SelectedActors);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("count"), ActorCount);
			StructuredContent->SetBoolField(TEXT("destroyed"), bDestroyed);

			return ActorToolMakeExecutionResult(
				FString::Printf(TEXT("Destroy selected actors result: destroyed=%s count=%d"), bDestroyed ? TEXT("true") : TEXT("false"), ActorCount),
				StructuredContent,
				!bDestroyed);
		}

		FUnrealMcpExecutionResult ExecuteClearLevelEnvironment(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString FilterText;
			FString ClassPathFilter;
			TArray<FString> RequestedPaths;
			bool bDryRun = true;
			bool bConfirmClearAll = false;
			bool bClearSelection = true;
			Arguments.TryGetStringField(TEXT("filter"), FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
			ActorToolTryGetStringArrayField(Arguments, TEXT("paths"), RequestedPaths);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("confirmClearAll"), bConfirmClearAll);
			Arguments.TryGetBoolField(TEXT("clearSelection"), bClearSelection);
			const int32 MaxPasses = FMath::Clamp(ActorToolGetPositiveIntArgument(Arguments, TEXT("maxPasses"), 3), 1, 10);
			const int32 Limit = FMath::Clamp(ActorToolGetPositiveIntArgument(Arguments, TEXT("limit"), 10000), 1, 10000);

			TSet<FString> ExplicitPaths;
			for (const FString& RequestedPath : RequestedPaths)
			{
				const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
				if (!TrimmedPath.IsEmpty())
				{
					ExplicitPaths.Add(TrimmedPath);
				}
			}

			const bool bHasSelectors = !FilterText.TrimStartAndEnd().IsEmpty()
				|| !ClassPathFilter.TrimStartAndEnd().IsEmpty()
				|| ExplicitPaths.Num() > 0;

			struct FCollectedTargets
			{
				TArray<AActor*> Actors;
				int32 MatchCount = 0;
				bool bTruncated = false;
			};

			auto CollectTargetActors = [&]()
			{
				TArray<AActor*> Targets;
				TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();
				AllActors.Sort([](const AActor& A, const AActor& B)
				{
					return A.GetActorLabel() < B.GetActorLabel();
				});

				int32 MatchCount = 0;
				for (AActor* Actor : AllActors)
				{
					if (!Actor)
					{
						continue;
					}

					if (bHasSelectors && !ActorToolMatchesActorFilters(Actor, FilterText, ClassPathFilter, ExplicitPaths))
					{
						continue;
					}

					++MatchCount;
					if (Targets.Num() < Limit)
					{
						Targets.Add(Actor);
					}
				}

				FCollectedTargets Result;
				Result.Actors = MoveTemp(Targets);
				Result.MatchCount = MatchCount;
				Result.bTruncated = MatchCount > Result.Actors.Num();
				return Result;
			};

			TArray<TSharedPtr<FJsonValue>> CandidateActors;
			TArray<TSharedPtr<FJsonValue>> Passes;
			int32 PassCount = 0;
			int32 RequestedDestroyCount = 0;
			bool bAnyPassFailed = false;
			bool bAnyPassTruncated = false;

			auto FirstTargets = CollectTargetActors();
			for (AActor* Actor : FirstTargets.Actors)
			{
				CandidateActors.Add(MakeShared<FJsonValueObject>(ActorToolMakeActorObject(Actor)));
			}

			if (bDryRun)
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("clear_level_environment"));
				StructuredContent->SetBoolField(TEXT("dryRun"), true);
				StructuredContent->SetBoolField(TEXT("confirmClearAll"), bConfirmClearAll);
				StructuredContent->SetStringField(TEXT("filter"), FilterText);
				StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
				StructuredContent->SetArrayField(TEXT("paths"), ActorToolMakeJsonStringArray(RequestedPaths));
				StructuredContent->SetBoolField(TEXT("hasSelectors"), bHasSelectors);
				StructuredContent->SetNumberField(TEXT("candidateCount"), FirstTargets.MatchCount);
				StructuredContent->SetNumberField(TEXT("returnedCandidateCount"), FirstTargets.Actors.Num());
				StructuredContent->SetBoolField(TEXT("truncated"), FirstTargets.bTruncated);
				StructuredContent->SetArrayField(TEXT("candidates"), CandidateActors);

				return ActorToolMakeExecutionResult(
					FString::Printf(TEXT("Dry run: would clear %d level actors%s."), FirstTargets.MatchCount, FirstTargets.bTruncated ? TEXT(" (candidate list truncated)") : TEXT("")),
					StructuredContent,
					false);
			}

			if (!bHasSelectors && !bConfirmClearAll)
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("clear_level_environment"));
				StructuredContent->SetBoolField(TEXT("dryRun"), false);
				StructuredContent->SetBoolField(TEXT("confirmClearAll"), false);
				StructuredContent->SetBoolField(TEXT("hasSelectors"), false);
				StructuredContent->SetNumberField(TEXT("candidateCount"), FirstTargets.MatchCount);
				StructuredContent->SetArrayField(TEXT("candidates"), CandidateActors);
				return ActorToolMakeExecutionResult(
					TEXT("Refusing to clear every level actor without confirmClearAll=true. Run with dryRun=true first, or provide filter/classPath/paths."),
					StructuredContent,
					true);
			}

			if (bClearSelection)
			{
				EditorActorSubsystem->SelectNothing();
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "UnrealMcpClearLevelEnvironment", "Unreal MCP Clear Level Environment"));
			for (int32 PassIndex = 0; PassIndex < MaxPasses; ++PassIndex)
			{
				FCollectedTargets Targets = PassIndex == 0 ? FirstTargets : CollectTargetActors();
				if (Targets.Actors.Num() == 0)
				{
					break;
				}

				++PassCount;
				RequestedDestroyCount += Targets.Actors.Num();
				bAnyPassTruncated = bAnyPassTruncated || Targets.bTruncated;

				for (AActor* Actor : Targets.Actors)
				{
					if (Actor)
					{
						Actor->Modify();
					}
				}

				const bool bDestroyed = EditorActorSubsystem->DestroyActors(Targets.Actors);
				bAnyPassFailed = bAnyPassFailed || !bDestroyed;

				TSharedPtr<FJsonObject> PassObject = MakeShared<FJsonObject>();
				PassObject->SetNumberField(TEXT("pass"), PassIndex + 1);
				PassObject->SetNumberField(TEXT("matchCount"), Targets.MatchCount);
				PassObject->SetNumberField(TEXT("requestedDestroyCount"), Targets.Actors.Num());
				PassObject->SetBoolField(TEXT("truncated"), Targets.bTruncated);
				PassObject->SetBoolField(TEXT("destroyed"), bDestroyed);
				Passes.Add(MakeShared<FJsonValueObject>(PassObject));

				if (Targets.bTruncated)
				{
					break;
				}
			}

			if (bClearSelection)
			{
				EditorActorSubsystem->SelectNothing();
			}

			auto RemainingTargets = CollectTargetActors();
			const int32 RemainingLevelActorCount = EditorActorSubsystem->GetAllLevelActors().Num();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("clear_level_environment"));
			StructuredContent->SetBoolField(TEXT("dryRun"), false);
			StructuredContent->SetBoolField(TEXT("confirmClearAll"), bConfirmClearAll);
			StructuredContent->SetStringField(TEXT("filter"), FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
			StructuredContent->SetArrayField(TEXT("paths"), ActorToolMakeJsonStringArray(RequestedPaths));
			StructuredContent->SetBoolField(TEXT("hasSelectors"), bHasSelectors);
			StructuredContent->SetNumberField(TEXT("maxPasses"), MaxPasses);
			StructuredContent->SetNumberField(TEXT("passCount"), PassCount);
			StructuredContent->SetNumberField(TEXT("requestedDestroyCount"), RequestedDestroyCount);
			StructuredContent->SetNumberField(TEXT("initialCandidateCount"), FirstTargets.MatchCount);
			StructuredContent->SetNumberField(TEXT("remainingTargetCount"), RemainingTargets.MatchCount);
			StructuredContent->SetNumberField(TEXT("remainingLevelActorCount"), RemainingLevelActorCount);
			StructuredContent->SetBoolField(TEXT("truncated"), bAnyPassTruncated || RemainingTargets.bTruncated);
			StructuredContent->SetBoolField(TEXT("allPassesDestroyed"), !bAnyPassFailed);
			StructuredContent->SetArrayField(TEXT("initialCandidates"), CandidateActors);
			StructuredContent->SetArrayField(TEXT("passes"), Passes);

			const bool bHasRemainingTargets = RemainingTargets.MatchCount > 0;
			const bool bIsError = bAnyPassFailed || bHasRemainingTargets || bAnyPassTruncated;
			FString Text = FString::Printf(
				TEXT("Clear level environment: passes=%d requestedDestroy=%d remainingTargets=%d remainingLevelActors=%d."),
				PassCount,
				RequestedDestroyCount,
				RemainingTargets.MatchCount,
				RemainingLevelActorCount);
			if (bAnyPassTruncated)
			{
				Text += TEXT(" Operation was truncated by limit; rerun with a higher limit if needed.");
			}
			if (bHasRemainingTargets)
			{
				Text += TEXT(" Some target actors remain.");
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, bIsError);
		}

		FUnrealMcpExecutionResult ExecuteBatchSetActorScale(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			double ScaleX = 1.0;
			double ScaleY = 1.0;
			double ScaleZ = 1.0;
			const bool bHasScaleX = Arguments.TryGetNumberField(TEXT("scaleX"), ScaleX);
			const bool bHasScaleY = Arguments.TryGetNumberField(TEXT("scaleY"), ScaleY);
			const bool bHasScaleZ = Arguments.TryGetNumberField(TEXT("scaleZ"), ScaleZ);
			if (!bHasScaleX && !bHasScaleY && !bHasScaleZ)
			{
				return ActorToolMakeExecutionResult(TEXT("Provide at least one of scaleX, scaleY, or scaleZ."), nullptr, true);
			}

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorScale", "Unreal MCP Batch Set Actor Scale"));

			int32 SuccessCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				const FVector BeforeScale = Actor->GetActorScale3D();
				const FVector RequestedScale(
					bHasScaleX ? ScaleX : BeforeScale.X,
					bHasScaleY ? ScaleY : BeforeScale.Y,
					bHasScaleZ ? ScaleZ : BeforeScale.Z);

				Actor->Modify();
				Actor->SetActorScale3D(RequestedScale);
				Actor->MarkPackageDirty();

				const FVector AfterScale = Actor->GetActorScale3D();
				const bool bSucceeded = AfterScale.Equals(RequestedScale, KINDA_SMALL_NUMBER);
				if (bSucceeded)
				{
					++SuccessCount;
				}

				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetObjectField(TEXT("beforeScale"), ActorToolMakeVectorObject(BeforeScale));
				ActorObject->SetObjectField(TEXT("afterScale"), ActorToolMakeVectorObject(AfterScale));
				ActorObject->SetBoolField(TEXT("success"), bSucceeded);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(
					TEXT("%s -> scale [%.2f, %.2f, %.2f] success=%s"),
					*Actor->GetActorLabel(),
					AfterScale.X,
					AfterScale.Y,
					AfterScale.Z,
					bSucceeded ? TEXT("true") : TEXT("false")));
			}

			const int32 FailureCount = Query.Actors.Num() - SuccessCount;

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Updated scale on %d actors. success=%d failure=%d"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteBatchSetActorTags(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			TArray<FString> TagStrings;
			ActorToolTryGetStringArrayField(Arguments, TEXT("tags"), TagStrings);
			TArray<FName> Tags;
			for (const FString& TagString : TagStrings)
			{
				const FString TrimmedTag = TagString.TrimStartAndEnd();
				if (!TrimmedTag.IsEmpty())
				{
					Tags.Add(FName(*TrimmedTag));
				}
			}

			if (Tags.Num() == 0)
			{
				return ActorToolMakeExecutionResult(TEXT("The tags argument must contain at least one non-empty tag."), nullptr, true);
			}

			bool bReplaceExisting = false;
			Arguments.TryGetBoolField(TEXT("replaceExisting"), bReplaceExisting);

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorTags", "Unreal MCP Batch Set Actor Tags"));

			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				TArray<TSharedPtr<FJsonValue>> BeforeTags;
				for (const FName& ExistingTag : Actor->Tags)
				{
					BeforeTags.Add(MakeShared<FJsonValueString>(ExistingTag.ToString()));
				}

				Actor->Modify();
				if (bReplaceExisting)
				{
					Actor->Tags.Reset();
				}

				for (const FName& Tag : Tags)
				{
					Actor->Tags.AddUnique(Tag);
				}
				Actor->MarkPackageDirty();

				TArray<TSharedPtr<FJsonValue>> AfterTags;
				for (const FName& ExistingTag : Actor->Tags)
				{
					AfterTags.Add(MakeShared<FJsonValueString>(ExistingTag.ToString()));
				}

				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetArrayField(TEXT("beforeTags"), BeforeTags);
				ActorObject->SetArrayField(TEXT("afterTags"), AfterTags);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TArray<FString> TagLineParts;
				for (const FName& ExistingTag : Actor->Tags)
				{
					TagLineParts.Add(ExistingTag.ToString());
				}
				TextLines.Add(FString::Printf(TEXT("%s -> [%s]"), *Actor->GetActorLabel(), *FString::Join(TagLineParts, TEXT(", "))));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetBoolField(TEXT("replaceExisting"), bReplaceExisting);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("%s tags on %d actors."),
				bReplaceExisting ? TEXT("Replaced") : TEXT("Updated"),
				Query.Actors.Num());
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteBatchSetPointLightProperties(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			double Intensity = 0.0;
			double AttenuationRadius = 0.0;
			double SourceRadius = 0.0;
			double SoftSourceRadius = 0.0;
			double Temperature = 6500.0;
			bool bUseTemperature = false;
			bool bCastShadows = true;
			bool bVisible = true;
			const bool bHasIntensity = Arguments.TryGetNumberField(TEXT("intensity"), Intensity);
			const bool bHasAttenuationRadius = Arguments.TryGetNumberField(TEXT("attenuationRadius"), AttenuationRadius);
			const bool bHasSourceRadius = Arguments.TryGetNumberField(TEXT("sourceRadius"), SourceRadius);
			const bool bHasSoftSourceRadius = Arguments.TryGetNumberField(TEXT("softSourceRadius"), SoftSourceRadius);
			const bool bHasTemperature = Arguments.TryGetNumberField(TEXT("temperature"), Temperature);
			const bool bHasUseTemperature = Arguments.TryGetBoolField(TEXT("useTemperature"), bUseTemperature);
			const bool bHasCastShadows = Arguments.TryGetBoolField(TEXT("castShadows"), bCastShadows);
			const bool bHasVisible = Arguments.TryGetBoolField(TEXT("visible"), bVisible);
			if (!bHasIntensity
				&& !bHasAttenuationRadius
				&& !bHasSourceRadius
				&& !bHasSoftSourceRadius
				&& !bHasTemperature
				&& !bHasUseTemperature
				&& !bHasCastShadows
				&& !bHasVisible)
			{
				return ActorToolMakeExecutionResult(
					TEXT("Provide at least one point light property such as intensity, attenuationRadius, sourceRadius, temperature, castShadows, or visible."),
					nullptr,
					true);
			}

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetPointLightProperties", "Unreal MCP Batch Set Point Light Properties"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				UPointLightComponent* PointLightComponent = Actor ? Actor->FindComponentByClass<UPointLightComponent>() : nullptr;
				if (!PointLightComponent)
				{
					++FailureCount;
					TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
					ActorObject->SetBoolField(TEXT("success"), false);
					ActorObject->SetStringField(TEXT("error"), TEXT("No PointLightComponent was found on this actor."));
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));
					TextLines.Add(FString::Printf(TEXT("%s: missing PointLightComponent"), *Actor->GetActorLabel()));
					continue;
				}

				Actor->Modify();
				PointLightComponent->Modify();

				if (bHasIntensity)
				{
					PointLightComponent->SetIntensity(Intensity);
				}
				if (bHasAttenuationRadius)
				{
					PointLightComponent->SetAttenuationRadius(AttenuationRadius);
				}
				if (bHasSourceRadius)
				{
					PointLightComponent->SetSourceRadius(SourceRadius);
				}
				if (bHasSoftSourceRadius)
				{
					PointLightComponent->SetSoftSourceRadius(SoftSourceRadius);
				}
				if (bHasUseTemperature)
				{
					PointLightComponent->SetUseTemperature(bUseTemperature);
				}
				if (bHasTemperature)
				{
					PointLightComponent->SetTemperature(Temperature);
				}
				if (bHasCastShadows)
				{
					PointLightComponent->SetCastShadows(bCastShadows);
				}
				if (bHasVisible)
				{
					PointLightComponent->SetVisibility(bVisible);
				}

				PointLightComponent->MarkPackageDirty();
				Actor->MarkPackageDirty();

				++SuccessCount;
				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetBoolField(TEXT("success"), true);
				if (bHasIntensity)
				{
					ActorObject->SetNumberField(TEXT("intensity"), Intensity);
				}
				if (bHasAttenuationRadius)
				{
					ActorObject->SetNumberField(TEXT("attenuationRadius"), AttenuationRadius);
				}
				if (bHasSourceRadius)
				{
					ActorObject->SetNumberField(TEXT("sourceRadius"), SourceRadius);
				}
				if (bHasSoftSourceRadius)
				{
					ActorObject->SetNumberField(TEXT("softSourceRadius"), SoftSourceRadius);
				}
				if (bHasUseTemperature)
				{
					ActorObject->SetBoolField(TEXT("useTemperature"), bUseTemperature);
				}
				if (bHasTemperature)
				{
					ActorObject->SetNumberField(TEXT("temperature"), Temperature);
				}
				if (bHasCastShadows)
				{
					ActorObject->SetBoolField(TEXT("castShadows"), bCastShadows);
				}
				if (bHasVisible)
				{
					ActorObject->SetBoolField(TEXT("visible"), bVisible);
				}
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(TEXT("%s: updated point light properties"), *Actor->GetActorLabel()));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Updated point light properties on %d actors. success=%d failure=%d"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteBatchConfigureStaticMeshActors(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			UEditorAssetSubsystem* EditorAssetSubsystem = ActorToolGetEditorAssetSubsystem();
			if (!EditorActorSubsystem || !EditorAssetSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
			}

			FString StaticMeshPath;
			FString MaterialPath;
			Arguments.TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath);
			Arguments.TryGetStringField(TEXT("materialPath"), MaterialPath);
			if (StaticMeshPath.TrimStartAndEnd().IsEmpty() && MaterialPath.TrimStartAndEnd().IsEmpty())
			{
				return ActorToolMakeExecutionResult(TEXT("Provide at least one of staticMeshPath or materialPath."), nullptr, true);
			}

			double MaterialSlotValue = 0.0;
			Arguments.TryGetNumberField(TEXT("materialSlot"), MaterialSlotValue);
			const int32 MaterialSlot = FMath::Max(0, static_cast<int32>(MaterialSlotValue));

			UStaticMesh* StaticMeshAsset = nullptr;
			FString MeshObjectPath;
			FString FailureReason;
			if (!StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				UObject* LoadedMeshAsset = ActorToolLoadAssetFromAnyPath(EditorAssetSubsystem, StaticMeshPath, MeshObjectPath, FailureReason);
				StaticMeshAsset = Cast<UStaticMesh>(LoadedMeshAsset);
				if (!StaticMeshAsset)
				{
					return ActorToolMakeExecutionResult(
						LoadedMeshAsset
							? FString::Printf(TEXT("Asset '%s' is not a StaticMesh."), *MeshObjectPath)
							: FailureReason,
						nullptr,
						true);
				}
			}

			UMaterialInterface* MaterialAsset = nullptr;
			FString MaterialObjectPath;
			if (!MaterialPath.TrimStartAndEnd().IsEmpty())
			{
				UObject* LoadedMaterialAsset = ActorToolLoadAssetFromAnyPath(EditorAssetSubsystem, MaterialPath, MaterialObjectPath, FailureReason);
				MaterialAsset = Cast<UMaterialInterface>(LoadedMaterialAsset);
				if (!MaterialAsset)
				{
					return ActorToolMakeExecutionResult(
						LoadedMaterialAsset
							? FString::Printf(TEXT("Asset '%s' is not a material or material instance."), *MaterialObjectPath)
							: FailureReason,
						nullptr,
						true);
				}
			}

			FActorToolQueryResult Query;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchConfigureStaticMeshActors", "Unreal MCP Batch Configure Static Mesh Actors"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				UStaticMeshComponent* StaticMeshComponent = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
				if (!StaticMeshComponent)
				{
					++FailureCount;
					TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
					ActorObject->SetBoolField(TEXT("success"), false);
					ActorObject->SetStringField(TEXT("error"), TEXT("No StaticMeshComponent was found on this actor."));
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));
					TextLines.Add(FString::Printf(TEXT("%s: missing StaticMeshComponent"), *Actor->GetActorLabel()));
					continue;
				}

				Actor->Modify();
				StaticMeshComponent->Modify();
				if (StaticMeshAsset)
				{
					StaticMeshComponent->SetStaticMesh(StaticMeshAsset);
				}
				if (MaterialAsset)
				{
					StaticMeshComponent->SetMaterial(MaterialSlot, MaterialAsset);
				}
				StaticMeshComponent->MarkPackageDirty();
				Actor->MarkPackageDirty();

				++SuccessCount;
				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetBoolField(TEXT("success"), true);
				if (StaticMeshAsset)
				{
					ActorObject->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
				}
				if (MaterialAsset)
				{
					ActorObject->SetStringField(TEXT("materialPath"), MaterialObjectPath);
					ActorObject->SetNumberField(TEXT("materialSlot"), MaterialSlot);
				}
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(TEXT("%s: updated static mesh configuration"), *Actor->GetActorLabel()));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			if (StaticMeshAsset)
			{
				StructuredContent->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
			}
			if (MaterialAsset)
			{
				StructuredContent->SetStringField(TEXT("materialPath"), MaterialObjectPath);
				StructuredContent->SetNumberField(TEXT("materialSlot"), MaterialSlot);
			}
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Configured static mesh actors. success=%d failure=%d"),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteLayoutActorsGrid(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			const int32 Columns = FMath::Max(1, ActorToolGetPositiveIntArgument(Arguments, TEXT("columns"), 5));

			double SpacingX = 300.0;
			double SpacingY = 300.0;
			double SpacingZ = 0.0;
			Arguments.TryGetNumberField(TEXT("spacingX"), SpacingX);
			Arguments.TryGetNumberField(TEXT("spacingY"), SpacingY);
			Arguments.TryGetNumberField(TEXT("spacingZ"), SpacingZ);

			FVector Origin = Query.Actors[0]->GetActorLocation();
			double NumericValue = 0.0;
			if (Arguments.TryGetNumberField(TEXT("startX"), NumericValue))
			{
				Origin.X = NumericValue;
			}
			if (Arguments.TryGetNumberField(TEXT("startY"), NumericValue))
			{
				Origin.Y = NumericValue;
			}
			if (Arguments.TryGetNumberField(TEXT("startZ"), NumericValue))
			{
				Origin.Z = NumericValue;
			}

			bool bHasPitch = false;
			bool bHasYaw = false;
			bool bHasRoll = false;
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			bHasPitch = Arguments.TryGetNumberField(TEXT("pitch"), Pitch);
			bHasYaw = Arguments.TryGetNumberField(TEXT("yaw"), Yaw);
			bHasRoll = Arguments.TryGetNumberField(TEXT("roll"), Roll);

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "LayoutActorsGrid", "Unreal MCP Layout Actors Grid"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (int32 ActorIndex = 0; ActorIndex < Query.Actors.Num(); ++ActorIndex)
			{
				AActor* Actor = Query.Actors[ActorIndex];
				const FVector BeforeLocation = Actor->GetActorLocation();
				const FRotator BeforeRotation = Actor->GetActorRotation();

				const int32 ColumnIndex = ActorIndex % Columns;
				const int32 RowIndex = ActorIndex / Columns;

				const FVector NewLocation(
					Origin.X + static_cast<double>(ColumnIndex) * SpacingX,
					Origin.Y + static_cast<double>(RowIndex) * SpacingY,
					Origin.Z + static_cast<double>(RowIndex) * SpacingZ);

				FRotator NewRotation = BeforeRotation;
				if (bHasPitch)
				{
					NewRotation.Pitch = Pitch;
				}
				if (bHasYaw)
				{
					NewRotation.Yaw = Yaw;
				}
				if (bHasRoll)
				{
					NewRotation.Roll = Roll;
				}

				const bool bMoved = EditorActorSubsystem->SetActorTransform(
					Actor,
					FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetNumberField(TEXT("index"), ActorIndex);
				ActorObject->SetObjectField(TEXT("beforeLocation"), ActorToolMakeVectorObject(BeforeLocation));
				ActorObject->SetObjectField(TEXT("beforeRotation"), ActorToolMakeRotatorObject(BeforeRotation));
				ActorObject->SetObjectField(TEXT("afterLocation"), ActorToolMakeVectorObject(Actor->GetActorLocation()));
				ActorObject->SetObjectField(TEXT("afterRotation"), ActorToolMakeRotatorObject(Actor->GetActorRotation()));
				ActorObject->SetBoolField(TEXT("success"), bMoved);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				if (bMoved)
				{
					++SuccessCount;
				}
				else
				{
					++FailureCount;
				}

				TextLines.Add(FString::Printf(
					TEXT("%s -> [%.1f, %.1f, %.1f] success=%s"),
					*Actor->GetActorLabel(),
					Actor->GetActorLocation().X,
					Actor->GetActorLocation().Y,
					Actor->GetActorLocation().Z,
					bMoved ? TEXT("true") : TEXT("false")));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
			StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("columns"), Columns);
			StructuredContent->SetNumberField(TEXT("spacingX"), SpacingX);
			StructuredContent->SetNumberField(TEXT("spacingY"), SpacingY);
			StructuredContent->SetNumberField(TEXT("spacingZ"), SpacingZ);
			StructuredContent->SetObjectField(TEXT("origin"), ActorToolMakeVectorObject(Origin));
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Laid out %d actors into a grid. success=%d failure=%d columns=%d"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount,
				Columns);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteLayoutActorsCircle(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			double Radius = 1000.0;
			double StartAngleDegrees = 0.0;
			double ArcDegrees = 360.0;
			double YawOffset = 0.0;
			bool bAlignYawToCenter = false;
			Arguments.TryGetNumberField(TEXT("radius"), Radius);
			Arguments.TryGetNumberField(TEXT("startAngleDegrees"), StartAngleDegrees);
			Arguments.TryGetNumberField(TEXT("arcDegrees"), ArcDegrees);
			Arguments.TryGetNumberField(TEXT("yawOffset"), YawOffset);
			Arguments.TryGetBoolField(TEXT("alignYawToCenter"), bAlignYawToCenter);

			if (Radius <= 0.0)
			{
				return ActorToolMakeExecutionResult(TEXT("radius must be greater than zero."), nullptr, true);
			}

			FVector Center = Query.Actors[0]->GetActorLocation();
			double NumericValue = 0.0;
			const bool bHasCenterX = Arguments.TryGetNumberField(TEXT("centerX"), NumericValue);
			if (bHasCenterX)
			{
				Center.X = NumericValue;
			}
			const bool bHasCenterY = Arguments.TryGetNumberField(TEXT("centerY"), NumericValue);
			if (bHasCenterY)
			{
				Center.Y = NumericValue;
			}
			const bool bHasCenterZ = Arguments.TryGetNumberField(TEXT("centerZ"), NumericValue);
			if (bHasCenterZ)
			{
				Center.Z = NumericValue;
			}

			const int32 ActorCount = Query.Actors.Num();
			const double NormalizedArc = FMath::Fmod(FMath::Abs(ArcDegrees), 360.0);
			const bool bFullCircle = FMath::IsNearlyZero(NormalizedArc, KINDA_SMALL_NUMBER);
			const double StepDenominator = ActorCount <= 1
				? 1.0
				: (bFullCircle ? static_cast<double>(ActorCount) : static_cast<double>(ActorCount - 1));

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "LayoutActorsCircle", "Unreal MCP Layout Actors Circle"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (int32 ActorIndex = 0; ActorIndex < ActorCount; ++ActorIndex)
			{
				AActor* Actor = Query.Actors[ActorIndex];
				const FVector BeforeLocation = Actor->GetActorLocation();
				const FRotator BeforeRotation = Actor->GetActorRotation();

				const double Alpha = ActorCount <= 1 ? 0.0 : static_cast<double>(ActorIndex) / StepDenominator;
				const double AngleDegrees = StartAngleDegrees + (ArcDegrees * Alpha);
				const double AngleRadians = FMath::DegreesToRadians(AngleDegrees);

				const FVector NewLocation(
					Center.X + FMath::Cos(AngleRadians) * Radius,
					Center.Y + FMath::Sin(AngleRadians) * Radius,
					bHasCenterZ ? Center.Z : BeforeLocation.Z);

				FRotator NewRotation = BeforeRotation;
				if (bAlignYawToCenter)
				{
					const FVector DirectionToCenter = Center - NewLocation;
					NewRotation.Yaw = FMath::RadiansToDegrees(FMath::Atan2(DirectionToCenter.Y, DirectionToCenter.X)) + YawOffset;
				}

				const bool bMoved = EditorActorSubsystem->SetActorTransform(
					Actor,
					FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetNumberField(TEXT("index"), ActorIndex);
				ActorObject->SetNumberField(TEXT("angleDegrees"), AngleDegrees);
				ActorObject->SetObjectField(TEXT("beforeLocation"), ActorToolMakeVectorObject(BeforeLocation));
				ActorObject->SetObjectField(TEXT("beforeRotation"), ActorToolMakeRotatorObject(BeforeRotation));
				ActorObject->SetObjectField(TEXT("afterLocation"), ActorToolMakeVectorObject(Actor->GetActorLocation()));
				ActorObject->SetObjectField(TEXT("afterRotation"), ActorToolMakeRotatorObject(Actor->GetActorRotation()));
				ActorObject->SetBoolField(TEXT("success"), bMoved);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				if (bMoved)
				{
					++SuccessCount;
				}
				else
				{
					++FailureCount;
				}

				TextLines.Add(FString::Printf(
					TEXT("%s -> [%.1f, %.1f, %.1f] angle=%.2f success=%s"),
					*Actor->GetActorLabel(),
					Actor->GetActorLocation().X,
					Actor->GetActorLocation().Y,
					Actor->GetActorLocation().Z,
					AngleDegrees,
					bMoved ? TEXT("true") : TEXT("false")));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
			StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("radius"), Radius);
			StructuredContent->SetNumberField(TEXT("startAngleDegrees"), StartAngleDegrees);
			StructuredContent->SetNumberField(TEXT("arcDegrees"), ArcDegrees);
			StructuredContent->SetObjectField(TEXT("center"), ActorToolMakeVectorObject(Center));
			StructuredContent->SetBoolField(TEXT("alignYawToCenter"), bAlignYawToCenter);
			StructuredContent->SetNumberField(TEXT("yawOffset"), YawOffset);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Laid out %d actors into a circle/arc. success=%d failure=%d radius=%.1f arc=%.1f"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount,
				Radius,
				ArcDegrees);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteSpawnActor(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			UEditorAssetSubsystem* EditorAssetSubsystem = ActorToolGetEditorAssetSubsystem();
			if (!EditorActorSubsystem || !EditorAssetSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
			}

			FString ClassPath;
			if (!Arguments.TryGetStringField(TEXT("classPath"), ClassPath) || ClassPath.IsEmpty())
			{
				return ActorToolMakeExecutionResult(TEXT("The classPath argument is required."), nullptr, true);
			}

			UClass* ResolvedClass = ResolveClassPath(ClassPath, EditorAssetSubsystem);
			if (!ResolvedClass)
			{
				return ActorToolMakeExecutionResult(FString::Printf(TEXT("Unable to resolve class '%s'."), *ClassPath), nullptr, true);
			}

			if (!ResolvedClass->IsChildOf(AActor::StaticClass()))
			{
				return ActorToolMakeExecutionResult(FString::Printf(TEXT("Class '%s' is not an Actor class."), *ResolvedClass->GetPathName()), nullptr, true);
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			double ScaleX = 1.0;
			double ScaleY = 1.0;
			double ScaleZ = 1.0;
			bool bHasScaleX = false;
			bool bHasScaleY = false;
			bool bHasScaleZ = false;
			FString Label;

			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);
			Arguments.TryGetNumberField(TEXT("z"), Z);
			Arguments.TryGetNumberField(TEXT("pitch"), Pitch);
			Arguments.TryGetNumberField(TEXT("yaw"), Yaw);
			Arguments.TryGetNumberField(TEXT("roll"), Roll);
			bHasScaleX = Arguments.TryGetNumberField(TEXT("sx"), ScaleX);
			bHasScaleY = Arguments.TryGetNumberField(TEXT("sy"), ScaleY);
			bHasScaleZ = Arguments.TryGetNumberField(TEXT("sz"), ScaleZ);
			Arguments.TryGetStringField(TEXT("label"), Label);

			AActor* NewActor = EditorActorSubsystem->SpawnActorFromClass(
				ResolvedClass,
				FVector(X, Y, Z),
				FRotator(Pitch, Yaw, Roll),
				false);

			if (!NewActor)
			{
				return ActorToolMakeExecutionResult(FString::Printf(TEXT("Failed to spawn actor from class '%s'."), *ResolvedClass->GetPathName()), nullptr, true);
			}

			if (!Label.IsEmpty())
			{
				FActorLabelUtilities::SetActorLabelUnique(NewActor, Label);
			}

			if (bHasScaleX || bHasScaleY || bHasScaleZ)
			{
				const FVector CurrentScale = NewActor->GetActorScale3D();
				NewActor->SetActorScale3D(FVector(
					bHasScaleX ? ScaleX : CurrentScale.X,
					bHasScaleY ? ScaleY : CurrentScale.Y,
					bHasScaleZ ? ScaleZ : CurrentScale.Z));
			}

			TSharedPtr<FJsonObject> StructuredContent = ActorToolMakeActorObject(NewActor);
			StructuredContent->SetObjectField(TEXT("scale"), ActorToolMakeVectorObject(NewActor->GetActorScale3D()));

			const TSharedPtr<FJsonObject>* PropertyValuesObject = nullptr;
			int32 PropertySuccessCount = 0;
			int32 PropertyFailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> PropertyEdits;
			if (Arguments.TryGetObjectField(TEXT("properties"), PropertyValuesObject) && PropertyValuesObject && (*PropertyValuesObject)->Values.Num() > 0)
			{
				ApplyPropertyMapToActor(NewActor, **PropertyValuesObject, PropertyEdits, PropertySuccessCount, PropertyFailureCount);
				StructuredContent->SetNumberField(TEXT("propertySuccessCount"), PropertySuccessCount);
				StructuredContent->SetNumberField(TEXT("propertyFailureCount"), PropertyFailureCount);
				StructuredContent->SetArrayField(TEXT("propertyEdits"), PropertyEdits);
			}

			FString Text = FString::Printf(TEXT("Spawned actor %s."), *NewActor->GetActorLabel());
			if (PropertySuccessCount > 0 || PropertyFailureCount > 0)
			{
				Text += FString::Printf(
					TEXT(" propertySuccess=%d propertyFailure=%d"),
					PropertySuccessCount,
					PropertyFailureCount);
			}

			return ActorToolMakeExecutionResult(
				Text,
				StructuredContent,
				PropertyFailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteSpawnActorBatch(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			TArray<TSharedPtr<FJsonObject>> SpawnItems;
			if (!ActorToolTryGetObjectArrayField(Arguments, TEXT("items"), SpawnItems) || SpawnItems.Num() == 0)
			{
				return ActorToolMakeExecutionResult(TEXT("The items argument must be a non-empty array of objects."), nullptr, true);
			}

			FString DefaultClassPath;
			bool bSelectSpawned = true;
			Arguments.TryGetStringField(TEXT("classPath"), DefaultClassPath);
			Arguments.TryGetBoolField(TEXT("selectSpawned"), bSelectSpawned);

			TArray<TSharedPtr<FJsonValue>> ResultItems;
			TArray<AActor*> SpawnedActors;
			TArray<FString> TextLines;
			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			int32 PropertyFailureCount = 0;

			for (int32 ItemIndex = 0; ItemIndex < SpawnItems.Num(); ++ItemIndex)
			{
				TSharedPtr<FJsonObject> MergedArguments = MakeShared<FJsonObject>();
				MergedArguments->Values = SpawnItems[ItemIndex]->Values;

				if (!MergedArguments->HasField(TEXT("classPath")) && !DefaultClassPath.IsEmpty())
				{
					MergedArguments->SetStringField(TEXT("classPath"), DefaultClassPath);
				}

				FUnrealMcpExecutionResult SpawnResult = ExecuteSpawnActor(TEXT("unreal.spawn_actor"), *MergedArguments);

				TSharedPtr<FJsonObject> ItemResult = SpawnResult.StructuredContent.IsValid()
					? MakeShared<FJsonObject>(*SpawnResult.StructuredContent)
					: MakeShared<FJsonObject>();
				bool bSpawnedActor = false;

				if (SpawnResult.StructuredContent.IsValid())
				{
					double ItemPropertyFailureCount = 0.0;
					if (SpawnResult.StructuredContent->TryGetNumberField(TEXT("propertyFailureCount"), ItemPropertyFailureCount))
					{
						PropertyFailureCount += static_cast<int32>(ItemPropertyFailureCount);
					}

					FString ActorPath;
					if (SpawnResult.StructuredContent->TryGetStringField(TEXT("path"), ActorPath))
					{
						bSpawnedActor = true;

						FString ResolveFailureReason;
						if (AActor* SpawnedActor = ActorToolResolveActorReference(EditorActorSubsystem, ActorPath, FString(), ResolveFailureReason))
						{
							SpawnedActors.Add(SpawnedActor);
						}
					}
				}

				ItemResult->SetNumberField(TEXT("index"), ItemIndex);
				ItemResult->SetBoolField(TEXT("success"), !SpawnResult.bIsError);
				ItemResult->SetBoolField(TEXT("spawned"), bSpawnedActor);
				ItemResult->SetBoolField(TEXT("toolError"), SpawnResult.bIsError);
				ItemResult->SetStringField(TEXT("text"), SpawnResult.Text);
				ResultItems.Add(MakeShared<FJsonValueObject>(ItemResult));

				if (bSpawnedActor)
				{
					++SuccessCount;
				}
				else
				{
					++FailureCount;
				}

				TextLines.Add(FString::Printf(TEXT("[%d] %s"), ItemIndex, *SpawnResult.Text));
			}

			if (bSelectSpawned)
			{
				EditorActorSubsystem->SelectNothing();
				for (AActor* SpawnedActor : SpawnedActors)
				{
					EditorActorSubsystem->SetActorSelectionState(SpawnedActor, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("defaultClassPath"), DefaultClassPath);
			StructuredContent->SetBoolField(TEXT("selectSpawned"), bSelectSpawned);
			StructuredContent->SetNumberField(TEXT("requestedCount"), SpawnItems.Num());
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetNumberField(TEXT("propertyFailureCount"), PropertyFailureCount);
			StructuredContent->SetArrayField(TEXT("items"), ResultItems);

			FString Text = FString::Printf(
				TEXT("Spawned %d/%d actors in batch. failures=%d propertyFailures=%d"),
				SuccessCount,
				SpawnItems.Num(),
				FailureCount,
				PropertyFailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, FailureCount > 0 || PropertyFailureCount > 0);
		}

		FUnrealMcpExecutionResult ExecuteSpawnStaticMeshActor(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			UEditorAssetSubsystem* EditorAssetSubsystem = ActorToolGetEditorAssetSubsystem();
			if (!EditorActorSubsystem || !EditorAssetSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
			}

			FString StaticMeshPath;
			if (!Arguments.TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath) || StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				return ActorToolMakeExecutionResult(TEXT("The staticMeshPath argument is required."), nullptr, true);
			}

			FString MaterialPath;
			Arguments.TryGetStringField(TEXT("materialPath"), MaterialPath);
			double MaterialSlotValue = 0.0;
			Arguments.TryGetNumberField(TEXT("materialSlot"), MaterialSlotValue);
			const int32 MaterialSlot = FMath::Max(0, static_cast<int32>(MaterialSlotValue));

			FString MeshObjectPath;
			FString FailureReason;
			UObject* LoadedMeshAsset = ActorToolLoadAssetFromAnyPath(EditorAssetSubsystem, StaticMeshPath, MeshObjectPath, FailureReason);
			UStaticMesh* StaticMeshAsset = Cast<UStaticMesh>(LoadedMeshAsset);
			if (!StaticMeshAsset)
			{
				return ActorToolMakeExecutionResult(
					LoadedMeshAsset
						? FString::Printf(TEXT("Asset '%s' is not a StaticMesh."), *MeshObjectPath)
						: FailureReason,
					nullptr,
					true);
			}

			UMaterialInterface* MaterialAsset = nullptr;
			FString MaterialObjectPath;
			if (!MaterialPath.TrimStartAndEnd().IsEmpty())
			{
				UObject* LoadedMaterialAsset = ActorToolLoadAssetFromAnyPath(EditorAssetSubsystem, MaterialPath, MaterialObjectPath, FailureReason);
				MaterialAsset = Cast<UMaterialInterface>(LoadedMaterialAsset);
				if (!MaterialAsset)
				{
					return ActorToolMakeExecutionResult(
						LoadedMaterialAsset
							? FString::Printf(TEXT("Asset '%s' is not a material or material instance."), *MaterialObjectPath)
							: FailureReason,
						nullptr,
						true);
				}
			}

			TSharedPtr<FJsonObject> SpawnArguments = MakeShared<FJsonObject>();
			SpawnArguments->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("x"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("y"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("z"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("pitch"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("yaw"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("roll"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("sx"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("sy"), SpawnArguments);
			ActorToolCopyNumberFieldIfPresent(Arguments, TEXT("sz"), SpawnArguments);
			ActorToolCopyStringFieldIfPresent(Arguments, TEXT("label"), SpawnArguments);

			FUnrealMcpExecutionResult SpawnResult = ExecuteSpawnActor(TEXT("unreal.spawn_actor"), *SpawnArguments);
			if (SpawnResult.bIsError || !SpawnResult.StructuredContent.IsValid())
			{
				return SpawnResult;
			}

			FString SpawnedActorPath;
			if (!SpawnResult.StructuredContent->TryGetStringField(TEXT("path"), SpawnedActorPath) || SpawnedActorPath.IsEmpty())
			{
				return ActorToolMakeExecutionResult(TEXT("The spawned StaticMeshActor did not report a valid actor path."), nullptr, true);
			}

			AActor* SpawnedActor = ActorToolResolveActorReference(EditorActorSubsystem, SpawnedActorPath, FString(), FailureReason);
			if (!SpawnedActor)
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			UStaticMeshComponent* StaticMeshComponent = SpawnedActor->FindComponentByClass<UStaticMeshComponent>();
			if (!StaticMeshComponent)
			{
				return ActorToolMakeExecutionResult(
					FString::Printf(TEXT("Spawned actor '%s' does not have a StaticMeshComponent."), *SpawnedActor->GetActorLabel()),
					nullptr,
					true);
			}

			SpawnedActor->Modify();
			StaticMeshComponent->Modify();
			StaticMeshComponent->SetStaticMesh(StaticMeshAsset);
			if (MaterialAsset)
			{
				StaticMeshComponent->SetMaterial(MaterialSlot, MaterialAsset);
			}
			StaticMeshComponent->MarkPackageDirty();
			SpawnedActor->MarkPackageDirty();

			TSharedPtr<FJsonObject> StructuredContent = ActorToolMakeActorObject(SpawnedActor);
			StructuredContent->SetObjectField(TEXT("scale"), ActorToolMakeVectorObject(SpawnedActor->GetActorScale3D()));
			StructuredContent->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
			if (MaterialAsset)
			{
				StructuredContent->SetStringField(TEXT("materialPath"), MaterialObjectPath);
				StructuredContent->SetNumberField(TEXT("materialSlot"), MaterialSlot);
			}

			FString Text = FString::Printf(TEXT("Spawned StaticMeshActor %s with mesh %s."), *SpawnedActor->GetActorLabel(), *MeshObjectPath);
			if (MaterialAsset)
			{
				Text += FString::Printf(TEXT(" Applied material %s to slot %d."), *MaterialObjectPath, MaterialSlot);
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteBatchSetActorProperties(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UEditorActorSubsystem* EditorActorSubsystem = ActorToolGetEditorActorSubsystem();
			if (!EditorActorSubsystem)
			{
				return ActorToolMakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			const TSharedPtr<FJsonObject>* PropertyValuesObject = nullptr;
			if (!Arguments.TryGetObjectField(TEXT("properties"), PropertyValuesObject) || !PropertyValuesObject || (*PropertyValuesObject)->Values.Num() == 0)
			{
				return ActorToolMakeExecutionResult(TEXT("The properties argument must be a non-empty object."), nullptr, true);
			}

			FActorToolQueryResult Query;
			FString FailureReason;
			if (!ActorToolResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return ActorToolMakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorProperties", "Unreal MCP Batch Set Actor Properties"));

			int32 TotalSuccessCount = 0;
			int32 TotalFailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				int32 ActorSuccessCount = 0;
				int32 ActorFailureCount = 0;
				TArray<TSharedPtr<FJsonValue>> EditResults;
				ApplyPropertyMapToActor(Actor, **PropertyValuesObject, EditResults, ActorSuccessCount, ActorFailureCount);

				TotalSuccessCount += ActorSuccessCount;
				TotalFailureCount += ActorFailureCount;

				TSharedPtr<FJsonObject> ActorObject = ActorToolMakeActorObject(Actor);
				ActorObject->SetNumberField(TEXT("propertySuccessCount"), ActorSuccessCount);
				ActorObject->SetNumberField(TEXT("propertyFailureCount"), ActorFailureCount);
				ActorObject->SetArrayField(TEXT("propertyEdits"), EditResults);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(
					TEXT("%s: propertySuccess=%d propertyFailure=%d"),
					*Actor->GetActorLabel(),
					ActorSuccessCount,
					ActorFailureCount));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
			StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("propertyCount"), (*PropertyValuesObject)->Values.Num());
			StructuredContent->SetNumberField(TEXT("successCount"), TotalSuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), TotalFailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Applied %d property edits across %d actors. success=%d failure=%d"),
				(TotalSuccessCount + TotalFailureCount),
				Query.Actors.Num(),
				TotalSuccessCount,
				TotalFailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return ActorToolMakeExecutionResult(Text, StructuredContent, TotalFailureCount > 0);
		}
	}

	bool TryExecuteActorTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.list_level_actors"))
		{
			OutResult = ExecuteListLevelActors(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.list_selected_actors"))
		{
			OutResult = ExecuteListSelectedActors();
			return true;
		}

		if (ToolName == TEXT("unreal.select_actors"))
		{
			OutResult = ExecuteSelectActors(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.actor_get_property"))
		{
			OutResult = ExecuteActorGetProperty(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.actor_get_transform"))
		{
			OutResult = ExecuteActorGetTransform(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.set_actor_transform"))
		{
			OutResult = ExecuteSetActorTransform(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.destroy_selected_actors"))
		{
			OutResult = ExecuteDestroySelectedActors(ToolName);
			return true;
		}

		if (ToolName == TEXT("unreal.clear_level_environment"))
		{
			OutResult = ExecuteClearLevelEnvironment(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.batch_set_actor_scale"))
		{
			OutResult = ExecuteBatchSetActorScale(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.batch_set_actor_tags"))
		{
			OutResult = ExecuteBatchSetActorTags(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.batch_set_point_light_properties"))
		{
			OutResult = ExecuteBatchSetPointLightProperties(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.batch_configure_static_mesh_actors"))
		{
			OutResult = ExecuteBatchConfigureStaticMeshActors(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.layout_actors_grid"))
		{
			OutResult = ExecuteLayoutActorsGrid(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.layout_actors_circle"))
		{
			OutResult = ExecuteLayoutActorsCircle(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.spawn_actor") || ToolName == TEXT("unreal.spawn_actor_basic"))
		{
			OutResult = ExecuteSpawnActor(TEXT("unreal.spawn_actor"), Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.spawn_actor_batch") || ToolName == TEXT("unreal.spawn_actor_batch_basic"))
		{
			OutResult = ExecuteSpawnActorBatch(TEXT("unreal.spawn_actor_batch"), Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.spawn_static_mesh_actor"))
		{
			OutResult = ExecuteSpawnStaticMeshActor(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.batch_set_actor_properties"))
		{
			OutResult = ExecuteBatchSetActorProperties(ToolName, Arguments);
			return true;
		}

		return false;
	}
}
