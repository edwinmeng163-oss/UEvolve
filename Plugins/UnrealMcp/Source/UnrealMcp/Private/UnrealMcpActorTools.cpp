#include "UnrealMcpActorTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UnrealMcpModule.h"

namespace UnrealMcp
{
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);

	namespace
	{
		static constexpr int32 ActorToolDefaultListLimit = 200;

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

		return false;
	}
}
