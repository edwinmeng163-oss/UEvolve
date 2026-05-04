#include "UnrealMcpActorTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UnrealMcpModule.h"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);

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

		return false;
	}
}
