#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpSharedPathResolver.h"

namespace UnrealMcp
{
	TSharedPtr<FJsonObject> MakeObjectSchema();
	FString NormalizeFullPathForCompare(const FString& Path);
	bool AnalyzeOpenAiSchemaCompatibility(
		const TSharedPtr<FJsonObject>& InputSchema,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutReason,
		TSharedPtr<FJsonObject>& OutNormalizedSchema);

		bool ExtractRequestedSchemaFromScaffoldReadme(const FString& ScaffoldDirectory, FString& OutSchemaJson)
		{
			OutSchemaJson.Reset();

			FString ReadmeText;
			const FString ReadmePath = FPaths::Combine(ScaffoldDirectory, TEXT("README.md"));
			if (!FFileHelper::LoadFileToString(ReadmeText, *ReadmePath))
			{
				return false;
			}

			const FString Heading = TEXT("## Requested Argument Schema");
			const int32 HeadingOffset = ReadmeText.Find(Heading, ESearchCase::IgnoreCase);
			if (HeadingOffset == INDEX_NONE)
			{
				return false;
			}

			const int32 FenceStart = ReadmeText.Find(TEXT("```json"), ESearchCase::IgnoreCase, ESearchDir::FromStart, HeadingOffset);
			if (FenceStart == INDEX_NONE)
			{
				return false;
			}

			const int32 JsonStart = FenceStart + FString(TEXT("```json")).Len();
			const int32 FenceEnd = ReadmeText.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, JsonStart);
			if (FenceEnd == INDEX_NONE || FenceEnd <= JsonStart)
			{
				return false;
			}

			OutSchemaJson = ReadmeText.Mid(JsonStart, FenceEnd - JsonStart).TrimStartAndEnd();
			return !OutSchemaJson.IsEmpty();
		}

		TSharedPtr<FJsonObject> MakeScaffoldFileObject(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			bool bIncludeFileText,
			int32 MaxPreviewChars)
		{
			const FString FilePath = FPaths::Combine(ScaffoldDirectory, FileName);
			TSharedPtr<FJsonObject> FileObject = MakeFileInfoObject(FilePath);
			FileObject->SetStringField(TEXT("name"), FileName);
			FileObject->SetBoolField(TEXT("required"), bRequired);
			FileObject->SetBoolField(TEXT("missing"), !FPaths::FileExists(FilePath));

			FString Text;
			if (FFileHelper::LoadFileToString(Text, *FilePath))
			{
				const int32 SafeMaxPreviewChars = FMath::Max(100, MaxPreviewChars);
				FileObject->SetNumberField(TEXT("characterCount"), Text.Len());
				FileObject->SetStringField(TEXT("preview"), Text.Left(SafeMaxPreviewChars));
				FileObject->SetBoolField(TEXT("previewTruncated"), Text.Len() > SafeMaxPreviewChars);
				if (bIncludeFileText)
				{
					FileObject->SetStringField(TEXT("text"), Text);
				}
			}
			return FileObject;
		}

		bool TryReadToolNameFromTestRequest(
			const FString& TestRequestPath,
			FString& OutToolName,
			TSharedPtr<FJsonObject>& OutTestRequestObject,
			FString& OutFailureReason)
		{
			OutToolName.Reset();
			OutTestRequestObject.Reset();
			OutFailureReason.Reset();

			if (!FPaths::FileExists(TestRequestPath))
			{
				OutFailureReason = FString::Printf(TEXT("TestRequest.json is missing at '%s'."), *TestRequestPath);
				return false;
			}

			if (!LoadJsonObjectFromFile(TestRequestPath, OutTestRequestObject, OutFailureReason))
			{
				return false;
			}

			FString Method;
			OutTestRequestObject->TryGetStringField(TEXT("method"), Method);
			if (Method != TEXT("tools/call"))
			{
				OutFailureReason = TEXT("TestRequest.json method is not tools/call.");
				return false;
			}

			const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
			if (!OutTestRequestObject->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !(*ParamsObject).IsValid())
			{
				OutFailureReason = TEXT("TestRequest.json is missing params object.");
				return false;
			}

			(*ParamsObject)->TryGetStringField(TEXT("name"), OutToolName);
			OutToolName = OutToolName.TrimStartAndEnd();
			if (OutToolName.IsEmpty())
			{
				OutFailureReason = TEXT("TestRequest.json params.name is empty.");
				return false;
			}
			return true;
		}

		bool ResolveMcpScaffoldForInspection(
			const FJsonObject& Arguments,
			FString& OutDirectory,
			FString& OutToolName,
			FString& OutFailureReason,
			FToolsReadResolution* OutResolution = nullptr)
		{
			if (OutResolution)
			{
				*OutResolution = FToolsReadResolution();
			}

			FString ScaffoldDir;
			FString ToolName;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
			ToolName = ToolName.TrimStartAndEnd();

			if (!ScaffoldDir.IsEmpty())
			{
				if (!ResolveProjectPathInsideProject(ScaffoldDir, OutDirectory, OutFailureReason))
				{
					return false;
				}
				if (OutResolution)
				{
					OutResolution->Path = OutDirectory;
					OutResolution->bFound = FPaths::DirectoryExists(OutDirectory);
					OutResolution->SourceKind = FToolsReadResolution::ESource::ProjectLocal;
					OutResolution->Candidates.Add(OutDirectory);
					OutResolution->Warning = TEXT("Explicit scaffoldDir is project-local only; pass toolName with scaffoldDir empty to use shared repo recipe fallback.");
				}
			}
			else
			{
				if (ToolName.IsEmpty())
				{
					OutFailureReason = TEXT("Provide either scaffoldDir or toolName.");
					return false;
				}

				if (OutputRoot.TrimStartAndEnd().IsEmpty())
				{
					if (!ResolveScaffoldReadDirectory(SanitizeMcpToolIdForPath(ToolName), OutDirectory, OutFailureReason, OutResolution))
					{
						return false;
					}
				}
				else
				{
					FString ResolvedOutputRoot;
					if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, OutFailureReason))
					{
						return false;
					}
					OutDirectory = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
					if (OutResolution)
					{
						OutResolution->Path = OutDirectory;
						OutResolution->bFound = FPaths::DirectoryExists(OutDirectory);
						OutResolution->SourceKind = FToolsReadResolution::ESource::ProjectLocal;
						OutResolution->Candidates.Add(OutDirectory);
						OutResolution->Warning = TEXT("Explicit outputRoot is project-local only; omit outputRoot to use shared repo recipe fallback.");
					}
				}
			}

			OutToolName = ToolName;
			if (OutToolName.IsEmpty())
			{
				TSharedPtr<FJsonObject> TestRequestObject;
				FString TestRequestFailure;
				TryReadToolNameFromTestRequest(FPaths::Combine(OutDirectory, TEXT("TestRequest.json")), OutToolName, TestRequestObject, TestRequestFailure);
			}
			return true;
		}

		TSharedPtr<FJsonObject> InspectMcpScaffoldDirectory(
			const FString& ScaffoldDirectory,
			const FString& RequestedToolName,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
			bool bIncludeFileText,
			int32 MaxPreviewChars)
		{
			const FString ResolvedScaffoldDirectory = NormalizeFullPathForCompare(ScaffoldDirectory);
			TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
			ResultObject->SetStringField(TEXT("directory"), ResolvedScaffoldDirectory);
			ResultObject->SetStringField(TEXT("toolId"), FPaths::GetCleanFilename(ResolvedScaffoldDirectory));
			ResultObject->SetBoolField(TEXT("exists"), FPaths::DirectoryExists(ResolvedScaffoldDirectory));

			static const TCHAR* RequiredFiles[] = {
				TEXT("README.md"),
				TEXT("ToolRegistrar.patch.cpp"),
				TEXT("ToolRegistrarCall.patch.cpp"),
				TEXT("CategoryHandlerFunction.patch.cpp"),
				TEXT("CategoryDispatcherBranch.patch.cpp"),
				TEXT("ToolRegistryPatch.json"),
				TEXT("TestRequest.json"),
				TEXT("IntegrationChecklist.md")
			};

			TArray<TSharedPtr<FJsonValue>> Files;
			TArray<TSharedPtr<FJsonValue>> MissingRequiredFiles;
			for (const TCHAR* FileName : RequiredFiles)
			{
				TSharedPtr<FJsonObject> FileObject = MakeScaffoldFileObject(ResolvedScaffoldDirectory, FileName, true, bIncludeFileText, MaxPreviewChars);
				if (FileObject->GetBoolField(TEXT("missing")))
				{
					MissingRequiredFiles.Add(MakeShared<FJsonValueString>(FileName));
				}
				Files.Add(MakeShared<FJsonValueObject>(FileObject));
			}
			Files.Add(MakeShared<FJsonValueObject>(MakeScaffoldFileObject(ResolvedScaffoldDirectory, TEXT("ChatCommand.patch.cpp"), false, bIncludeFileText, MaxPreviewChars)));
			Files.Add(MakeShared<FJsonValueObject>(MakeScaffoldFileObject(ResolvedScaffoldDirectory, TEXT("LegacyToolDefinition.legacy.cpp"), false, bIncludeFileText, MaxPreviewChars)));
			Files.Add(MakeShared<FJsonValueObject>(MakeScaffoldFileObject(ResolvedScaffoldDirectory, TEXT("LegacyExecuteToolHandler.legacy.cpp"), false, bIncludeFileText, MaxPreviewChars)));

			FString ToolName = RequestedToolName;
			FString TestRequestFailure;
			TSharedPtr<FJsonObject> TestRequestObject;
			const bool bValidTestRequest = TryReadToolNameFromTestRequest(
				FPaths::Combine(ResolvedScaffoldDirectory, TEXT("TestRequest.json")),
				ToolName,
				TestRequestObject,
				TestRequestFailure);

			ResultObject->SetStringField(TEXT("toolName"), ToolName);
			ResultObject->SetBoolField(TEXT("validTestRequest"), bValidTestRequest);
			if (!TestRequestFailure.IsEmpty())
			{
				ResultObject->SetStringField(TEXT("testRequestIssue"), TestRequestFailure);
			}
			if (TestRequestObject.IsValid())
			{
				ResultObject->SetObjectField(TEXT("testRequest"), TestRequestObject);
			}

			static const TCHAR* RequiredPatchFiles[] = {
				TEXT("ToolRegistrar.patch.cpp"),
				TEXT("ToolRegistrarCall.patch.cpp"),
				TEXT("CategoryHandlerFunction.patch.cpp"),
				TEXT("CategoryDispatcherBranch.patch.cpp")
			};

			TArray<TSharedPtr<FJsonValue>> PatchValidations;
			TArray<TSharedPtr<FJsonValue>> ReadinessIssues;
			bool bPatchesSafe = true;
			auto AddReadinessIssue = [&ReadinessIssues](const FString& Code, const FString& Message)
			{
				TSharedPtr<FJsonObject> IssueObject = MakeShared<FJsonObject>();
				IssueObject->SetStringField(TEXT("severity"), TEXT("error"));
				IssueObject->SetStringField(TEXT("code"), Code);
				IssueObject->SetStringField(TEXT("message"), Message);
				ReadinessIssues.Add(MakeShared<FJsonValueObject>(IssueObject));
			};

			for (const TCHAR* PatchFileName : RequiredPatchFiles)
			{
				const FString PatchPath = FPaths::Combine(ResolvedScaffoldDirectory, PatchFileName);
				FString PatchText;
				if (!FFileHelper::LoadFileToString(PatchText, *PatchPath))
				{
					bPatchesSafe = false;
					AddReadinessIssue(TEXT("missing_patch_file"), FString::Printf(TEXT("Required patch file is missing or unreadable: %s"), PatchFileName));
					continue;
				}

				TSharedPtr<FJsonObject> ValidationObject = ValidateCppSnippetText(PatchText, PatchFileName, ToolName);
				PatchValidations.Add(MakeShared<FJsonValueObject>(ValidationObject));
				if (!ValidationObject->GetBoolField(TEXT("safe")))
				{
					bPatchesSafe = false;
					AddReadinessIssue(TEXT("unsafe_patch_file"), FString::Printf(TEXT("Patch file failed static validation: %s"), PatchFileName));
				}
			}

			const FString RegistryPatchPath = FPaths::Combine(ResolvedScaffoldDirectory, TEXT("ToolRegistryPatch.json"));
			TSharedPtr<FJsonObject> RegistryPatchObject;
			FString RegistryPatchFailure;
			bool bRegistryPatchValid = LoadJsonObjectFromFile(RegistryPatchPath, RegistryPatchObject, RegistryPatchFailure) && RegistryPatchObject.IsValid();
			if (!bRegistryPatchValid)
			{
				AddReadinessIssue(TEXT("invalid_registry_patch"), RegistryPatchFailure.IsEmpty() ? TEXT("ToolRegistryPatch.json is missing or invalid JSON.") : RegistryPatchFailure);
			}
			else
			{
				FString RegistryPatchToolName;
				RegistryPatchObject->TryGetStringField(TEXT("name"), RegistryPatchToolName);
				if (!ToolName.IsEmpty() && RegistryPatchToolName != ToolName)
				{
					bRegistryPatchValid = false;
					AddReadinessIssue(
						TEXT("registry_tool_name_mismatch"),
						FString::Printf(TEXT("ToolRegistryPatch.json name '%s' does not match scaffold toolName '%s'."), *RegistryPatchToolName, *ToolName));
				}
				ResultObject->SetObjectField(TEXT("toolRegistryPatch"), RegistryPatchObject);
			}

			FString RequestedSchemaJson;
			const bool bHasRequestedSchema = ExtractRequestedSchemaFromScaffoldReadme(ResolvedScaffoldDirectory, RequestedSchemaJson);
			ResultObject->SetBoolField(TEXT("hasRequestedSchema"), bHasRequestedSchema);
			if (bHasRequestedSchema)
			{
				ResultObject->SetStringField(TEXT("requestedSchemaJson"), RequestedSchemaJson);
				TSharedPtr<FJsonObject> RequestedSchemaObject;
				if (LoadJsonObject(RequestedSchemaJson, RequestedSchemaObject) && RequestedSchemaObject.IsValid())
				{
					TArray<TSharedPtr<FJsonValue>> SchemaIssues;
					FString SchemaReason;
					TSharedPtr<FJsonObject> NormalizedSchema;
					const bool bSchemaCompatible = AnalyzeOpenAiSchemaCompatibility(RequestedSchemaObject, SchemaIssues, SchemaReason, NormalizedSchema);
					ResultObject->SetBoolField(TEXT("schemaCompatible"), bSchemaCompatible);
					ResultObject->SetStringField(TEXT("schemaReason"), SchemaReason);
					ResultObject->SetArrayField(TEXT("schemaIssues"), SchemaIssues);
					if (NormalizedSchema.IsValid())
					{
						ResultObject->SetObjectField(TEXT("normalizedSchema"), NormalizedSchema);
					}
				}
				else
				{
					ResultObject->SetBoolField(TEXT("schemaCompatible"), false);
					ResultObject->SetStringField(TEXT("schemaReason"), TEXT("Requested schema block is not valid JSON."));
				}
			}

			const bool bToolListed = !ToolName.IsEmpty() && FindToolDefinitionByName(ToolsArray, ToolName).IsValid();
			ResultObject->SetBoolField(TEXT("toolListed"), bToolListed);
			ResultObject->SetBoolField(TEXT("patchesSafe"), bPatchesSafe);
			ResultObject->SetBoolField(TEXT("registryPatchValid"), bRegistryPatchValid);
			ResultObject->SetBoolField(TEXT("readyForApply"), MissingRequiredFiles.Num() == 0 && bValidTestRequest && bPatchesSafe && bRegistryPatchValid);
			ResultObject->SetArrayField(TEXT("missingRequiredFiles"), MissingRequiredFiles);
			ResultObject->SetArrayField(TEXT("patchValidations"), PatchValidations);
			ResultObject->SetArrayField(TEXT("readinessIssues"), ReadinessIssues);
			ResultObject->SetArrayField(TEXT("files"), Files);
			return ResultObject;
		}

		FUnrealMcpExecutionResult ListMcpScaffolds(
			const FJsonObject& Arguments,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
		{
			FString OutputRoot;
			FString ToolNameFilter;
			bool bIncludeSavedTestScaffolds = true;
			bool bIncludeFileText = false;
			bool bReadyOnly = false;
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			Arguments.TryGetStringField(TEXT("toolNameFilter"), ToolNameFilter);
			Arguments.TryGetBoolField(TEXT("includeSavedTestScaffolds"), bIncludeSavedTestScaffolds);
			Arguments.TryGetBoolField(TEXT("includeFileText"), bIncludeFileText);
			Arguments.TryGetBoolField(TEXT("readyOnly"), bReadyOnly);
			const int32 MaxPreviewChars = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewChars"), 1200), 20000);

			FString ResolvedOutputRoot;
			FString FailureReason;
			if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TArray<FString> CandidateDirectories;
			FindImmediateChildren(ResolvedOutputRoot, TEXT("*"), false, true, CandidateDirectories);
			if (bIncludeSavedTestScaffolds)
			{
				FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, CandidateDirectories);
			}
			CandidateDirectories.Sort();

			TArray<TSharedPtr<FJsonValue>> ScaffoldObjects;
			TArray<TSharedPtr<FJsonValue>> SkippedObjects;
			for (const FString& CandidateDirectory : CandidateDirectories)
			{
				TSharedPtr<FJsonObject> ScaffoldObject = InspectMcpScaffoldDirectory(CandidateDirectory, FString(), ToolsArray, bIncludeFileText, MaxPreviewChars);
				FString ToolName;
				ScaffoldObject->TryGetStringField(TEXT("toolName"), ToolName);
				const bool bReadyForApply = ScaffoldObject->GetBoolField(TEXT("readyForApply"));

				if (!ToolNameFilter.TrimStartAndEnd().IsEmpty()
					&& !ToolName.Contains(ToolNameFilter.TrimStartAndEnd(), ESearchCase::IgnoreCase)
					&& !CandidateDirectory.Contains(ToolNameFilter.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				{
					ScaffoldObject->SetStringField(TEXT("skipReason"), TEXT("toolNameFilter did not match."));
					SkippedObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
					continue;
				}

				if (bReadyOnly && !bReadyForApply)
				{
					ScaffoldObject->SetStringField(TEXT("skipReason"), TEXT("readyOnly=true and scaffold is not ready for apply."));
					SkippedObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
					continue;
				}

				ScaffoldObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_list_scaffolds"));
			StructuredContent->SetStringField(TEXT("outputRoot"), ResolvedOutputRoot);
			StructuredContent->SetBoolField(TEXT("includeSavedTestScaffolds"), bIncludeSavedTestScaffolds);
			StructuredContent->SetBoolField(TEXT("includeFileText"), bIncludeFileText);
			StructuredContent->SetBoolField(TEXT("readyOnly"), bReadyOnly);
			StructuredContent->SetStringField(TEXT("toolNameFilter"), ToolNameFilter);
			StructuredContent->SetNumberField(TEXT("scaffoldCount"), ScaffoldObjects.Num());
			StructuredContent->SetNumberField(TEXT("skippedCount"), SkippedObjects.Num());
			StructuredContent->SetArrayField(TEXT("scaffolds"), ScaffoldObjects);
			StructuredContent->SetArrayField(TEXT("skipped"), SkippedObjects);

			return MakeExecutionResult(
				FString::Printf(TEXT("Found %d MCP scaffold%s (%d skipped)."), ScaffoldObjects.Num(), ScaffoldObjects.Num() == 1 ? TEXT("") : TEXT("s"), SkippedObjects.Num()),
				StructuredContent,
				false);
		}

			FUnrealMcpExecutionResult InspectMcpScaffold(
				const FJsonObject& Arguments,
				const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
			{
			bool bIncludeFileText = false;
			Arguments.TryGetBoolField(TEXT("includeFileText"), bIncludeFileText);
			const int32 MaxPreviewChars = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewChars"), 2000), 50000);

			FString ScaffoldDirectory;
			FString ToolName;
			FString FailureReason;
			FToolsReadResolution ScaffoldResolution;
			if (!ResolveMcpScaffoldForInspection(Arguments, ScaffoldDirectory, ToolName, FailureReason, &ScaffoldResolution))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TSharedPtr<FJsonObject> InspectionObject = InspectMcpScaffoldDirectory(ScaffoldDirectory, ToolName, ToolsArray, bIncludeFileText, MaxPreviewChars);
			InspectionObject->SetStringField(TEXT("action"), TEXT("mcp_inspect_scaffold"));
			InspectionObject->SetBoolField(TEXT("scaffoldFound"), ScaffoldResolution.bFound);
			InspectionObject->SetStringField(TEXT("scaffoldSourceKind"), LexToString(ScaffoldResolution.SourceKind));
			InspectionObject->SetArrayField(TEXT("scaffoldCandidates"), MakeToolsReadCandidateValues(ScaffoldResolution));
			if (!ScaffoldResolution.Warning.IsEmpty())
			{
				InspectionObject->SetStringField(TEXT("scaffoldResolutionWarning"), ScaffoldResolution.Warning);
			}
			const bool bReadyForApply = InspectionObject->GetBoolField(TEXT("readyForApply"));
			FString InspectedToolName;
			InspectionObject->TryGetStringField(TEXT("toolName"), InspectedToolName);

			return MakeExecutionResult(
				FString::Printf(TEXT("Inspected MCP scaffold %s. readyForApply=%s."),
					InspectedToolName.IsEmpty() ? *InspectionObject->GetStringField(TEXT("toolId")) : *InspectedToolName,
					bReadyForApply ? TEXT("true") : TEXT("false")),
					InspectionObject,
					false);
			}

			TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value);

			TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object)
			{
				TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
				if (!Object.IsValid())
				{
					return Clone;
				}

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					Clone->SetField(Pair.Key, CloneJsonValue(Pair.Value));
				}
				return Clone;
			}

			TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
			{
				if (!Value.IsValid())
				{
					return MakeShared<FJsonValueNull>();
				}

				switch (Value->Type)
				{
				case EJson::String:
					return MakeShared<FJsonValueString>(Value->AsString());
				case EJson::Number:
					return MakeShared<FJsonValueNumber>(Value->AsNumber());
				case EJson::Boolean:
					return MakeShared<FJsonValueBoolean>(Value->AsBool());
				case EJson::Array:
				{
					TArray<TSharedPtr<FJsonValue>> ClonedArray;
					for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
					{
						ClonedArray.Add(CloneJsonValue(Item));
					}
					return MakeShared<FJsonValueArray>(ClonedArray);
				}
				case EJson::Object:
					return MakeShared<FJsonValueObject>(CloneJsonObject(Value->AsObject()));
				case EJson::Null:
				default:
					return MakeShared<FJsonValueNull>();
				}
			}

			TArray<FString> GetSortedJsonObjectKeys(const TSharedPtr<FJsonObject>& Object)
			{
				TArray<FString> Keys;
				if (Object.IsValid())
				{
					Object->Values.GetKeys(Keys);
					Keys.Sort();
				}
				return Keys;
			}

			FString GetSchemaType(const TSharedPtr<FJsonObject>& SchemaObject)
			{
				FString Type;
				if (SchemaObject.IsValid())
				{
					SchemaObject->TryGetStringField(TEXT("type"), Type);
					if (Type.IsEmpty())
					{
						if (SchemaObject->HasTypedField<EJson::Object>(TEXT("properties")))
						{
							Type = TEXT("object");
						}
						else if (SchemaObject->HasField(TEXT("items")))
						{
							Type = TEXT("array");
						}
					}
				}
				return Type.ToLower();
			}

			TSharedPtr<FJsonValue> MakeSampleJsonValueForSchema(const TSharedPtr<FJsonObject>& SchemaObject, bool bBoundaryValue, bool bWrongType)
			{
				const FString Type = GetSchemaType(SchemaObject);
				if (bWrongType)
				{
					if (Type == TEXT("string"))
					{
						return MakeShared<FJsonValueNumber>(12345.0);
					}
					if (Type == TEXT("integer") || Type == TEXT("number"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_a_number"));
					}
					if (Type == TEXT("boolean"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_a_boolean"));
					}
					if (Type == TEXT("array"))
					{
						return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
					}
					if (Type == TEXT("object"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_an_object"));
					}
					return MakeShared<FJsonValueString>(TEXT("wrong_type_probe"));
				}

				if (!bBoundaryValue && SchemaObject.IsValid())
				{
					const TSharedPtr<FJsonValue> DefaultValue = SchemaObject->TryGetField(TEXT("default"));
					if (DefaultValue.IsValid())
					{
						return CloneJsonValue(DefaultValue);
					}

					const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
					if (SchemaObject->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues && EnumValues->Num() > 0)
					{
						return CloneJsonValue((*EnumValues)[0]);
					}
				}

				if (Type == TEXT("integer"))
				{
					return MakeShared<FJsonValueNumber>(bBoundaryValue ? 0.0 : 1.0);
				}
				if (Type == TEXT("number"))
				{
					return MakeShared<FJsonValueNumber>(bBoundaryValue ? 0.0 : 1.0);
				}
				if (Type == TEXT("boolean"))
				{
					return MakeShared<FJsonValueBoolean>(!bBoundaryValue);
				}
				if (Type == TEXT("array"))
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					if (!bBoundaryValue && SchemaObject.IsValid())
					{
						const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
						if (SchemaObject->TryGetObjectField(TEXT("items"), ItemsObject) && ItemsObject && (*ItemsObject).IsValid())
						{
							Values.Add(MakeSampleJsonValueForSchema(*ItemsObject, false, false));
						}
						else
						{
							Values.Add(MakeShared<FJsonValueString>(TEXT("sample")));
						}
					}
					return MakeShared<FJsonValueArray>(Values);
				}
				if (Type == TEXT("object"))
				{
					TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
					if (!bBoundaryValue && SchemaObject.IsValid())
					{
						const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
						if (SchemaObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
						{
							for (const FString& Key : GetSortedJsonObjectKeys(*PropertiesObject))
							{
								const TSharedPtr<FJsonObject>* ChildSchema = nullptr;
								if ((*PropertiesObject)->TryGetObjectField(Key, ChildSchema) && ChildSchema && (*ChildSchema).IsValid())
								{
									ObjectValue->SetField(Key, MakeSampleJsonValueForSchema(*ChildSchema, false, false));
								}
							}
						}
					}
					return MakeShared<FJsonValueObject>(ObjectValue);
				}
				return MakeShared<FJsonValueString>(bBoundaryValue ? FString() : FString(TEXT("sample")));
			}

			void GetRequiredSchemaFields(const TSharedPtr<FJsonObject>& SchemaObject, TArray<FString>& OutRequiredFields)
			{
				OutRequiredFields.Reset();
				if (!SchemaObject.IsValid())
				{
					return;
				}

				const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
				if (SchemaObject->TryGetArrayField(TEXT("required"), RequiredValues) && RequiredValues)
				{
					for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
					{
						if (Value.IsValid() && Value->Type == EJson::String)
						{
							OutRequiredFields.Add(Value->AsString());
						}
					}
				}
				OutRequiredFields.Sort();
			}

			TSharedPtr<FJsonObject> MakeSampleArgumentsForSchema(
				const TSharedPtr<FJsonObject>& InputSchema,
				const TSharedPtr<FJsonObject>& ExampleArguments,
				bool bBoundaryValue)
			{
				if (!bBoundaryValue && ExampleArguments.IsValid())
				{
					return CloneJsonObject(ExampleArguments);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				if (InputSchema.IsValid()
					&& InputSchema->TryGetObjectField(TEXT("properties"), PropertiesObject)
					&& PropertiesObject
					&& (*PropertiesObject).IsValid())
				{
					for (const FString& Key : GetSortedJsonObjectKeys(*PropertiesObject))
					{
						const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
						if ((*PropertiesObject)->TryGetObjectField(Key, PropertySchema) && PropertySchema && (*PropertySchema).IsValid())
						{
							ArgumentsObject->SetField(Key, MakeSampleJsonValueForSchema(*PropertySchema, bBoundaryValue, false));
						}
					}
				}
				return ArgumentsObject;
			}

			TSharedPtr<FJsonObject> MakeToolCallRequestObject(
				const FString& ToolName,
				const TSharedPtr<FJsonObject>& ArgumentsObject,
				int32 RequestId)
			{
				TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
				ParamsObject->SetStringField(TEXT("name"), ToolName);
				ParamsObject->SetObjectField(TEXT("arguments"), ArgumentsObject.IsValid() ? ArgumentsObject : MakeShared<FJsonObject>());

				TSharedPtr<FJsonObject> RequestObject = MakeShared<FJsonObject>();
				RequestObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
				RequestObject->SetNumberField(TEXT("id"), RequestId);
				RequestObject->SetStringField(TEXT("method"), TEXT("tools/call"));
				RequestObject->SetObjectField(TEXT("params"), ParamsObject);
				return RequestObject;
			}

			TSharedPtr<FJsonObject> MakeMcpTestCaseObject(
				const FString& Name,
				const FString& Description,
				const FString& Category,
				const TSharedPtr<FJsonObject>& RequestObject,
				bool bExpectToolCallError,
				const FString& ExpectationNote)
			{
				TSharedPtr<FJsonObject> TestCaseObject = MakeShared<FJsonObject>();
				TestCaseObject->SetStringField(TEXT("name"), Name);
				TestCaseObject->SetStringField(TEXT("description"), Description);
				TestCaseObject->SetStringField(TEXT("category"), Category);
				TestCaseObject->SetBoolField(TEXT("expectToolListed"), true);
				TestCaseObject->SetBoolField(TEXT("executeTool"), true);
				TestCaseObject->SetBoolField(TEXT("expectToolCallError"), bExpectToolCallError);
				TestCaseObject->SetStringField(TEXT("expectationNote"), ExpectationNote);
				TestCaseObject->SetObjectField(TEXT("request"), RequestObject);
				return TestCaseObject;
			}

			bool WriteMcpGeneratedTestFile(
				const FString& FilePath,
				const TSharedPtr<FJsonObject>& TestObject,
				bool bOverwrite,
				bool bDryRun,
				TArray<TSharedPtr<FJsonValue>>& OutFiles,
				FString& OutFailureReason)
			{
				TSharedPtr<FJsonObject> FileObject = MakeFileInfoObject(FilePath);
				FileObject->SetStringField(TEXT("path"), FilePath);
				FileObject->SetStringField(TEXT("name"), FPaths::GetCleanFilename(FilePath));
				FileObject->SetBoolField(TEXT("dryRun"), bDryRun);

				const FString NewText = JsonObjectToString(TestObject) + TEXT("\n");
				FString ExistingText;
				const bool bExists = FFileHelper::LoadFileToString(ExistingText, *FilePath);
				const bool bUnchanged = bExists && ExistingText == NewText;
				FileObject->SetBoolField(TEXT("exists"), bExists);
				FileObject->SetBoolField(TEXT("unchanged"), bUnchanged);

				if (bDryRun)
				{
					FileObject->SetBoolField(TEXT("wouldWrite"), !bUnchanged);
					OutFiles.Add(MakeShared<FJsonValueObject>(FileObject));
					return true;
				}

				if (bExists && !bOverwrite && !bUnchanged)
				{
					OutFailureReason = FString::Printf(TEXT("Refusing to overwrite existing test file '%s'. Set overwrite=true."), *FilePath);
					return false;
				}

				if (!bUnchanged)
				{
					if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true)
						|| !FFileHelper::SaveStringToFile(NewText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to write test file '%s'."), *FilePath);
						return false;
					}
				}

				FileObject->SetBoolField(TEXT("created"), !bExists);
				FileObject->SetBoolField(TEXT("overwritten"), bExists && !bUnchanged);
				OutFiles.Add(MakeShared<FJsonValueObject>(FileObject));
				return true;
			}

			void FindMcpTestJsonFilesRecursive(const FString& Directory, TArray<FString>& OutFiles)
			{
				OutFiles.Reset();
				if (!FPaths::DirectoryExists(Directory))
				{
					return;
				}
				IFileManager::Get().FindFilesRecursive(OutFiles, *Directory, TEXT("*.json"), true, false);
				OutFiles.Sort();
			}

			bool ResolveDefaultMcpTestRoot(
				FString& OutRoot,
				TArray<FString>& OutCandidateRoots,
				FString& OutFailureReason)
			{
				OutRoot.Reset();
				OutCandidateRoots.Reset();
				OutFailureReason.Reset();

				if (ResolveSharedRepoRoot(TEXT("UnrealMcpTests"), { TEXT("*.json") }, OutRoot, OutCandidateRoots))
				{
					return true;
				}

				OutFailureReason = FString::Printf(
					TEXT("No MCP test JSON files found under default test roots: %s."),
					*FString::Join(OutCandidateRoots, TEXT(", ")));
				return false;
			}

			FString GetDefaultMcpTestsDirectoryForRoot(const FString& Root)
			{
				const FString CoreDirectory = FPaths::Combine(Root, TEXT("Core"));
				return FPaths::DirectoryExists(CoreDirectory) ? CoreDirectory : Root;
			}

			bool ResolveMcpTestsDirectory(
				const FJsonObject& Arguments,
				FString& OutTestsDirectory,
				FString& OutScaffoldDirectory,
				FString& OutToolName,
				FString& OutFailureReason,
				TArray<FString>* OutCandidateRoots)
			{
				OutTestsDirectory.Reset();
				OutScaffoldDirectory.Reset();
				OutToolName.Reset();
				OutFailureReason.Reset();
				if (OutCandidateRoots)
				{
					OutCandidateRoots->Reset();
				}

				FString TestsDir;
				FString ToolName;
				FString ScaffoldDir;
				FString OutputRoot;
				bool bAllowDefaultTestFixtureRoot = false;
				Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
				Arguments.TryGetStringField(TEXT("toolName"), ToolName);
				Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
				Arguments.TryGetBoolField(TEXT("__allowDefaultTestFixtureRoot"), bAllowDefaultTestFixtureRoot);

				TestsDir = TestsDir.TrimStartAndEnd();
				ToolName = ToolName.TrimStartAndEnd();
				ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
				if (bAllowDefaultTestFixtureRoot && TestsDir.IsEmpty() && ToolName.IsEmpty() && ScaffoldDir.IsEmpty())
				{
					TArray<FString> CandidateRoots;
					FString DefaultRoot;
					if (!ResolveDefaultMcpTestRoot(DefaultRoot, CandidateRoots, OutFailureReason))
					{
						if (OutCandidateRoots)
						{
							*OutCandidateRoots = CandidateRoots;
						}
						return false;
					}

					if (OutCandidateRoots)
					{
						*OutCandidateRoots = CandidateRoots;
					}
					OutScaffoldDirectory = DefaultRoot;
					OutTestsDirectory = GetDefaultMcpTestsDirectoryForRoot(DefaultRoot);
					return true;
				}

				if (!TestsDir.TrimStartAndEnd().IsEmpty())
				{
					if (!ResolveProjectPathInsideProject(TestsDir, OutTestsDirectory, OutFailureReason))
					{
						return false;
					}

					OutToolName = ToolName;
					if (!ScaffoldDir.IsEmpty())
					{
						if (!ResolveProjectPathInsideProject(ScaffoldDir, OutScaffoldDirectory, OutFailureReason))
						{
							return false;
						}
					}
					else
					{
						OutScaffoldDirectory = FPaths::GetPath(OutTestsDirectory);
					}
					return true;
				}

				TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
				ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
				ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);

				if (!ResolveMcpScaffoldDirectory(*ResolveArguments, OutScaffoldDirectory, OutToolName, OutFailureReason))
				{
					return false;
				}

				if (OutTestsDirectory.IsEmpty())
				{
					OutTestsDirectory = FPaths::Combine(OutScaffoldDirectory, TEXT("Tests"));
				}
				return true;
			}

			FUnrealMcpExecutionResult GenerateMcpTests(
				const FJsonObject& Arguments,
				const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
			{
				bool bOverwrite = true;
				bool bDryRun = false;
				Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
				Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

				FString TestsDirectory;
				FString ScaffoldDirectory;
				FString ToolName;
				FString FailureReason;
				if (!ResolveMcpTestsDirectory(Arguments, TestsDirectory, ScaffoldDirectory, ToolName, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				FString SchemaJson;
				Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
				SchemaJson = SchemaJson.TrimStartAndEnd();

				TSharedPtr<FJsonObject> ExistingTestRequest;
				FString TestRequestToolName;
				FString TestRequestFailure;
				TryReadToolNameFromTestRequest(FPaths::Combine(ScaffoldDirectory, TEXT("TestRequest.json")), TestRequestToolName, ExistingTestRequest, TestRequestFailure);

				TSharedPtr<FJsonObject> ExampleArguments;
				if (ExistingTestRequest.IsValid())
				{
					const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
					if (ExistingTestRequest->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && (*ParamsObject).IsValid())
					{
						const TSharedPtr<FJsonObject>* ArgumentsObject = nullptr;
						if ((*ParamsObject)->TryGetObjectField(TEXT("arguments"), ArgumentsObject) && ArgumentsObject && (*ArgumentsObject).IsValid())
						{
							ExampleArguments = *ArgumentsObject;
						}
					}
				}

				TSharedPtr<FJsonObject> InputSchema;
				FString SchemaSource = TEXT("none");
				if (!SchemaJson.IsEmpty())
				{
					if (!LoadJsonObject(SchemaJson, InputSchema) || !InputSchema.IsValid())
					{
						return MakeExecutionResult(TEXT("schemaJson is not valid JSON."), nullptr, true);
					}
					SchemaSource = TEXT("schemaJson");
				}

				if (!InputSchema.IsValid())
				{
					const TSharedPtr<FJsonObject> ToolObject = FindToolDefinitionByName(ToolsArray, ToolName);
					const TSharedPtr<FJsonObject>* ToolInputSchema = nullptr;
					if (ToolObject.IsValid()
						&& ToolObject->TryGetObjectField(TEXT("inputSchema"), ToolInputSchema)
						&& ToolInputSchema
						&& (*ToolInputSchema).IsValid())
					{
						InputSchema = *ToolInputSchema;
						SchemaSource = TEXT("loadedToolInputSchema");
					}
				}

				if (!InputSchema.IsValid())
				{
					if (ExtractRequestedSchemaFromScaffoldReadme(ScaffoldDirectory, SchemaJson)
						&& LoadJsonObject(SchemaJson, InputSchema)
						&& InputSchema.IsValid())
					{
						SchemaSource = TEXT("scaffoldReadmeRequestedSchema");
					}
				}

				if (!InputSchema.IsValid())
				{
					InputSchema = MakeObjectSchema();
					TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					if (ExampleArguments.IsValid())
					{
						for (const FString& Key : GetSortedJsonObjectKeys(ExampleArguments))
						{
							TSharedPtr<FJsonObject> PropertySchema = MakeShared<FJsonObject>();
							const TSharedPtr<FJsonValue> Value = ExampleArguments->TryGetField(Key);
							if (Value.IsValid())
							{
								if (Value->Type == EJson::Number)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("number"));
								}
								else if (Value->Type == EJson::Boolean)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("boolean"));
								}
								else if (Value->Type == EJson::Array)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("array"));
								}
								else if (Value->Type == EJson::Object)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("object"));
								}
								else
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("string"));
								}
							}
							PropertiesObject->SetObjectField(Key, PropertySchema);
						}
					}
					InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);
					SchemaSource = TEXT("inferredFromTestRequest");
				}

				TSharedPtr<FJsonObject> HappyArguments = MakeSampleArgumentsForSchema(InputSchema, ExampleArguments, false);
				TSharedPtr<FJsonObject> BoundaryArguments = MakeSampleArgumentsForSchema(InputSchema, nullptr, true);
				TSharedPtr<FJsonObject> MissingArguments = CloneJsonObject(HappyArguments);
				TSharedPtr<FJsonObject> WrongTypeArguments = CloneJsonObject(HappyArguments);

				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				TArray<FString> PropertyKeys;
				if (InputSchema->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
				{
					PropertyKeys = GetSortedJsonObjectKeys(*PropertiesObject);
				}

				TArray<FString> RequiredFields;
				GetRequiredSchemaFields(InputSchema, RequiredFields);
				const bool bHasExplicitRequiredFields = RequiredFields.Num() > 0;
				const FString FieldToRemove = bHasExplicitRequiredFields
					? RequiredFields[0]
					: (PropertyKeys.Num() > 0 ? PropertyKeys[0] : FString());
				if (!FieldToRemove.IsEmpty())
				{
					MissingArguments->RemoveField(FieldToRemove);
				}

				FString WrongTypeField;
				if (PropertyKeys.Num() > 0)
				{
					WrongTypeField = PropertyKeys[0];
					const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
					if (PropertiesObject && (*PropertiesObject)->TryGetObjectField(WrongTypeField, PropertySchema) && PropertySchema && (*PropertySchema).IsValid())
					{
						WrongTypeArguments->SetField(WrongTypeField, MakeSampleJsonValueForSchema(*PropertySchema, false, true));
					}
				}
				else
				{
					WrongTypeArguments->SetStringField(TEXT("__wrong_type_probe"), TEXT("no declared properties"));
				}

				TArray<TSharedPtr<FJsonValue>> GeneratedFiles;
				TArray<TSharedPtr<FJsonValue>> TestCases;
				const bool bMissingShouldError = bHasExplicitRequiredFields;
				const bool bWrongTypeShouldError = PropertyKeys.Num() > 0;

				struct FGeneratedTestCase
				{
					FString FileName;
					TSharedPtr<FJsonObject> Object;
				};
				TArray<FGeneratedTestCase> Cases;
				Cases.Add({
					TEXT("valid_basic.json"),
					MakeMcpTestCaseObject(
						TEXT("valid_basic"),
						TEXT("Happy path generated from the tool schema or TestRequest.json."),
						TEXT("happy_path"),
						MakeToolCallRequestObject(ToolName, HappyArguments, 1),
						false,
						TEXT("Tool should execute without returning an MCP error."))
				});
				Cases.Add({
					TEXT("missing_required.json"),
					MakeMcpTestCaseObject(
						TEXT("missing_required"),
						FieldToRemove.IsEmpty()
							? TEXT("No schema properties were available to remove; executes the base request.")
							: FString::Printf(TEXT("Removes argument '%s' to exercise required-field handling."), *FieldToRemove),
						TEXT("missing_required"),
						MakeToolCallRequestObject(ToolName, MissingArguments, 2),
						bMissingShouldError,
						bHasExplicitRequiredFields ? TEXT("Schema declares required fields, so the tool is expected to reject the request.") : TEXT("Schema has no explicit required array; generated case is informational and may pass."))
				});
				Cases.Add({
					TEXT("boundary_values.json"),
					MakeMcpTestCaseObject(
						TEXT("boundary_values"),
						TEXT("Uses simple boundary values such as empty strings, zero numbers, false booleans, empty arrays, and empty objects."),
						TEXT("boundary"),
						MakeToolCallRequestObject(ToolName, BoundaryArguments, 3),
						false,
						TEXT("Boundary values should not produce an MCP error unless the tool adds stricter validation."))
				});
				Cases.Add({
					TEXT("wrong_type.json"),
					MakeMcpTestCaseObject(
						TEXT("wrong_type"),
						WrongTypeField.IsEmpty()
							? TEXT("No schema properties were available; adds a probe field with an unexpected shape.")
							: FString::Printf(TEXT("Sends a wrong JSON type for argument '%s'."), *WrongTypeField),
						TEXT("wrong_type"),
						MakeToolCallRequestObject(ToolName, WrongTypeArguments, 4),
						bWrongTypeShouldError,
						bWrongTypeShouldError ? TEXT("A robust tool should reject wrong JSON types.") : TEXT("No declared property exists to type-check; generated case is informational and may pass."))
				});

				for (const FGeneratedTestCase& TestCase : Cases)
				{
					const FString FilePath = FPaths::Combine(TestsDirectory, TestCase.FileName);
					if (!WriteMcpGeneratedTestFile(FilePath, TestCase.Object, bOverwrite, bDryRun, GeneratedFiles, FailureReason))
					{
						return MakeExecutionResult(FailureReason, nullptr, true);
					}
					TSharedPtr<FJsonObject> TestSummary = MakeShared<FJsonObject>();
					TestSummary->SetStringField(TEXT("name"), TestCase.Object->GetStringField(TEXT("name")));
					TestSummary->SetStringField(TEXT("path"), FilePath);
					TestSummary->SetBoolField(TEXT("expectToolCallError"), TestCase.Object->GetBoolField(TEXT("expectToolCallError")));
					TestCases.Add(MakeShared<FJsonValueObject>(TestSummary));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_generate_tests"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetStringField(TEXT("testsDir"), TestsDirectory);
				StructuredContent->SetStringField(TEXT("schemaSource"), SchemaSource);
				StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
				StructuredContent->SetBoolField(TEXT("overwrite"), bOverwrite);
				StructuredContent->SetBoolField(TEXT("hasExplicitRequiredFields"), bHasExplicitRequiredFields);
				StructuredContent->SetArrayField(TEXT("requiredFields"), MakeJsonStringArray(RequiredFields));
				StructuredContent->SetArrayField(TEXT("propertyKeys"), MakeJsonStringArray(PropertyKeys));
				StructuredContent->SetArrayField(TEXT("files"), GeneratedFiles);
				StructuredContent->SetArrayField(TEXT("testCases"), TestCases);

				return MakeExecutionResult(
					FString::Printf(TEXT("%s %d MCP test cases for %s under %s."),
						bDryRun ? TEXT("Previewed") : TEXT("Generated"),
						Cases.Num(),
						*ToolName,
						*TestsDirectory),
					StructuredContent,
					false);
			}

}
