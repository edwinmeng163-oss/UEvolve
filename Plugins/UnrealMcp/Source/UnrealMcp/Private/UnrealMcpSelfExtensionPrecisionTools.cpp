#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealMcpKnowledgeBridge.h"

namespace UnrealMcp
{
	namespace
	{
		TArray<TSharedPtr<FJsonValue>> MakeStringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		bool GetBoolArgument(const FJsonObject& Arguments, const FString& FieldName, bool DefaultValue)
		{
			bool Value = DefaultValue;
			Arguments.TryGetBoolField(FieldName, Value);
			return Value;
		}

		FString GetStringArgument(const FJsonObject& Arguments, const FString& FieldName, const FString& DefaultValue = FString())
		{
			FString Value = DefaultValue;
			Arguments.TryGetStringField(FieldName, Value);
			return Value.TrimStartAndEnd();
		}

		TArray<FString> GetStringArrayArgument(const FJsonObject& Arguments, const FString& FieldName)
		{
			TArray<FString> Values;
			const TArray<TSharedPtr<FJsonValue>>* ArrayField = nullptr;
			if (Arguments.TryGetArrayField(FieldName, ArrayField) && ArrayField)
			{
				for (const TSharedPtr<FJsonValue>& Value : *ArrayField)
				{
					if (Value.IsValid() && Value->Type == EJson::String)
					{
						Values.Add(Value->AsString());
					}
				}
			}
			return Values;
		}

		bool SaveJsonObjectToString(const TSharedPtr<FJsonObject>& Object, FString& OutText)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
			return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}

		FString MakeSnapshotRoot()
		{
			return FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("ProjectSnapshots"));
		}

		FString SanitizeSnapshotName(const FString& RequestedName)
		{
			FString Name = RequestedName.TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				Name = FString::Printf(TEXT("snapshot-%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")));
			}
			const TCHAR* InvalidChars = TEXT("\\/:*?\"<>| .");
			for (int32 Index = 0; InvalidChars[Index] != 0; ++Index)
			{
				Name.ReplaceCharInline(InvalidChars[Index], TEXT('-'));
			}
			while (Name.Contains(TEXT("--")))
			{
				Name.ReplaceInline(TEXT("--"), TEXT("-"));
			}
			return Name.Left(96);
		}

		FString ResolveSnapshotPathForRead(const FString& SnapshotPath, FString& OutFailureReason)
		{
			if (SnapshotPath.TrimStartAndEnd().IsEmpty())
			{
				OutFailureReason = TEXT("Snapshot path must not be empty.");
				return FString();
			}

			FString ResolvedPath;
			if (!ResolveProjectPathInsideProject(SnapshotPath, ResolvedPath, OutFailureReason))
			{
				return FString();
			}
			return ResolvedPath;
		}

		TArray<TSharedPtr<FJsonValue>> BuildEvidenceValuesForTask(const FString& Task)
		{
			TArray<TSharedPtr<FJsonValue>> EvidenceValues;
			if (Task.TrimStartAndEnd().IsEmpty())
			{
				return EvidenceValues;
			}

			const TArray<TSharedPtr<FJsonObject>> EvidenceObjects = BuildEvidenceForTask(Task, 3, 600);
			for (const TSharedPtr<FJsonObject>& EvidenceObject : EvidenceObjects)
			{
				if (EvidenceObject.IsValid())
				{
					EvidenceValues.Add(MakeShared<FJsonValueObject>(EvidenceObject));
				}
			}
			return EvidenceValues;
		}

		bool TryLoadOutcomeManifest(const FJsonObject& Arguments, TSharedPtr<FJsonObject>& OutManifest, FString& OutManifestPath)
		{
			FString ManifestPath = GetStringArgument(Arguments, TEXT("manifestPath"));
			const bool bExplicitManifestPath = !ManifestPath.IsEmpty();
			FString FailureReason;
			if (bExplicitManifestPath)
			{
				ManifestPath = ResolveSnapshotPathForRead(ManifestPath, FailureReason);
			}
			else
			{
				ManifestPath = GetLatestMcpExtensionManifestPath();
			}
			if (ManifestPath.IsEmpty() || !LoadJsonObjectFromFile(ManifestPath, OutManifest, FailureReason) || !OutManifest.IsValid())
			{
				return false;
			}

			if (!bExplicitManifestPath)
			{
				FString BackupDirectory;
				if (OutManifest->TryGetStringField(TEXT("backupDirectory"), BackupDirectory) && !BackupDirectory.IsEmpty())
				{
					const FString BackupManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
					if (FPaths::FileExists(BackupManifestPath))
					{
						ManifestPath = BackupManifestPath;
					}
				}
			}

			OutManifestPath = ManifestPath;
			return true;
		}

		bool IsOutcomeManifestPostcheckOk(const TSharedPtr<FJsonObject>& Manifest)
		{
			const TSharedPtr<FJsonObject>* PostcheckObject = nullptr;
			if (!Manifest.IsValid() || !Manifest->TryGetObjectField(TEXT("postcheck"), PostcheckObject) || !PostcheckObject || !(*PostcheckObject).IsValid())
			{
				return false;
			}

			bool bDescriptorSourceIntegrated = false;
			bool bHandlerSourceIntegrated = false;
			bool bRegistryPatchIntegrated = false;
			(*PostcheckObject)->TryGetBoolField(TEXT("descriptorSourceIntegrated"), bDescriptorSourceIntegrated);
			(*PostcheckObject)->TryGetBoolField(TEXT("handlerSourceIntegrated"), bHandlerSourceIntegrated);
			(*PostcheckObject)->TryGetBoolField(TEXT("registryPatchIntegrated"), bRegistryPatchIntegrated);
			return bDescriptorSourceIntegrated && bHandlerSourceIntegrated && bRegistryPatchIntegrated;
		}

		FString MakeOutcomeTitle(const TSharedPtr<FJsonObject>& Manifest, const FString& Task, const FString& SessionId)
		{
			FString ToolName;
			if (Manifest.IsValid())
			{
				Manifest->TryGetStringField(TEXT("toolName"), ToolName);
			}
			if (!ToolName.TrimStartAndEnd().IsEmpty())
			{
				return FString::Printf(TEXT("Outcome: %s"), *ToolName.TrimStartAndEnd());
			}
			if (!Task.TrimStartAndEnd().IsEmpty())
			{
				return FString::Printf(TEXT("Outcome: %s"), *Task.TrimStartAndEnd().Left(80));
			}
			return FString::Printf(TEXT("Outcome: %s"), *SessionId);
		}

		FString BuildOutcomeCardText(
			const TSharedPtr<FJsonObject>& Manifest,
			const FString& ManifestPath,
			const FString& Task,
			int32 CheckCount,
			int32 FailedChecks)
		{
			FString SessionId;
			FString ToolName;
			FString ToolId;
			FString ScaffoldDir;
			if (Manifest.IsValid())
			{
				Manifest->TryGetStringField(TEXT("sessionId"), SessionId);
				Manifest->TryGetStringField(TEXT("toolName"), ToolName);
				Manifest->TryGetStringField(TEXT("toolId"), ToolId);
				Manifest->TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			}

			TArray<FString> Lines;
			Lines.Add(FString::Printf(TEXT("Task: %s"), *Task));
			Lines.Add(FString::Printf(TEXT("Manifest sessionId: %s"), *SessionId));
			Lines.Add(FString::Printf(TEXT("Tool: %s"), *ToolName));
			Lines.Add(FString::Printf(TEXT("ToolId: %s"), *ToolId));
			Lines.Add(FString::Printf(TEXT("Scaffold: %s"), *ScaffoldDir));
			Lines.Add(FString::Printf(TEXT("Manifest: %s"), *MakePathRelativeToProject(ManifestPath)));
			Lines.Add(FString::Printf(TEXT("Verification: passed=true, checkCount=%d, failedCheckCount=%d."), CheckCount, FailedChecks));

			const TSharedPtr<FJsonObject>* PostcheckObject = nullptr;
			if (Manifest.IsValid() && Manifest->TryGetObjectField(TEXT("postcheck"), PostcheckObject) && PostcheckObject && (*PostcheckObject).IsValid())
			{
				bool bDescriptorSourceIntegrated = false;
				bool bHandlerSourceIntegrated = false;
				bool bRegistryPatchIntegrated = false;
				(*PostcheckObject)->TryGetBoolField(TEXT("descriptorSourceIntegrated"), bDescriptorSourceIntegrated);
				(*PostcheckObject)->TryGetBoolField(TEXT("handlerSourceIntegrated"), bHandlerSourceIntegrated);
				(*PostcheckObject)->TryGetBoolField(TEXT("registryPatchIntegrated"), bRegistryPatchIntegrated);
				Lines.Add(FString::Printf(
					TEXT("Postcheck: descriptorSourceIntegrated=%s, handlerSourceIntegrated=%s, registryPatchIntegrated=%s."),
					bDescriptorSourceIntegrated ? TEXT("true") : TEXT("false"),
					bHandlerSourceIntegrated ? TEXT("true") : TEXT("false"),
					bRegistryPatchIntegrated ? TEXT("true") : TEXT("false")));
			}
			return FString::Join(Lines, TEXT("\n")).Left(1800).TrimStartAndEnd();
		}

		void WriteVerifiedOutcomeCardIfReady(const FJsonObject& Arguments, const FString& Task, int32 CheckCount, int32 FailedChecks)
		{
			if (FailedChecks != 0)
			{
				return;
			}

			TSharedPtr<FJsonObject> Manifest;
			FString ManifestPath;
			if (!TryLoadOutcomeManifest(Arguments, Manifest, ManifestPath) || !IsOutcomeManifestPostcheckOk(Manifest))
			{
				return;
			}

			FString SessionId;
			Manifest->TryGetStringField(TEXT("sessionId"), SessionId);
			if (SessionId.TrimStartAndEnd().IsEmpty())
			{
				return;
			}

			FString ToolId;
			Manifest->TryGetStringField(TEXT("toolId"), ToolId);
			TArray<FString> Tags = { TEXT("outcome"), TEXT("self-extension") };
			if (!ToolId.TrimStartAndEnd().IsEmpty())
			{
				Tags.Add(ToolId.TrimStartAndEnd());
			}

			FString FailureReason;
			if (!WriteOutcomeKnowledgeCard(
				SessionId,
				MakeOutcomeTitle(Manifest, Task, SessionId),
				BuildOutcomeCardText(Manifest, ManifestPath, Task, CheckCount, FailedChecks),
				MakePathRelativeToProject(ManifestPath),
				Tags,
				FailureReason))
			{
				UE_LOG(LogUnrealMcp, Warning, TEXT("Failed to write outcome knowledge card: %s"), *FailureReason);
			}
		}

		bool FindLatestSnapshots(FString& OutBeforePath, FString& OutAfterPath)
		{
			TArray<FString> Files;
			FindImmediateChildren(MakeSnapshotRoot(), TEXT("*.json"), true, false, Files);
			Files.Sort([](const FString& Left, const FString& Right)
			{
				const FFileStatData LeftStat = IFileManager::Get().GetStatData(*Left);
				const FFileStatData RightStat = IFileManager::Get().GetStatData(*Right);
				return LeftStat.ModificationTime < RightStat.ModificationTime;
			});

			if (Files.Num() < 2)
			{
				return false;
			}
			OutBeforePath = Files[Files.Num() - 2];
			OutAfterPath = Files[Files.Num() - 1];
			return true;
		}

		TSharedPtr<FJsonObject> MakeActorSnapshotObject(const AActor* Actor)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			if (!Actor)
			{
				return Object;
			}

			const FVector Location = Actor->GetActorLocation();
			const FRotator Rotation = Actor->GetActorRotation();
			const FVector Scale = Actor->GetActorScale3D();
			Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Object->SetStringField(TEXT("name"), Actor->GetName());
			Object->SetStringField(TEXT("path"), Actor->GetPathName());
			Object->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
			Object->SetNumberField(TEXT("x"), Location.X);
			Object->SetNumberField(TEXT("y"), Location.Y);
			Object->SetNumberField(TEXT("z"), Location.Z);
			Object->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			Object->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			Object->SetNumberField(TEXT("roll"), Rotation.Roll);
			Object->SetNumberField(TEXT("scaleX"), Scale.X);
			Object->SetNumberField(TEXT("scaleY"), Scale.Y);
			Object->SetNumberField(TEXT("scaleZ"), Scale.Z);
			return Object;
		}

		TSharedPtr<FJsonObject> MakeAssetSnapshotObject(const FAssetData& AssetData)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
			Object->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
			Object->SetStringField(TEXT("assetName"), AssetData.AssetName.ToString());
			Object->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());
			return Object;
		}

		void AddAssetSnapshotArrays(
			TSharedPtr<FJsonObject>& Snapshot,
			const FString& AssetPath,
			int32 Limit,
			bool bIncludeAssets,
			bool bIncludeBlueprints,
			bool bIncludeWidgets)
		{
			TArray<TSharedPtr<FJsonValue>> Assets;
			TArray<TSharedPtr<FJsonValue>> Blueprints;
			TArray<TSharedPtr<FJsonValue>> Widgets;
			if (!bIncludeAssets && !bIncludeBlueprints && !bIncludeWidgets)
			{
				Snapshot->SetArrayField(TEXT("assets"), Assets);
				Snapshot->SetArrayField(TEXT("blueprints"), Blueprints);
				Snapshot->SetArrayField(TEXT("widgets"), Widgets);
				return;
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*AssetPath));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> AssetDataList;
			AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);
			AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.GetObjectPathString() < Right.GetObjectPathString();
			});

			const int32 SafeLimit = FMath::Max(1, Limit);
			for (const FAssetData& AssetData : AssetDataList)
			{
				const FString ClassPath = AssetData.AssetClassPath.ToString();
				const bool bIsWidget = ClassPath.Contains(TEXT("WidgetBlueprint"), ESearchCase::IgnoreCase);
				const bool bIsBlueprint = ClassPath.Contains(TEXT("Blueprint"), ESearchCase::IgnoreCase);
				TSharedPtr<FJsonObject> AssetObject = MakeAssetSnapshotObject(AssetData);

				if (bIncludeAssets && Assets.Num() < SafeLimit)
				{
					Assets.Add(MakeShared<FJsonValueObject>(AssetObject));
				}
				if (bIncludeBlueprints && bIsBlueprint && Blueprints.Num() < SafeLimit)
				{
					Blueprints.Add(MakeShared<FJsonValueObject>(AssetObject));
				}
				if (bIncludeWidgets && bIsWidget && Widgets.Num() < SafeLimit)
				{
					Widgets.Add(MakeShared<FJsonValueObject>(AssetObject));
				}
			}

			Snapshot->SetArrayField(TEXT("assets"), Assets);
			Snapshot->SetArrayField(TEXT("blueprints"), Blueprints);
			Snapshot->SetArrayField(TEXT("widgets"), Widgets);
			Snapshot->SetNumberField(TEXT("assetScanCount"), AssetDataList.Num());
		}

		void AddActorSnapshotArray(TSharedPtr<FJsonObject>& Snapshot, int32 Limit, bool bIncludeActors)
		{
			TArray<TSharedPtr<FJsonValue>> Actors;
			if (bIncludeActors)
			{
				UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
				if (ActorSubsystem)
				{
					TArray<AActor*> LevelActors = ActorSubsystem->GetAllLevelActors();
					LevelActors.Sort([](const AActor& Left, const AActor& Right)
					{
						return Left.GetPathName() < Right.GetPathName();
					});

					const int32 SafeLimit = FMath::Max(1, Limit);
					for (const AActor* Actor : LevelActors)
					{
						if (Actor && Actors.Num() < SafeLimit)
						{
							Actors.Add(MakeShared<FJsonValueObject>(MakeActorSnapshotObject(Actor)));
						}
					}
					Snapshot->SetNumberField(TEXT("levelActorScanCount"), LevelActors.Num());
				}
			}
			Snapshot->SetArrayField(TEXT("actors"), Actors);
		}

		void AddMemorySnapshotArray(TSharedPtr<FJsonObject>& Snapshot, bool bIncludeMemory)
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			if (bIncludeMemory)
			{
				TSharedPtr<FJsonObject> MemoryObject;
				FString FailureReason;
				if (LoadJsonObjectFromFile(GetProjectMemoryFilePath(), MemoryObject, FailureReason) && MemoryObject.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* MemoryEntries = nullptr;
					if (MemoryObject->TryGetArrayField(TEXT("entries"), MemoryEntries) && MemoryEntries)
					{
						for (const TSharedPtr<FJsonValue>& EntryValue : *MemoryEntries)
						{
							if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
							{
								continue;
							}
							TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
							TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
							FString Key;
							FString Status;
							FString UpdatedAt;
							Entry->TryGetStringField(TEXT("key"), Key);
							Entry->TryGetStringField(TEXT("status"), Status);
							Entry->TryGetStringField(TEXT("updatedAt"), UpdatedAt);
							Summary->SetStringField(TEXT("key"), Key);
							Summary->SetStringField(TEXT("status"), Status);
							Summary->SetStringField(TEXT("updatedAt"), UpdatedAt);
							Entries.Add(MakeShared<FJsonValueObject>(Summary));
						}
					}
				}
			}
			Snapshot->SetArrayField(TEXT("memory"), Entries);
		}

		void AddSkillSnapshotArray(TSharedPtr<FJsonObject>& Snapshot, bool bIncludeSkills)
		{
			TArray<TSharedPtr<FJsonValue>> Skills;
			if (bIncludeSkills)
			{
				const FString SkillsRoot = FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpSkills"));
				TArray<FString> SkillDirs;
				FindImmediateChildren(SkillsRoot, TEXT("*"), false, true, SkillDirs);
				for (const FString& SkillDir : SkillDirs)
				{
					const FString SkillPath = FPaths::Combine(SkillDir, TEXT("SKILL.md"));
					if (!FPaths::FileExists(SkillPath))
					{
						continue;
					}
					TSharedPtr<FJsonObject> SkillObject = MakeFileInfoObject(SkillPath);
					SkillObject->SetStringField(TEXT("name"), FPaths::GetCleanFilename(SkillDir));
					Skills.Add(MakeShared<FJsonValueObject>(SkillObject));
				}
			}
			Snapshot->SetArrayField(TEXT("skills"), Skills);
		}

		TSet<FString> ExtractIdentitySet(const TSharedPtr<FJsonObject>& Snapshot, const FString& ArrayName)
		{
			TSet<FString> Result;
			if (!Snapshot.IsValid())
			{
				return Result;
			}
			const TArray<TSharedPtr<FJsonValue>>* ArrayField = nullptr;
			if (!Snapshot->TryGetArrayField(ArrayName, ArrayField) || !ArrayField)
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value : *ArrayField)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FString Identity;
				if (!Object->TryGetStringField(TEXT("objectPath"), Identity)
					&& !Object->TryGetStringField(TEXT("path"), Identity)
					&& !Object->TryGetStringField(TEXT("key"), Identity)
					&& !Object->TryGetStringField(TEXT("name"), Identity))
				{
					continue;
				}
				if (!Identity.IsEmpty())
				{
					Result.Add(Identity);
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> MakeSetDiffObject(const TSet<FString>& Before, const TSet<FString>& After)
		{
			TArray<FString> Added;
			TArray<FString> Removed;
			TArray<FString> Common;
			for (const FString& Value : After)
			{
				if (Before.Contains(Value))
				{
					Common.Add(Value);
				}
				else
				{
					Added.Add(Value);
				}
			}
			for (const FString& Value : Before)
			{
				if (!After.Contains(Value))
				{
					Removed.Add(Value);
				}
			}
			Added.Sort();
			Removed.Sort();
			Common.Sort();

			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetNumberField(TEXT("beforeCount"), Before.Num());
			Diff->SetNumberField(TEXT("afterCount"), After.Num());
			Diff->SetNumberField(TEXT("addedCount"), Added.Num());
			Diff->SetNumberField(TEXT("removedCount"), Removed.Num());
			Diff->SetNumberField(TEXT("unchangedCount"), Common.Num());
			Diff->SetArrayField(TEXT("added"), MakeStringValues(Added));
			Diff->SetArrayField(TEXT("removed"), MakeStringValues(Removed));
			return Diff;
		}

		TSharedPtr<FJsonObject> BuildSnapshotDiffObject(const TSharedPtr<FJsonObject>& Before, const TSharedPtr<FJsonObject>& After)
		{
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			const TArray<FString> Areas = { TEXT("actors"), TEXT("assets"), TEXT("blueprints"), TEXT("widgets"), TEXT("memory"), TEXT("skills") };
			int32 AddedTotal = 0;
			int32 RemovedTotal = 0;
			for (const FString& Area : Areas)
			{
				TSharedPtr<FJsonObject> AreaDiff = MakeSetDiffObject(ExtractIdentitySet(Before, Area), ExtractIdentitySet(After, Area));
				AddedTotal += static_cast<int32>(AreaDiff->GetNumberField(TEXT("addedCount")));
				RemovedTotal += static_cast<int32>(AreaDiff->GetNumberField(TEXT("removedCount")));
				Diff->SetObjectField(Area, AreaDiff);
			}
			Diff->SetNumberField(TEXT("addedTotal"), AddedTotal);
			Diff->SetNumberField(TEXT("removedTotal"), RemovedTotal);
			Diff->SetBoolField(TEXT("changed"), AddedTotal > 0 || RemovedTotal > 0);
			return Diff;
		}

		void AddPlanStep(TArray<TSharedPtr<FJsonValue>>& Steps, const FString& Title, const FString& ToolName, const FString& Risk, bool bBackup, bool bVerify)
		{
			TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("title"), Title);
			Step->SetStringField(TEXT("tool"), ToolName);
			Step->SetStringField(TEXT("risk"), Risk);
			Step->SetBoolField(TEXT("needsBackup"), bBackup);
			Step->SetBoolField(TEXT("needsVerification"), bVerify);
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		}

		void AddErrorCategory(
			TArray<TSharedPtr<FJsonValue>>& Categories,
			const FString& Category,
			const FString& Severity,
			const FString& Evidence,
			const TArray<FString>& Suggestions)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("category"), Category);
			Object->SetStringField(TEXT("severity"), Severity);
			Object->SetStringField(TEXT("evidence"), Evidence);
			Object->SetArrayField(TEXT("suggestions"), MakeStringValues(Suggestions));
			Categories.Add(MakeShared<FJsonValueObject>(Object));
		}

		void ClassifyErrorText(const FString& ErrorText, TArray<TSharedPtr<FJsonValue>>& Categories)
		{
			const FString Lower = ErrorText.ToLower();
			if (Lower.Contains(TEXT("unrealbuildtool")) || Lower.Contains(TEXT("ubt")) || Lower.Contains(TEXT("compilerresultslog")) || Lower.Contains(TEXT("error c")) || Lower.Contains(TEXT("clang: error")))
			{
				AddErrorCategory(Categories, TEXT("ubt_compile"), TEXT("error"), TEXT("Build/compiler markers detected."), {
					TEXT("Run unreal.mcp_compile_error_fix_plan with the latest build log."),
					TEXT("Check the first compile error before editing later cascading errors."),
					TEXT("Close Unreal Editor or Live Coding before rebuilding if dylib/DLL files are locked.")
				});
			}
			if (Lower.Contains(TEXT("additionalproperties")) || Lower.Contains(TEXT("schema")) || Lower.Contains(TEXT("tools/list")) || Lower.Contains(TEXT("function calling")))
			{
				AddErrorCategory(Categories, TEXT("json_schema_or_tool_contract"), TEXT("warning"), TEXT("Schema/tool contract markers detected."), {
					TEXT("Run unreal.mcp_validate_tool_schema."),
					TEXT("Ensure object schemas set additionalProperties=false."),
					TEXT("Verify Tools/UnrealMcpToolRegistry/tools.json and handler registry coverage.")
				});
			}
			if (Lower.Contains(TEXT("jsonrpc")) || Lower.Contains(TEXT("tools/call")) || Lower.Contains(TEXT("method not found")) || Lower.Contains(TEXT("unknown tool")))
			{
				AddErrorCategory(Categories, TEXT("mcp_protocol"), TEXT("warning"), TEXT("MCP JSON-RPC markers detected."), {
					TEXT("Check tools/list for visibility and handlerName mapping."),
					TEXT("Run unreal.mcp_tool_audit."),
					TEXT("Restart Editor after C++ tool registration changes.")
				});
			}
			if (Lower.Contains(TEXT("traceback")) || Lower.Contains(TEXT("syntaxerror")) || Lower.Contains(TEXT("logpython")) || Lower.Contains(TEXT("attributeerror")))
			{
				AddErrorCategory(Categories, TEXT("ue_python"), TEXT("warning"), TEXT("Unreal Python markers detected."), {
					TEXT("Use ExecuteFile for multiline Python instead of ExecuteStatement."),
					TEXT("Check Unreal Python API names for the current engine version."),
					TEXT("Prefer fixed MCP tools over large ad hoc Python snippets when possible.")
				});
			}
			if (Lower.Contains(TEXT("http")) || Lower.Contains(TEXT("connectionerror")) || Lower.Contains(TEXT("timed out")) || Lower.Contains(TEXT("127.0.0.1:8765")))
			{
				AddErrorCategory(Categories, TEXT("http_endpoint"), TEXT("warning"), TEXT("HTTP/endpoint markers detected."), {
					TEXT("Confirm Unreal Editor is open and the MCP endpoint is listening."),
					TEXT("Use supervisor restart if the endpoint disappeared."),
					TEXT("On Windows, prefer Invoke-WebRequest plus ConvertTo-Json over fragile curl alias escaping.")
				});
			}
			if (Lower.Contains(TEXT("rate limit")) || Lower.Contains(TEXT("api key")) || Lower.Contains(TEXT("401")) || Lower.Contains(TEXT("429")) || Lower.Contains(TEXT("openai")))
			{
				AddErrorCategory(Categories, TEXT("openai_api"), TEXT("warning"), TEXT("OpenAI API markers detected."), {
					TEXT("Use the Chat toolbar Test AI Connection button."),
					TEXT("Check API key, endpoint, selected model, and organization rate limits."),
					TEXT("Retry after the rate-limit reset window if the error is 429.")
				});
			}
			if (Lower.Contains(TEXT("live coding")) || Lower.Contains(TEXT("unable to build while live coding")))
			{
				AddErrorCategory(Categories, TEXT("unreal_editor_state"), TEXT("error"), TEXT("Live Coding/build lock marker detected."), {
					TEXT("Close Unreal Editor or disable Live Coding before running UBT."),
					TEXT("Use supervisor restart after a successful build."),
					TEXT("Avoid rebuilding plugin binaries while the editor has them locked.")
				});
			}
		}
	}

	FUnrealMcpExecutionResult PreviewChangePlan(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		const FString Task = GetStringArgument(Arguments, TEXT("task"));
		if (Task.IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide task."), nullptr, true);
		}

		const FString Lower = Task.ToLower();
		TArray<TSharedPtr<FJsonValue>> Steps;
		AddPlanStep(Steps, TEXT("Capture a before snapshot for objective diffing."), TEXT("unreal.capture_project_snapshot"), TEXT("low"), false, true);
		if (Lower.Contains(TEXT("blueprint")) || Lower.Contains(TEXT("bp")) || Lower.Contains(TEXT("蓝图")))
		{
			AddPlanStep(Steps, TEXT("Inspect Blueprint graphs before editing."), TEXT("unreal.bp_list_graph_nodes"), TEXT("read_only"), false, true);
			AddPlanStep(Steps, TEXT("Apply Blueprint graph edits through fixed-schema tools."), TEXT("unreal.bp_*"), TEXT("medium"), true, true);
			AddPlanStep(Steps, TEXT("Compile and save the Blueprint, then trace important pin links."), TEXT("unreal.bp_compile_save"), TEXT("medium"), false, true);
		}
		if (Lower.Contains(TEXT("widget")) || Lower.Contains(TEXT("umg")) || Lower.Contains(TEXT("ui")) || Lower.Contains(TEXT("界面")))
		{
			AddPlanStep(Steps, TEXT("Dump the Widget tree before editing."), TEXT("unreal.widget_dump_tree"), TEXT("read_only"), false, true);
			AddPlanStep(Steps, TEXT("Apply Widget hierarchy/layout/binding edits through fixed-schema tools."), TEXT("unreal.widget_*"), TEXT("medium"), true, true);
		}
		if (Lower.Contains(TEXT("actor")) || Lower.Contains(TEXT("level")) || Lower.Contains(TEXT("map")) || Lower.Contains(TEXT("场景")) || Lower.Contains(TEXT("关卡")))
		{
			AddPlanStep(Steps, TEXT("Inspect level actors and selection state."), TEXT("unreal.list_level_actors"), TEXT("read_only"), false, true);
			AddPlanStep(Steps, TEXT("Apply actor or level mutations with postcheck evidence."), TEXT("unreal.*actor*"), TEXT("medium"), true, true);
		}
		if (Lower.Contains(TEXT("tool")) || Lower.Contains(TEXT("mcp")) || Lower.Contains(TEXT("self")) || Lower.Contains(TEXT("自扩展")))
		{
			AddPlanStep(Steps, TEXT("Audit current MCP tool health before extension."), TEXT("unreal.mcp_tool_audit"), TEXT("read_only"), false, true);
			AddPlanStep(Steps, TEXT("Use scaffold dry run, apply, build, restart, and test pipeline."), TEXT("unreal.mcp_extension_pipeline"), TEXT("high"), true, true);
		}
		AddPlanStep(Steps, TEXT("Capture an after snapshot and diff the project state."), TEXT("unreal.capture_project_snapshot + unreal.diff_project_snapshot"), TEXT("low"), false, true);
		AddPlanStep(Steps, TEXT("Verify final task outcome against expected changed areas and tool evidence."), TEXT("unreal.verify_task_outcome"), TEXT("read_only"), false, true);

		TArray<FString> MissingTools;
		for (const TSharedPtr<FJsonValue>& StepValue : Steps)
		{
			if (!StepValue.IsValid() || StepValue->Type != EJson::Object || !StepValue->AsObject().IsValid())
			{
				continue;
			}
			FString ToolName;
			StepValue->AsObject()->TryGetStringField(TEXT("tool"), ToolName);
			if (ToolName.StartsWith(TEXT("unreal.")) && !ToolName.Contains(TEXT("*")) && !ToolName.Contains(TEXT("+")) && !FindToolDefinitionByName(ToolsArray, ToolName).IsValid())
			{
				MissingTools.AddUnique(ToolName);
			}
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("preview_change_plan"));
		StructuredContent->SetStringField(TEXT("task"), Task);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("missingTools"), MakeStringValues(MissingTools));
		StructuredContent->SetBoolField(TEXT("needsBackup"), Lower.Contains(TEXT("create")) || Lower.Contains(TEXT("add")) || Lower.Contains(TEXT("modify")) || Lower.Contains(TEXT("delete")) || Lower.Contains(TEXT("新增")) || Lower.Contains(TEXT("修改")) || Lower.Contains(TEXT("删除")));
		StructuredContent->SetBoolField(TEXT("needsBuild"), Lower.Contains(TEXT("c++")) || Lower.Contains(TEXT("compile")) || Lower.Contains(TEXT("build")) || Lower.Contains(TEXT("编译")));
		StructuredContent->SetBoolField(TEXT("needsRestart"), Lower.Contains(TEXT("plugin")) || Lower.Contains(TEXT("mcp")) || Lower.Contains(TEXT("restart")) || Lower.Contains(TEXT("重启")));
		StructuredContent->SetStringField(TEXT("recommendedFirstTool"), TEXT("unreal.capture_project_snapshot"));
		const TArray<TSharedPtr<FJsonValue>> EvidenceValues = BuildEvidenceValuesForTask(Task);
		if (!EvidenceValues.IsEmpty())
		{
			StructuredContent->SetArrayField(TEXT("evidence"), EvidenceValues);
		}

		return MakeExecutionResult(FString::Printf(TEXT("Previewed change plan with %d steps."), Steps.Num()), StructuredContent, MissingTools.Num() > 0);
	}

	FUnrealMcpExecutionResult CaptureProjectSnapshot(const FJsonObject& Arguments)
	{
		const FString SnapshotName = SanitizeSnapshotName(GetStringArgument(Arguments, TEXT("snapshotName")));
		const FString AssetPath = GetStringArgument(Arguments, TEXT("assetPath"), TEXT("/Game"));
		const int32 ActorLimit = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("actorLimit"), 500), 5000);
		const int32 AssetLimit = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("assetLimit"), 1000), 10000);

		TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetStringField(TEXT("schemaVersion"), TEXT("uevolve.project_snapshot.v1"));
		Snapshot->SetStringField(TEXT("snapshotName"), SnapshotName);
		Snapshot->SetStringField(TEXT("createdAt"), FileTimeToIsoString(FDateTime::UtcNow()));
		Snapshot->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Snapshot->SetStringField(TEXT("map"), GEditor && GEditor->GetEditorWorldContext().World() ? GEditor->GetEditorWorldContext().World()->GetPathName() : FString());
		Snapshot->SetStringField(TEXT("assetPath"), AssetPath);

		AddActorSnapshotArray(Snapshot, ActorLimit, GetBoolArgument(Arguments, TEXT("includeActors"), true));
		AddAssetSnapshotArrays(
			Snapshot,
			AssetPath,
			AssetLimit,
			GetBoolArgument(Arguments, TEXT("includeAssets"), true),
			GetBoolArgument(Arguments, TEXT("includeBlueprints"), true),
			GetBoolArgument(Arguments, TEXT("includeWidgets"), true));
		AddMemorySnapshotArray(Snapshot, GetBoolArgument(Arguments, TEXT("includeMemory"), true));
		AddSkillSnapshotArray(Snapshot, GetBoolArgument(Arguments, TEXT("includeSkills"), true));

		const FString SnapshotPath = FPaths::Combine(MakeSnapshotRoot(), SnapshotName + TEXT(".json"));
		FString FailureReason;
		if (!SaveJsonObjectToFile(Snapshot, SnapshotPath, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("capture_project_snapshot"));
		StructuredContent->SetStringField(TEXT("snapshotName"), SnapshotName);
		StructuredContent->SetStringField(TEXT("snapshotPath"), SnapshotPath);
		StructuredContent->SetStringField(TEXT("snapshotRelativePath"), MakePathRelativeToProject(SnapshotPath));
		StructuredContent->SetObjectField(TEXT("snapshot"), Snapshot);
		return MakeExecutionResult(FString::Printf(TEXT("Captured project snapshot '%s'."), *SnapshotName), StructuredContent, false);
	}

	FUnrealMcpExecutionResult DiffProjectSnapshot(const FJsonObject& Arguments)
	{
		FString BeforePath = GetStringArgument(Arguments, TEXT("beforeSnapshotPath"));
		FString AfterPath = GetStringArgument(Arguments, TEXT("afterSnapshotPath"));
		if (BeforePath.IsEmpty() || AfterPath.IsEmpty())
		{
			if (!FindLatestSnapshots(BeforePath, AfterPath))
			{
				return MakeExecutionResult(TEXT("Provide beforeSnapshotPath and afterSnapshotPath, or capture at least two snapshots first."), nullptr, true);
			}
		}

		FString FailureReason;
		BeforePath = ResolveSnapshotPathForRead(BeforePath, FailureReason);
		if (BeforePath.IsEmpty())
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		AfterPath = ResolveSnapshotPathForRead(AfterPath, FailureReason);
		if (AfterPath.IsEmpty())
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> Before;
		TSharedPtr<FJsonObject> After;
		if (!LoadJsonObjectFromFile(BeforePath, Before, FailureReason) || !Before.IsValid())
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		if (!LoadJsonObjectFromFile(AfterPath, After, FailureReason) || !After.IsValid())
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> Diff = BuildSnapshotDiffObject(Before, After);
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("diff_project_snapshot"));
		StructuredContent->SetStringField(TEXT("beforeSnapshotPath"), BeforePath);
		StructuredContent->SetStringField(TEXT("afterSnapshotPath"), AfterPath);
		StructuredContent->SetObjectField(TEXT("diff"), Diff);

		return MakeExecutionResult(
			FString::Printf(TEXT("Snapshot diff changed=%s added=%d removed=%d."),
				Diff->GetBoolField(TEXT("changed")) ? TEXT("true") : TEXT("false"),
				static_cast<int32>(Diff->GetNumberField(TEXT("addedTotal"))),
				static_cast<int32>(Diff->GetNumberField(TEXT("removedTotal")))),
			StructuredContent,
			false);
	}

	FUnrealMcpExecutionResult VerifyTaskOutcome(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		const FString Task = GetStringArgument(Arguments, TEXT("task"));
		if (Task.IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide task."), nullptr, true);
		}

		TArray<TSharedPtr<FJsonValue>> Checks;
		int32 FailedChecks = 0;
		auto AddCheck = [&Checks, &FailedChecks](const FString& Name, bool bPassed, const FString& Details)
		{
			TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
			Check->SetStringField(TEXT("name"), Name);
			Check->SetBoolField(TEXT("passed"), bPassed);
			Check->SetStringField(TEXT("details"), Details);
			Checks.Add(MakeShared<FJsonValueObject>(Check));
			FailedChecks += bPassed ? 0 : 1;
		};

		const TArray<FString> ExpectedTools = GetStringArrayArgument(Arguments, TEXT("expectedTools"));
		for (const FString& ToolName : ExpectedTools)
		{
			AddCheck(FString::Printf(TEXT("tool_listed:%s"), *ToolName), FindToolDefinitionByName(ToolsArray, ToolName).IsValid(), TEXT("Expected tool should appear in tools/list."));
		}

		const FString BeforePath = GetStringArgument(Arguments, TEXT("beforeSnapshotPath"));
		const FString AfterPath = GetStringArgument(Arguments, TEXT("afterSnapshotPath"));
		if (!BeforePath.IsEmpty() && !AfterPath.IsEmpty())
		{
			TSharedPtr<FJsonObject> Before;
			TSharedPtr<FJsonObject> After;
			FString FailureReason;
			FString ResolvedBefore = ResolveSnapshotPathForRead(BeforePath, FailureReason);
			FString ResolvedAfter = ResolveSnapshotPathForRead(AfterPath, FailureReason);
			const bool bLoadedSnapshots = !ResolvedBefore.IsEmpty() && !ResolvedAfter.IsEmpty()
				&& LoadJsonObjectFromFile(ResolvedBefore, Before, FailureReason)
				&& LoadJsonObjectFromFile(ResolvedAfter, After, FailureReason);
			AddCheck(TEXT("snapshots_loaded"), bLoadedSnapshots, bLoadedSnapshots ? TEXT("Before/after snapshots loaded.") : FailureReason);
			if (bLoadedSnapshots)
			{
				TSharedPtr<FJsonObject> Diff = BuildSnapshotDiffObject(Before, After);
				const TArray<FString> ExpectedAreas = GetStringArrayArgument(Arguments, TEXT("expectedChangedAreas"));
				for (const FString& Area : ExpectedAreas)
				{
					const TSharedPtr<FJsonObject>* AreaDiff = nullptr;
					const bool bAreaChanged = Diff->TryGetObjectField(Area, AreaDiff)
						&& AreaDiff
						&& (*AreaDiff).IsValid()
						&& ((*AreaDiff)->GetNumberField(TEXT("addedCount")) > 0 || (*AreaDiff)->GetNumberField(TEXT("removedCount")) > 0);
					AddCheck(FString::Printf(TEXT("snapshot_area_changed:%s"), *Area), bAreaChanged, TEXT("Expected area should show added or removed identities."));
				}
			}
		}

		const FString RequiredText = GetStringArgument(Arguments, TEXT("requiredEvidenceText"));
		const FString EvidenceText = GetStringArgument(Arguments, TEXT("evidenceText"));
		if (!RequiredText.IsEmpty())
		{
			AddCheck(TEXT("required_evidence_text"), EvidenceText.Contains(RequiredText, ESearchCase::IgnoreCase), FString::Printf(TEXT("Evidence should contain '%s'."), *RequiredText));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("verify_task_outcome"));
		StructuredContent->SetStringField(TEXT("task"), Task);
		StructuredContent->SetNumberField(TEXT("checkCount"), Checks.Num());
		StructuredContent->SetNumberField(TEXT("failedCheckCount"), FailedChecks);
		StructuredContent->SetBoolField(TEXT("passed"), FailedChecks == 0);
		StructuredContent->SetArrayField(TEXT("checks"), Checks);
		const TArray<TSharedPtr<FJsonValue>> EvidenceValues = BuildEvidenceValuesForTask(Task);
		if (!EvidenceValues.IsEmpty())
		{
			StructuredContent->SetArrayField(TEXT("evidence"), EvidenceValues);
		}
		WriteVerifiedOutcomeCardIfReady(Arguments, Task, Checks.Num(), FailedChecks);
		return MakeExecutionResult(FString::Printf(TEXT("Task verification %s: %d checks, %d failed."), FailedChecks == 0 ? TEXT("passed") : TEXT("failed"), Checks.Num(), FailedChecks), StructuredContent, FailedChecks > 0);
	}

	FUnrealMcpExecutionResult ClassifyMcpError(const FJsonObject& Arguments)
	{
		FString ErrorText = GetStringArgument(Arguments, TEXT("text"));
		const FString LogPathArg = GetStringArgument(Arguments, TEXT("logPath"));
		const int32 TailLineCount = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("tailLines"), 200), 2000);
		FString ResolvedLogPath;
		FString FailureReason;
		if (!LogPathArg.IsEmpty())
		{
			ResolvedLogPath = ResolveSnapshotPathForRead(LogPathArg, FailureReason);
			if (ResolvedLogPath.IsEmpty())
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			FString LogText;
			if (!FFileHelper::LoadFileToString(LogText, *ResolvedLogPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read logPath '%s'."), *ResolvedLogPath), nullptr, true);
			}
			ErrorText += TEXT("\n") + TailLines(LogText, TailLineCount);
		}
		if (ErrorText.TrimStartAndEnd().IsEmpty())
		{
			return MakeExecutionResult(TEXT("Provide text or logPath."), nullptr, true);
		}

		TArray<TSharedPtr<FJsonValue>> Categories;
		ClassifyErrorText(ErrorText, Categories);
		if (Categories.Num() == 0)
		{
			AddErrorCategory(Categories, TEXT("unknown"), TEXT("info"), TEXT("No known markers detected."), {
				TEXT("Capture the full failing tool response or build log."),
				TEXT("Run unreal.mcp_workbench_status and unreal.mcp_tool_audit."),
				TEXT("If this is a new failure mode, add a classifier fixture before automating a fix.")
			});
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_classify_error"));
		StructuredContent->SetStringField(TEXT("logPath"), ResolvedLogPath);
		StructuredContent->SetNumberField(TEXT("categoryCount"), Categories.Num());
		StructuredContent->SetArrayField(TEXT("categories"), Categories);
		return MakeExecutionResult(FString::Printf(TEXT("Classified error into %d category group(s)."), Categories.Num()), StructuredContent, false);
	}

	FUnrealMcpExecutionResult PrepareTestSandbox(const FJsonObject& Arguments)
	{
		const FString ContentPath = GetStringArgument(Arguments, TEXT("contentPath"), TEXT("/Game/__UEvolveMcpTest"));
		const bool bReset = GetBoolArgument(Arguments, TEXT("reset"), true);
		const bool bResetActors = GetBoolArgument(Arguments, TEXT("resetActors"), false);
		const FString ActorLabelPrefix = GetStringArgument(Arguments, TEXT("actorLabelPrefix"), TEXT("UEvolveMcpTest_"));
		const bool bDryRun = GetBoolArgument(Arguments, TEXT("dryRun"), false);
		if (!ContentPath.StartsWith(TEXT("/Game/__UEvolve"), ESearchCase::CaseSensitive))
		{
			return MakeExecutionResult(TEXT("contentPath must be under /Game/__UEvolve* for sandbox safety."), nullptr, true);
		}
		if (bResetActors && !ActorLabelPrefix.StartsWith(TEXT("UEvolveMcpTest_"), ESearchCase::CaseSensitive))
		{
			return MakeExecutionResult(TEXT("actorLabelPrefix must start with UEvolveMcpTest_ when resetActors=true."), nullptr, true);
		}

		UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		if (!EditorAssetSubsystem)
		{
			return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		UEditorActorSubsystem* EditorActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
		if (bResetActors && !EditorActorSubsystem)
		{
			return MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
		}

		const bool bExistedBefore = EditorAssetSubsystem->DoesDirectoryExist(ContentPath);
		int32 DeletedAssetCount = 0;
		int32 ActorCandidateCount = 0;
		int32 DeletedActorCount = 0;
		bool bDeleted = false;
		bool bCreated = false;
		if (bResetActors && EditorActorSubsystem)
		{
			TArray<AActor*> ActorsToDelete;
			for (AActor* Actor : EditorActorSubsystem->GetAllLevelActors())
			{
				if (!Actor)
				{
					continue;
				}
				const FString Label = Actor->GetActorLabel();
				if (Label.StartsWith(ActorLabelPrefix, ESearchCase::CaseSensitive))
				{
					ActorsToDelete.Add(Actor);
				}
			}
			ActorCandidateCount = ActorsToDelete.Num();
			if (!bDryRun && ActorsToDelete.Num() > 0)
			{
				for (AActor* Actor : ActorsToDelete)
				{
					if (Actor && EditorActorSubsystem->DestroyActor(Actor))
					{
						++DeletedActorCount;
					}
				}
			}
		}
		if (!bDryRun)
		{
			if (bReset)
			{
				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				FARFilter Filter;
				Filter.PackagePaths.Add(FName(*ContentPath));
				Filter.bRecursivePaths = true;
				TArray<FAssetData> AssetsToDelete;
				AssetRegistryModule.Get().GetAssets(Filter, AssetsToDelete);
				AssetsToDelete.Sort([](const FAssetData& Left, const FAssetData& Right)
				{
					return Left.GetObjectPathString() > Right.GetObjectPathString();
				});
				for (const FAssetData& AssetData : AssetsToDelete)
				{
					if (EditorAssetSubsystem->DeleteAsset(AssetData.GetObjectPathString()))
					{
						++DeletedAssetCount;
					}
				}
				bDeleted = EditorAssetSubsystem->DeleteDirectory(ContentPath);
			}
			if (!EditorAssetSubsystem->DoesDirectoryExist(ContentPath))
			{
				bCreated = EditorAssetSubsystem->MakeDirectory(ContentPath);
			}
		}

		const bool bExistsAfter = EditorAssetSubsystem->DoesDirectoryExist(ContentPath);
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_prepare_test_sandbox"));
		StructuredContent->SetStringField(TEXT("contentPath"), ContentPath);
		StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
		StructuredContent->SetBoolField(TEXT("reset"), bReset);
		StructuredContent->SetBoolField(TEXT("resetActors"), bResetActors);
		StructuredContent->SetStringField(TEXT("actorLabelPrefix"), ActorLabelPrefix);
		StructuredContent->SetBoolField(TEXT("existedBefore"), bExistedBefore);
		StructuredContent->SetNumberField(TEXT("deletedAssetCount"), DeletedAssetCount);
		StructuredContent->SetNumberField(TEXT("actorCandidateCount"), ActorCandidateCount);
		StructuredContent->SetNumberField(TEXT("deletedActorCount"), DeletedActorCount);
		StructuredContent->SetBoolField(TEXT("deleted"), bDeleted);
		StructuredContent->SetBoolField(TEXT("created"), bCreated);
		StructuredContent->SetBoolField(TEXT("existsAfter"), bExistsAfter);
		return MakeExecutionResult(
			FString::Printf(TEXT("Prepared test sandbox %s. dryRun=%s existsAfter=%s"), *ContentPath, bDryRun ? TEXT("true") : TEXT("false"), bExistsAfter ? TEXT("true") : TEXT("false")),
			StructuredContent,
			!bDryRun && !bExistsAfter);
	}
}
