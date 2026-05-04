#include "UnrealMcpModule.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "JsonObjectConverter.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace UnrealMcp
{
	static constexpr int32 DefaultListLimit = 200;

	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	bool TryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues);

		struct FActorQueryResult
		{
			TArray<AActor*> Actors;
			TArray<FString> RequestedPaths;
			FString FilterText;
			FString ClassPathFilter;
			int32 MatchCount = 0;
			int32 Limit = DefaultListLimit;
			bool bSelectedOnly = false;
			bool bTruncated = false;
		};

		bool MatchesActorFilters(
			AActor* Actor,
			const FString& FilterText,
			const FString& ClassPathFilter,
			const TSet<FString>& ExplicitPaths);

		FProperty* FindPropertyByNameLoose(const UStruct* Struct, const FString& PropertyName)
		{
			if (!Struct)
			{
				return nullptr;
			}

			if (FProperty* ExactProperty = Struct->FindPropertyByName(*PropertyName))
			{
				return ExactProperty;
			}

			for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase)
					|| It->GetAuthoredName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}

			return nullptr;
		}

		bool ResolveObjectPropertyPath(
			UObject* RootObject,
			const FString& PropertyPath,
			UObject*& OutOwnerObject,
			FProperty*& OutLeafProperty,
			FProperty*& OutNotifyProperty,
			void*& OutValuePtr,
			FString& OutFailureReason)
		{
			OutOwnerObject = nullptr;
			OutLeafProperty = nullptr;
			OutNotifyProperty = nullptr;
			OutValuePtr = nullptr;
			OutFailureReason.Reset();

			if (!RootObject)
			{
				OutFailureReason = TEXT("Cannot resolve a property path on a null object.");
				return false;
			}

			TArray<FString> RawSegments;
			PropertyPath.ParseIntoArray(RawSegments, TEXT("."), true);

			TArray<FString> Segments;
			for (const FString& RawSegment : RawSegments)
			{
				const FString Segment = RawSegment.TrimStartAndEnd();
				if (!Segment.IsEmpty())
				{
					Segments.Add(Segment);
				}
			}

			if (Segments.Num() == 0)
			{
				OutFailureReason = TEXT("The property path is empty.");
				return false;
			}

			UObject* CurrentOwnerObject = RootObject;
			const UStruct* CurrentStruct = RootObject->GetClass();
			void* CurrentContainer = RootObject;
			FProperty* CurrentNotifyProperty = nullptr;

			for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
			{
				const FString& Segment = Segments[SegmentIndex];
				FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segment);
				if (!Property)
				{
					OutFailureReason = FString::Printf(
						TEXT("Property segment '%s' was not found on '%s' while resolving '%s'."),
						*Segment,
						*CurrentStruct->GetPathName(),
						*PropertyPath);
					return false;
				}

				const bool bIsLastSegment = SegmentIndex == Segments.Num() - 1;
				if (bIsLastSegment)
				{
					OutOwnerObject = CurrentOwnerObject;
					OutLeafProperty = Property;
					OutNotifyProperty = CurrentNotifyProperty ? CurrentNotifyProperty : Property;
					OutValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
					return OutOwnerObject != nullptr && OutLeafProperty != nullptr && OutValuePtr != nullptr;
				}

				if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					if (!CurrentNotifyProperty)
					{
						CurrentNotifyProperty = Property;
					}

					CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
					CurrentStruct = StructProperty->Struct;
					continue;
				}

				if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					UObject* NextObject = ObjectProperty->GetObjectPropertyValue_InContainer(CurrentContainer);
					if (!NextObject)
					{
						OutFailureReason = FString::Printf(
							TEXT("Property segment '%s' on '%s' resolved to null while resolving '%s'."),
							*Segment,
							*CurrentOwnerObject->GetPathName(),
							*PropertyPath);
						return false;
					}

					CurrentOwnerObject = NextObject;
					CurrentStruct = NextObject->GetClass();
					CurrentContainer = NextObject;
					CurrentNotifyProperty = nullptr;
					continue;
				}

				OutFailureReason = FString::Printf(
					TEXT("Property segment '%s' on '%s' is neither a struct nor object property, so '%s' cannot be resolved further."),
					*Segment,
					*CurrentStruct->GetPathName(),
					*PropertyPath);
				return false;
			}

			OutFailureReason = FString::Printf(TEXT("Unable to resolve property path '%s'."), *PropertyPath);
			return false;
		}

		TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr)
		{
			if (!Property || !ValuePtr)
			{
				return MakeShared<FJsonValueNull>();
			}

			if (TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
				Property,
				ValuePtr,
				0,
				0,
				nullptr,
				nullptr,
				EJsonObjectConversionFlags::WriteTextAsComplexString))
			{
				return JsonValue;
			}

			FString ExportedText;
			Property->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(ExportedText);
		}

		bool ApplyPropertyMapToActor(
			AActor* Actor,
			const FJsonObject& PropertyValues,
			TArray<TSharedPtr<FJsonValue>>& OutEditResults,
			int32& OutSuccessCount,
			int32& OutFailureCount)
		{
			OutSuccessCount = 0;
			OutFailureCount = 0;

			if (!Actor)
			{
				return false;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : PropertyValues.Values)
			{
				const FString PropertyPath = Entry.Key.TrimStartAndEnd();
				TSharedPtr<FJsonObject> EditObject = MakeShared<FJsonObject>();
				EditObject->SetStringField(TEXT("propertyPath"), PropertyPath);

				if (PropertyPath.IsEmpty())
				{
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(TEXT("error"), TEXT("Property paths cannot be empty."));
					OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
					++OutFailureCount;
					continue;
				}

				UObject* OwnerObject = nullptr;
				FProperty* LeafProperty = nullptr;
				FProperty* NotifyProperty = nullptr;
				void* ValuePtr = nullptr;
				FString FailureReason;
				if (!ResolveObjectPropertyPath(Actor, PropertyPath, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, FailureReason))
				{
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(TEXT("error"), FailureReason);
					OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
					++OutFailureCount;
					continue;
					}

					EditObject->SetStringField(TEXT("ownerObjectPath"), OwnerObject->GetPathName());
					const TSharedPtr<FJsonValue> BeforeValue = UnrealMcp::PropertyValueToJson(LeafProperty, ValuePtr);
					if (BeforeValue.IsValid())
					{
						EditObject->SetField(TEXT("before"), BeforeValue);
				}

				OwnerObject->Modify();
				if (OwnerObject != Actor)
				{
					Actor->Modify();
				}

				OwnerObject->PreEditChange(NotifyProperty);

				FText ImportFailureReason;
				const bool bApplied = FJsonObjectConverter::JsonValueToUProperty(
					Entry.Value,
					LeafProperty,
					ValuePtr,
					0,
					0,
					false,
					&ImportFailureReason,
					nullptr);

				if (bApplied)
				{
					FPropertyChangedEvent PropertyChangedEvent(NotifyProperty, EPropertyChangeType::ValueSet);
					OwnerObject->PostEditChangeProperty(PropertyChangedEvent);
					OwnerObject->MarkPackageDirty();
					Actor->MarkPackageDirty();

					EditObject->SetBoolField(TEXT("success"), true);
					const TSharedPtr<FJsonValue> AfterValue = UnrealMcp::PropertyValueToJson(LeafProperty, ValuePtr);
					if (AfterValue.IsValid())
					{
						EditObject->SetField(TEXT("after"), AfterValue);
					}
					++OutSuccessCount;
				}
				else
				{
					OwnerObject->PostEditChange();
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(
						TEXT("error"),
						ImportFailureReason.IsEmpty()
							? FString::Printf(TEXT("Failed to apply property '%s'."), *PropertyPath)
							: ImportFailureReason.ToString());
					++OutFailureCount;
				}

				OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
			}

			return OutFailureCount == 0;
		}

		bool ResolveActorsFromArguments(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FJsonObject& Arguments,
			FActorQueryResult& OutQuery,
			FString& OutFailureReason)
		{
			OutQuery = FActorQueryResult();
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return false;
			}

			Arguments.TryGetStringField(TEXT("filter"), OutQuery.FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), OutQuery.ClassPathFilter);
			Arguments.TryGetBoolField(TEXT("selectedOnly"), OutQuery.bSelectedOnly);
			TryGetStringArrayField(Arguments, TEXT("paths"), OutQuery.RequestedPaths);
			OutQuery.Limit = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("limit"), DefaultListLimit), 1000);

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
				if (!Actor || !MatchesActorFilters(Actor, OutQuery.FilterText, OutQuery.ClassPathFilter, ExplicitPaths))
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


	TArray<FAssetData> GetSelectedAssets()
	{
		TArray<FAssetData> SelectedAssets;
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		return SelectedAssets;
	}

	TSharedPtr<FJsonObject> MakeAssetObject(const FAssetData& Asset)
	{
		TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
		AssetObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
		AssetObject->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
		AssetObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
		return AssetObject;
	}

	TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Vector)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Rotator)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Object->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeActorObject(AActor* Actor)
	{
		TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
		ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObject->SetStringField(TEXT("name"), Actor->GetName());
		ActorObject->SetStringField(TEXT("classPath"), Actor->GetClass()->GetPathName());
		ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());
		ActorObject->SetObjectField(TEXT("location"), MakeVectorObject(Actor->GetActorLocation()));
		ActorObject->SetObjectField(TEXT("rotation"), MakeRotatorObject(Actor->GetActorRotation()));
		return ActorObject;
	}

	FString DescribeAsset(const FAssetData& Asset)
	{
		return FString::Printf(TEXT("%s [%s]"), *Asset.GetSoftObjectPath().ToString(), *Asset.AssetClassPath.ToString());
	}

		FString DescribeActor(AActor* Actor)
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

		bool MatchesActorFilters(
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

		AActor* ResolveActorReference(
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
}
