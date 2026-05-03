#include "UnrealMcpToolOutcomeVerifiers.h"

#include "UnrealMcpModule.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "WidgetBlueprint.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> WidgetMakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		TSharedPtr<FJsonObject> WidgetMakeVerifierResult(const FString& ToolName)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), ToolName);
			Object->SetStringField(TEXT("category"), TEXT("widget"));
			Object->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_state"));
			Object->SetBoolField(TEXT("toolSpecificVerifierAvailable"), true);
			return Object;
		}

		void WidgetFinishVerifier(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Evidence,
			const TArray<FString>& Failures)
		{
			Object->SetBoolField(TEXT("verified"), Failures.Num() == 0);
			Object->SetArrayField(TEXT("evidence"), WidgetMakeStringValues(Evidence));
			Object->SetArrayField(TEXT("failures"), WidgetMakeStringValues(Failures));
			Object->SetStringField(TEXT("summary"), Failures.Num() == 0
				? TEXT("Widget Blueprint verifier confirmed the requested widget tree state.")
				: TEXT("Widget Blueprint verifier found mismatches; inspect failures for details."));
		}

		FString WidgetAddObjectNameCandidate(const FString& Path)
		{
			if (!Path.StartsWith(TEXT("/")) || Path.Contains(TEXT(".")))
			{
				return Path;
			}

			const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
			return AssetName.IsEmpty() ? Path : FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		}

		UObject* WidgetLoadObjectFromPathCandidates(const FString& RawPath)
		{
			const FString TrimmedPath = RawPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				return nullptr;
			}

			TArray<FString> Candidates;
			Candidates.Add(TrimmedPath);
			Candidates.Add(WidgetAddObjectNameCandidate(TrimmedPath));

			for (const FString& Candidate : Candidates)
			{
				if (UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Candidate))
				{
					return Object;
				}
			}
			return nullptr;
		}

		UWidgetBlueprint* LoadWidgetBlueprintFromPaths(const FJsonObject& Arguments, const FUnrealMcpExecutionResult& Result, FString& OutPath)
		{
			TArray<FString> Candidates;
			if (Result.StructuredContent.IsValid())
			{
				FString Value;
				if (Result.StructuredContent->TryGetStringField(TEXT("widgetBlueprint"), Value)) { Candidates.Add(Value); }
				if (Result.StructuredContent->TryGetStringField(TEXT("objectPath"), Value)) { Candidates.Add(Value); }
			}

			FString ArgValue;
			if (Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), ArgValue)) { Candidates.Add(ArgValue); }

			for (const FString& Candidate : Candidates)
			{
				if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(WidgetLoadObjectFromPathCandidates(Candidate)))
				{
					OutPath = WidgetBlueprint->GetPathName();
					return WidgetBlueprint;
				}
			}
			return nullptr;
		}

		FString GetReturnedWidgetName(const FUnrealMcpExecutionResult& Result)
		{
			if (!Result.StructuredContent.IsValid())
			{
				return FString();
			}

			const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
			if (Result.StructuredContent->TryGetObjectField(TEXT("widget"), WidgetObject) && WidgetObject && (*WidgetObject).IsValid())
			{
				FString WidgetName;
				(*WidgetObject)->TryGetStringField(TEXT("name"), WidgetName);
				return WidgetName;
			}
			return FString();
		}
	}

	TSharedPtr<FJsonObject> BuildWidgetToolPreflight(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& GenericPreflight)
	{
		if (!ToolName.StartsWith(TEXT("unreal.widget_")) || !GenericPreflight.IsValid())
		{
			return nullptr;
		}

		TArray<FString> Evidence;
		TArray<FString> Failures;
		GenericPreflight->SetBoolField(TEXT("toolSpecificPreflightAvailable"), true);
		GenericPreflight->SetStringField(TEXT("category"), TEXT("widget"));
		GenericPreflight->SetStringField(TEXT("checkLevel"), TEXT("tool_specific_preflight"));

		FString WidgetBlueprintPath;
		FUnrealMcpExecutionResult EmptyResult;
		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintFromPaths(Arguments, EmptyResult, WidgetBlueprintPath);
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			Failures.Add(TEXT("Target Widget Blueprint or WidgetTree could not be loaded from widgetBlueprintPath."));
		}
		else
		{
			Evidence.Add(FString::Printf(TEXT("Target Widget Blueprint is loadable before execution: %s."), *WidgetBlueprintPath));

			if (ToolName != TEXT("unreal.widget_build_template"))
			{
				FString WidgetName;
				Arguments.TryGetStringField(TEXT("widgetName"), WidgetName);
				if (WidgetName.TrimStartAndEnd().IsEmpty())
				{
					Arguments.TryGetStringField(TEXT("parentName"), WidgetName);
				}

				if (!WidgetName.TrimStartAndEnd().IsEmpty())
				{
					if (WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)))
					{
						Evidence.Add(FString::Printf(TEXT("Referenced widget exists before execution: %s."), *WidgetName));
					}
					else if (ToolName == TEXT("unreal.widget_add"))
					{
						Evidence.Add(FString::Printf(TEXT("Referenced widget '%s' is not present yet; widget_add may create it or use it as optional parent depending on arguments."), *WidgetName));
					}
					else
					{
						Failures.Add(FString::Printf(TEXT("Referenced widget does not exist before execution: %s."), *WidgetName));
					}
				}
			}
		}

		GenericPreflight->SetBoolField(TEXT("ready"), Failures.Num() == 0);
		GenericPreflight->SetArrayField(TEXT("evidence"), WidgetMakeStringValues(Evidence));
		GenericPreflight->SetArrayField(TEXT("failures"), WidgetMakeStringValues(Failures));
		GenericPreflight->SetStringField(TEXT("summary"), Failures.Num() == 0
			? TEXT("Widget preflight confirmed required Widget Blueprint state or creation context.")
			: TEXT("Widget preflight found missing target state; inspect failures before applying."));
		return GenericPreflight;
	}

	TSharedPtr<FJsonObject> VerifyWidgetToolOutcome(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const FUnrealMcpExecutionResult& Result)
	{
		if (!ToolName.StartsWith(TEXT("unreal.widget_")))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Verifier = WidgetMakeVerifierResult(ToolName);
		Verifier->SetBoolField(TEXT("toolReturnedError"), Result.bIsError);
		Verifier->SetBoolField(TEXT("genericResultSucceeded"), !Result.bIsError);
		TArray<FString> Evidence;
		TArray<FString> Failures;

		if (Result.bIsError)
		{
			Failures.Add(TEXT("Tool returned an error; Widget Blueprint success state was not verified."));
			WidgetFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		FString WidgetBlueprintPath;
		UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintFromPaths(Arguments, Result, WidgetBlueprintPath);
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			Failures.Add(TEXT("Unable to load Widget Blueprint or WidgetTree from result or arguments."));
			WidgetFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}
		Evidence.Add(FString::Printf(TEXT("Loaded Widget Blueprint %s."), *WidgetBlueprintPath));

		if (ToolName == TEXT("unreal.widget_build_template"))
		{
			const bool bHasRoot = WidgetBlueprint->WidgetTree->RootWidget != nullptr;
			if (bHasRoot)
			{
				Evidence.Add(FString::Printf(TEXT("Root widget exists: %s."), *WidgetBlueprint->WidgetTree->RootWidget->GetName()));
			}
			else
			{
				Failures.Add(TEXT("Template build did not leave a root widget."));
			}
			WidgetFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		if (ToolName == TEXT("unreal.widget_remove"))
		{
			FString RemovedWidgetName;
			Result.StructuredContent->TryGetStringField(TEXT("removedWidgetName"), RemovedWidgetName);
			if (RemovedWidgetName.TrimStartAndEnd().IsEmpty())
			{
				Arguments.TryGetStringField(TEXT("widgetName"), RemovedWidgetName);
			}

			if (!WidgetBlueprint->WidgetTree->FindWidget(FName(*RemovedWidgetName)))
			{
				Evidence.Add(FString::Printf(TEXT("Removed widget '%s' is no longer present."), *RemovedWidgetName));
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("Removed widget '%s' is still present."), *RemovedWidgetName));
			}
			WidgetFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}

		FString WidgetName = GetReturnedWidgetName(Result);
		if (WidgetName.TrimStartAndEnd().IsEmpty())
		{
			Arguments.TryGetStringField(TEXT("widgetName"), WidgetName);
		}

		UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
		if (!Widget)
		{
			Failures.Add(FString::Printf(TEXT("Widget '%s' was not found in the WidgetTree."), *WidgetName));
			WidgetFinishVerifier(Verifier, Evidence, Failures);
			return Verifier;
		}
		Evidence.Add(FString::Printf(TEXT("Widget '%s' exists with class %s."), *Widget->GetName(), *Widget->GetClass()->GetPathName()));

		if (ToolName == TEXT("unreal.widget_bind_blueprint_variable"))
		{
			bool bExpose = true;
			Arguments.TryGetBoolField(TEXT("expose"), bExpose);
			if ((Widget->bIsVariable != 0) == bExpose)
			{
				Evidence.Add(FString::Printf(TEXT("Widget variable exposure matches expected value: %s."), bExpose ? TEXT("true") : TEXT("false")));
			}
			else
			{
				Failures.Add(TEXT("Widget variable exposure does not match requested expose value."));
			}
		}

		if (ToolName == TEXT("unreal.widget_set_slot_layout"))
		{
			if (Widget->Slot)
			{
				Evidence.Add(FString::Printf(TEXT("Widget has parent slot %s after layout update."), *Widget->Slot->GetClass()->GetPathName()));
			}
			else
			{
				Failures.Add(TEXT("Widget has no parent slot after slot layout update."));
			}
		}

		if (ToolName == TEXT("unreal.widget_bind_event"))
		{
			FString EventName;
			if (Result.StructuredContent->TryGetStringField(TEXT("eventName"), EventName))
			{
				Evidence.Add(FString::Printf(TEXT("Result reports event binding for '%s'."), *EventName));
			}
			else
			{
				Failures.Add(TEXT("Event binding result did not report eventName."));
			}
		}

		WidgetFinishVerifier(Verifier, Evidence, Failures);
		return Verifier;
	}
}
