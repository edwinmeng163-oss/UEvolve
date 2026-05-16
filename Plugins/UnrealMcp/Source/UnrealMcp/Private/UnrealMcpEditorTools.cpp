#include "UnrealMcpEditorTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorScriptingHelpers.h"
#include "Engine/RendererSettings.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/InputSettings.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "IContentBrowserSingleton.h"
#include "IAssetTools.h"
#include "IPythonScriptPlugin.h"
#include "Logging/MessageLog.h"
#include "Modules/ModuleManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UnrealMcpEngineCompat.h"
#include "UnrealMcpModule.h"
#include "UnrealMcpSettings.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UnrealType.h"

namespace UnrealMcp
{
	FString NormalizeEndpointPath(const FString& EndpointPath);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FUnrealMcpExecutionResult ExecuteEditorEngineVersion();
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	TArray<FAssetData> GetSelectedAssets();
	TSharedPtr<FJsonObject> MakeAssetObject(const FAssetData& Asset);
	FString DescribeAsset(const FAssetData& Asset);
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
		static constexpr int32 EditorToolDefaultListLimit = 200;

		// Forward decls so the chunk 5 migration tools (asset_move,
		// redirector_fixup, dependency_remap, project_version_migration) at
		// lines ~675/797/932/1071 can reference the PIE-block helpers whose
		// definitions live below at ~1271/1279 in this same anonymous
		// namespace.
		bool IsEditorPlaying();
		FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);

		struct FEditorToolAssetPath
		{
			FString PackageName;
			FString ObjectPath;
			FString PackagePath;
			FString AssetName;
		};

		void EditorToolAttachError(TSharedPtr<FJsonObject> StructuredContent, const FString& Code, const FString& Message)
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

		FUnrealMcpExecutionResult MakeEditorToolStructuredError(
			const FString& Code,
			const FString& Message,
			TSharedPtr<FJsonObject> StructuredContent)
		{
			if (!StructuredContent.IsValid())
			{
				StructuredContent = MakeShared<FJsonObject>();
			}
			EditorToolAttachError(StructuredContent, Code, Message);
			return MakeExecutionResult(Message, StructuredContent, true);
		}

		FUnrealMcpExecutionResult MakeProjectSettingsError(
			const FString& Code,
			const FString& Message,
			const FString& Category,
			const FString& Key,
			const FString& SourceConfig = FString())
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("category"), Category);
			StructuredContent->SetStringField(TEXT("key"), Key);
			if (!SourceConfig.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("sourceConfig"), SourceConfig);
			}
			EditorToolAttachError(StructuredContent, Code, Message);
			return MakeExecutionResult(Message, StructuredContent, true);
		}

		bool EditorToolIsContainerProperty(const FProperty* Property)
		{
			return Property
				&& (Property->IsA<FArrayProperty>()
					|| Property->IsA<FSetProperty>()
					|| Property->IsA<FMapProperty>());
		}

		TArray<TSharedPtr<FJsonValue>> EditorToolMakeJsonStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		void AddProjectSettingsSection(TArray<FString>& Sections, const FString& Section)
		{
			if (!Section.IsEmpty())
			{
				Sections.Add(Section);
			}
		}

		FString NormalizeProjectSettingsKey(const FString& Category, const FString& Key, FString& OutNotes)
		{
			OutNotes.Reset();

			if (Category == TEXT("input"))
			{
				if (Key.Equals(TEXT("DefaultInputAxisMappings"), ESearchCase::IgnoreCase))
				{
					OutNotes = TEXT("Resolved DefaultInputAxisMappings to UInputSettings.AxisMappings.");
					return TEXT("AxisMappings");
				}
				if (Key.Equals(TEXT("DefaultInputActionMappings"), ESearchCase::IgnoreCase))
				{
					OutNotes = TEXT("Resolved DefaultInputActionMappings to UInputSettings.ActionMappings.");
					return TEXT("ActionMappings");
				}
			}

			if (Category == TEXT("game") && Key.Equals(TEXT("DefaultGameMode"), ESearchCase::IgnoreCase))
			{
				OutNotes = TEXT("Resolved DefaultGameMode to GameMapsSettings.GlobalDefaultGameMode.");
				return TEXT("GlobalDefaultGameMode");
			}

			return Key;
		}

		bool TryReadConfigSetting(
			const FString& ConfigFilename,
			const TArray<FString>& Sections,
			const FString& Key,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutSection)
		{
			if (!GConfig || ConfigFilename.IsEmpty() || Key.IsEmpty())
			{
				return false;
			}

			for (const FString& Section : Sections)
			{
				FString StringValue;
				if (GConfig->GetString(*Section, *Key, StringValue, ConfigFilename))
				{
					OutValue = MakeShared<FJsonValueString>(StringValue);
					OutSection = Section;
					return true;
				}

				TArray<FString> ArrayValues;
				if (GConfig->GetArray(*Section, *Key, ArrayValues, ConfigFilename) > 0)
				{
					OutValue = MakeShared<FJsonValueArray>(EditorToolMakeJsonStringArray(ArrayValues));
					OutSection = Section;
					return true;
				}
			}

			return false;
		}

		bool EditorToolTryNormalizeAssetPath(const FString& RawPath, FEditorToolAssetPath& OutPath, FString& OutFailureReason)
		{
			OutPath = FEditorToolAssetPath();
			OutFailureReason.Reset();

			FString Path = RawPath.TrimStartAndEnd();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));

			if (Path.IsEmpty())
			{
				OutFailureReason = TEXT("Asset path is required.");
				return false;
			}

			if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
			{
				OutFailureReason = TEXT("Use a UE package/object path without the .uasset extension.");
				return false;
			}

			if (Path.Contains(TEXT(":")))
			{
				OutFailureReason = TEXT("Subobject paths are not supported; provide a top-level asset path.");
				return false;
			}

			if (!Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
			{
				OutFailureReason = TEXT("Asset path must start with /Game/.");
				return false;
			}

			int32 LastSlashIndex = INDEX_NONE;
			Path.FindLastChar(TEXT('/'), LastSlashIndex);

			int32 DotIndex = INDEX_NONE;
			const bool bHasObjectName = Path.FindChar(TEXT('.'), DotIndex) && DotIndex > LastSlashIndex;
			FString PackageName = bHasObjectName ? Path.Left(DotIndex) : Path;

			FText InvalidPackageReason;
			if (!FPackageName::IsValidLongPackageName(PackageName, false, &InvalidPackageReason))
			{
				OutFailureReason = InvalidPackageReason.IsEmpty()
					? FString::Printf(TEXT("Invalid long package name '%s'."), *PackageName)
					: InvalidPackageReason.ToString();
				return false;
			}

			const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
			if (AssetName.IsEmpty())
			{
				OutFailureReason = FString::Printf(TEXT("Asset path '%s' does not include an asset name."), *Path);
				return false;
			}

			if (bHasObjectName)
			{
				const FString ObjectName = Path.Mid(DotIndex + 1);
				if (!ObjectName.Equals(AssetName, ESearchCase::CaseSensitive))
				{
					OutFailureReason = FString::Printf(
						TEXT("Object name '%s' must match package asset name '%s'."),
						*ObjectName,
						*AssetName);
					return false;
				}
			}

			OutPath.PackageName = PackageName;
			OutPath.ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
			OutPath.PackagePath = FPackageName::GetLongPackagePath(PackageName);
			OutPath.AssetName = AssetName;
			return true;
		}

		bool EditorToolTryNormalizeGameContentPath(const FString& RawPath, FString& OutPath, FString& OutFailureReason)
		{
			OutPath.Reset();
			OutFailureReason.Reset();

			FString Path = RawPath.TrimStartAndEnd();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Path.IsEmpty())
			{
				Path = TEXT("/Game");
			}
			while (Path.Len() > 5 && Path.EndsWith(TEXT("/")))
			{
				Path.LeftChopInline(1);
			}

			if (Path != TEXT("/Game") && !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
			{
				OutFailureReason = TEXT("Path must be /Game or a /Game/... content subtree.");
				return false;
			}

			if (Path.Contains(TEXT(".")) || Path.Contains(TEXT(":")))
			{
				OutFailureReason = TEXT("Path must be a Content Browser folder path, not an asset or subobject path.");
				return false;
			}

			const FString ProbePackageName = Path == TEXT("/Game")
				? TEXT("/Game/__UEvolvePathProbe")
				: Path / TEXT("__UEvolvePathProbe");
			FText InvalidPackageReason;
			if (!FPackageName::IsValidLongPackageName(ProbePackageName, false, &InvalidPackageReason))
			{
				OutFailureReason = InvalidPackageReason.IsEmpty()
					? FString::Printf(TEXT("Invalid content path '%s'."), *Path)
					: InvalidPackageReason.ToString();
				return false;
			}

			OutPath = Path;
			return true;
		}

		TArray<FName> EditorToolGetReferencingPackages(IAssetRegistry& AssetRegistry, const FString& PackageName)
		{
			TArray<FName> Referencers;
			AssetRegistry.GetReferencers(
				FName(*PackageName),
				Referencers,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::FDependencyQuery());
			return Referencers;
		}

		UObjectRedirector* EditorToolLoadRedirectorAtPath(IAssetRegistry& AssetRegistry, const FString& ObjectPath)
		{
			if (UObjectRedirector* LoadedRedirector = FindObject<UObjectRedirector>(nullptr, *ObjectPath))
			{
				return LoadedRedirector;
			}

			const FAssetData RedirectorData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			if (RedirectorData.IsValid() && RedirectorData.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName())
			{
				return Cast<UObjectRedirector>(RedirectorData.GetAsset());
			}

			return nullptr;
		}

		bool EditorToolRedirectorExistsAtPath(IAssetRegistry& AssetRegistry, const FString& ObjectPath)
		{
			return EditorToolLoadRedirectorAtPath(AssetRegistry, ObjectPath) != nullptr;
		}

		UObject* EditorToolLoadAssetForMigration(
			UEditorAssetSubsystem* EditorAssetSubsystem,
			const FString& ObjectPath,
			FString& OutFailureReason)
		{
			if (!EditorAssetSubsystem)
			{
				OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
				return nullptr;
			}

			UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(ObjectPath);
			if (!LoadedAsset)
			{
				OutFailureReason = FString::Printf(TEXT("Asset '%s' was not found or could not be loaded."), *ObjectPath);
			}
			return LoadedAsset;
		}

		bool EditorToolIsAssetDeletedOrRedirected(IAssetRegistry& AssetRegistry, UEditorAssetSubsystem* EditorAssetSubsystem, const FString& ObjectPath)
		{
			const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			if (AssetData.IsValid() && AssetData.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName())
			{
				return true;
			}

			return !EditorAssetSubsystem || !EditorAssetSubsystem->DoesAssetExist(ObjectPath);
		}

		TArray<TSharedPtr<FJsonValue>> EditorToolMakeRedirectorFailureArray(const TArray<TPair<FString, FString>>& Failures)
		{
			TArray<TSharedPtr<FJsonValue>> FailureValues;
			for (const TPair<FString, FString>& Failure : Failures)
			{
				TSharedPtr<FJsonObject> FailureObject = MakeShared<FJsonObject>();
				FailureObject->SetStringField(TEXT("redirectorPath"), Failure.Key);
				FailureObject->SetStringField(TEXT("reason"), Failure.Value);
				FailureValues.Add(MakeShared<FJsonValueObject>(FailureObject));
			}
			return FailureValues;
		}

		bool EditorToolTryParseVersionTriplet(const FString& Version, int32& OutMajor, int32& OutMinor, int32& OutPatch)
		{
			TArray<FString> Parts;
			Version.ParseIntoArray(Parts, TEXT("."), true);
			if (Parts.Num() < 2 || !LexTryParseString(OutMajor, *Parts[0]) || !LexTryParseString(OutMinor, *Parts[1]))
			{
				return false;
			}

			OutPatch = 0;
			if (Parts.Num() >= 3 && !LexTryParseString(OutPatch, *Parts[2]))
			{
				return false;
			}
			return true;
		}

		int32 EditorToolCompareVersionTriplets(const FString& Left, const FString& Right)
		{
			int32 LeftMajor = 0;
			int32 LeftMinor = 0;
			int32 LeftPatch = 0;
			int32 RightMajor = 0;
			int32 RightMinor = 0;
			int32 RightPatch = 0;
			if (!EditorToolTryParseVersionTriplet(Left, LeftMajor, LeftMinor, LeftPatch)
				|| !EditorToolTryParseVersionTriplet(Right, RightMajor, RightMinor, RightPatch))
			{
				return 0;
			}

			if (LeftMajor != RightMajor)
			{
				return LeftMajor < RightMajor ? -1 : 1;
			}
			if (LeftMinor != RightMinor)
			{
				return LeftMinor < RightMinor ? -1 : 1;
			}
			if (LeftPatch != RightPatch)
			{
				return LeftPatch < RightPatch ? -1 : 1;
			}
			return 0;
		}

		bool EditorToolTryLoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason)
		{
			OutObject.Reset();
			OutFailureReason.Reset();

			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to read JSON file '%s'."), *FilePath);
				return false;
			}

			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
			{
				OutFailureReason = FString::Printf(TEXT("Invalid JSON in '%s'."), *FilePath);
				return false;
			}

			return true;
		}

		bool EditorToolTrySaveJsonObjectToFile(const FString& FilePath, const TSharedRef<FJsonObject>& JsonObject, FString& OutFailureReason)
		{
			OutFailureReason.Reset();

			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			if (!FJsonSerializer::Serialize(JsonObject, Writer))
			{
				OutFailureReason = TEXT("Failed to serialize project JSON.");
				return false;
			}

			if (!FFileHelper::SaveStringToFile(JsonText, *FilePath))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write '%s'."), *FilePath);
				return false;
			}

			return true;
		}

		FUnrealMcpExecutionResult ExecuteEditorStatus()
		{
			UEditorActorSubsystem* EditorActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString CurrentMap = EditorWorld ? EditorWorld->GetOutermost()->GetName() : TEXT("");
			const bool bIsPIE = GEditor && GEditor->PlayWorld != nullptr;
			const bool bIsSimulating = GEditor && GEditor->bIsSimulatingInEditor;
			const bool bPlayRequestPending = GEditor && GEditor->GetPlaySessionRequest().IsSet();
			const FString EngineVersion = FEngineVersion::Current().ToString();
			const TArray<FAssetData> SelectedAssets = GetSelectedAssets();
			const int32 SelectedActorCount = EditorActorSubsystem ? EditorActorSubsystem->GetSelectedLevelActors().Num() : 0;

			const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
			const FString EndpointUrl = FString::Printf(TEXT("http://127.0.0.1:%d%s"), Settings->Port, *NormalizeEndpointPath(Settings->EndpointPath));

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("projectName"), FApp::GetProjectName());
			StructuredContent->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
			StructuredContent->SetStringField(TEXT("engineVersion"), EngineVersion);
			StructuredContent->SetStringField(TEXT("currentMap"), CurrentMap);
			StructuredContent->SetBoolField(TEXT("isPlayInEditor"), bIsPIE);
			StructuredContent->SetBoolField(TEXT("isSimulatingInEditor"), bIsSimulating);
			StructuredContent->SetBoolField(TEXT("playRequestPending"), bPlayRequestPending);
			StructuredContent->SetNumberField(TEXT("selectedAssetCount"), SelectedAssets.Num());
			StructuredContent->SetNumberField(TEXT("selectedActorCount"), SelectedActorCount);
			StructuredContent->SetStringField(TEXT("endpoint"), EndpointUrl);

			const FString Text = FString::Printf(
				TEXT("Project: %s\nEngine: %s\nMap: %s\nPIE: %s\nSimulating: %s\nPlay request pending: %s\nSelected assets: %d\nSelected actors: %d\nEndpoint: %s"),
				FApp::GetProjectName(),
				*EngineVersion,
				CurrentMap.IsEmpty() ? TEXT("<none>") : *CurrentMap,
				bIsPIE ? TEXT("true") : TEXT("false"),
				bIsSimulating ? TEXT("true") : TEXT("false"),
				bPlayRequestPending ? TEXT("true") : TEXT("false"),
				SelectedAssets.Num(),
				SelectedActorCount,
				*EndpointUrl);

			return MakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteProjectSettingsGet(const FJsonObject& Arguments)
		{
			FString Category;
			FString Key;
			Arguments.TryGetStringField(TEXT("category"), Category);
			Arguments.TryGetStringField(TEXT("key"), Key);
			Category = Category.TrimStartAndEnd().ToLower();
			Key = Key.TrimStartAndEnd();

			if (Key.IsEmpty())
			{
				return MakeProjectSettingsError(TEXT("KEY_NOT_FOUND"), TEXT("key is required."), Category, Key);
			}

			UObject* SettingsObject = nullptr;
			FString SourceConfig;
			FString ConfigFilename;
			TArray<FString> ConfigSections;

			if (Category == TEXT("input"))
			{
				SettingsObject = GetMutableDefault<UInputSettings>();
				SourceConfig = TEXT("DefaultInput.ini");
				ConfigFilename = GInputIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/Engine.InputSettings"));
			}
			else if (Category == TEXT("rendering"))
			{
				SettingsObject = GetMutableDefault<URendererSettings>();
				SourceConfig = TEXT("DefaultEngine.ini");
				ConfigFilename = GEngineIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/Engine.RendererSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/MacTargetPlatform.MacTargetSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/LinuxTargetPlatform.LinuxTargetSettings"));
			}
			else if (Category == TEXT("game"))
			{
				SourceConfig = TEXT("DefaultEngine.ini");
				ConfigFilename = GEngineIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/EngineSettings.GameMapsSettings"));
			}
			else if (Category == TEXT("engine"))
			{
				SourceConfig = TEXT("DefaultEngine.ini");
				ConfigFilename = GEngineIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/Engine.Engine"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/EngineSettings.GameMapsSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/Engine.RendererSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/MacTargetPlatform.MacTargetSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/LinuxTargetPlatform.LinuxTargetSettings"));
			}
			else if (Category == TEXT("editor"))
			{
				SourceConfig = TEXT("DefaultEditor.ini");
				ConfigFilename = GEditorIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/UnrealEd.EditorLoadingSavingSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/UnrealEd.LevelEditorViewportSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/UnrealEd.EditorPerformanceSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/UnrealEd.EditorExperimentalSettings"));
			}
			else if (Category == TEXT("physics"))
			{
				SourceConfig = TEXT("DefaultEngine.ini");
				ConfigFilename = GEngineIni;
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/Engine.PhysicsSettings"));
				AddProjectSettingsSection(ConfigSections, TEXT("/Script/PhysicsCore.PhysicsSettings"));
			}
			else
			{
				const FString Message = FString::Printf(TEXT("Unknown project settings category '%s'."), *Category);
				return MakeProjectSettingsError(TEXT("UNKNOWN_CATEGORY"), Message, Category, Key);
			}

			FString Notes;
			const FString LookupKey = NormalizeProjectSettingsKey(Category, Key, Notes);

			if (SettingsObject)
			{
				UObject* OwnerObject = nullptr;
				FProperty* LeafProperty = nullptr;
				FProperty* NotifyProperty = nullptr;
				void* ValuePtr = nullptr;
				FString FailureReason;
				if (ResolveObjectPropertyPath(SettingsObject, LookupKey, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, FailureReason))
				{
					const TSharedPtr<FJsonValue> JsonValue = PropertyValueToJson(LeafProperty, ValuePtr);
					if (!JsonValue.IsValid())
					{
						const FString Message = FString::Printf(TEXT("Failed to serialize project setting '%s': unsupported property type."), *Key);
						return MakeProjectSettingsError(TEXT("READ_FAILED"), Message, Category, Key, SourceConfig);
					}

					TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
					StructuredContent->SetStringField(TEXT("category"), Category);
					StructuredContent->SetStringField(TEXT("key"), Key);
					StructuredContent->SetField(TEXT("value"), JsonValue);
					StructuredContent->SetStringField(TEXT("sourceConfig"), SourceConfig);
					if (!Notes.IsEmpty())
					{
						StructuredContent->SetStringField(TEXT("notes"), Notes);
					}
					if (EditorToolIsContainerProperty(LeafProperty) && Notes.IsEmpty())
					{
						StructuredContent->SetStringField(TEXT("notes"), TEXT("Value serialized from a container UDeveloperSettings property."));
					}

					return MakeExecutionResult(
						FString::Printf(TEXT("Read project setting %s.%s from %s."), *Category, *Key, *SourceConfig),
						StructuredContent,
						false);
				}
			}

			TSharedPtr<FJsonValue> ConfigValue;
			FString SourceSection;
			if (TryReadConfigSetting(ConfigFilename, ConfigSections, LookupKey, ConfigValue, SourceSection) && ConfigValue.IsValid())
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("category"), Category);
				StructuredContent->SetStringField(TEXT("key"), Key);
				StructuredContent->SetField(TEXT("value"), ConfigValue);
				StructuredContent->SetStringField(TEXT("sourceConfig"), SourceConfig);
				FString ConfigNotes = FString::Printf(TEXT("Read config key '%s' from section '%s'."), *LookupKey, *SourceSection);
				if (!Notes.IsEmpty())
				{
					ConfigNotes = Notes + TEXT(" ") + ConfigNotes;
				}
				StructuredContent->SetStringField(TEXT("notes"), ConfigNotes);

				return MakeExecutionResult(
					FString::Printf(TEXT("Read project setting %s.%s from %s."), *Category, *Key, *SourceConfig),
					StructuredContent,
					false);
			}

			const FString Message = FString::Printf(TEXT("Project setting key '%s' was not found in category '%s'."), *Key, *Category);
			return MakeProjectSettingsError(TEXT("KEY_NOT_FOUND"), Message, Category, Key, SourceConfig);
		}

		FUnrealMcpExecutionResult ExecuteAssetMove(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			FString SourcePath;
			FString DestinationPath;
			Arguments.TryGetStringField(TEXT("sourcePath"), SourcePath);
			Arguments.TryGetStringField(TEXT("destinationPath"), DestinationPath);
			bool bCreateRedirector = true;
			bool bDryRun = false;
			Arguments.TryGetBoolField(TEXT("createRedirector"), bCreateRedirector);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("sourcePath"), SourcePath);
			StructuredContent->SetStringField(TEXT("destinationPath"), DestinationPath);
			StructuredContent->SetBoolField(TEXT("createRedirector"), bCreateRedirector);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("redirectorCreated"), false);
			StructuredContent->SetNumberField(TEXT("referencingAssets"), 0);
			StructuredContent->SetNumberField(TEXT("referencesUpdated"), 0);

			FEditorToolAssetPath Source;
			FString FailureReason;
			if (!EditorToolTryNormalizeAssetPath(SourcePath, Source, FailureReason))
			{
				const FString Message = FString::Printf(TEXT("Source asset path is invalid or missing: %s"), *FailureReason);
				return MakeEditorToolStructuredError(TEXT("SOURCE_NOT_FOUND"), Message, StructuredContent);
			}

			FEditorToolAssetPath Destination;
			if (!EditorToolTryNormalizeAssetPath(DestinationPath, Destination, FailureReason))
			{
				const FString Message = FString::Printf(TEXT("Destination asset path is invalid: %s"), *FailureReason);
				return MakeEditorToolStructuredError(TEXT("DESTINATION_INVALID"), Message, StructuredContent);
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;

			UObject* SourceAsset = EditorToolLoadAssetForMigration(EditorAssetSubsystem, Source.ObjectPath, FailureReason);
			if (!SourceAsset)
			{
				return MakeEditorToolStructuredError(TEXT("SOURCE_NOT_FOUND"), FailureReason, StructuredContent);
			}

			const TArray<FName> PreRenameReferencers = EditorToolGetReferencingPackages(AssetRegistry, Source.PackageName);
			StructuredContent->SetNumberField(TEXT("referencingAssets"), PreRenameReferencers.Num());

			const FAssetData DestinationData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Destination.ObjectPath));
			if (DestinationData.IsValid() || (EditorAssetSubsystem && EditorAssetSubsystem->DoesAssetExist(Destination.ObjectPath)))
			{
				const FString Message = FString::Printf(TEXT("Destination asset '%s' already exists."), *Destination.ObjectPath);
				return MakeEditorToolStructuredError(TEXT("DESTINATION_OCCUPIED"), Message, StructuredContent);
			}

			if (bDryRun)
			{
				StructuredContent->SetStringField(TEXT("normalizedSourcePath"), Source.ObjectPath);
				StructuredContent->SetStringField(TEXT("normalizedDestinationPath"), Destination.ObjectPath);
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run: would move %s to %s. referencingAssets=%d"),
						*Source.ObjectPath,
						*Destination.ObjectPath,
						PreRenameReferencers.Num()),
					StructuredContent,
					false);
			}

			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			IAssetTools& AssetTools = AssetToolsModule.Get();
			TArray<FAssetRenameData> AssetsToRename;
			AssetsToRename.Add(FAssetRenameData(SourceAsset, Destination.PackagePath, Destination.AssetName));

			bool bRenameSucceeded = false;
			{
				TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
				bRenameSucceeded = AssetTools.RenameAssets(AssetsToRename);
			}

			if (!bRenameSucceeded)
			{
				const FString Message = FString::Printf(
					TEXT("IAssetTools::RenameAssets failed while moving '%s' to '%s'."),
					*Source.ObjectPath,
					*Destination.ObjectPath);
				return MakeEditorToolStructuredError(TEXT("RENAME_FAILED"), Message, StructuredContent);
			}

			if (!bCreateRedirector)
			{
				if (UObjectRedirector* Redirector = EditorToolLoadRedirectorAtPath(AssetRegistry, Source.ObjectPath))
				{
					TArray<UObjectRedirector*> RedirectorsToFix;
					RedirectorsToFix.Add(Redirector);
					TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
					AssetTools.FixupReferencers(RedirectorsToFix, false, ERedirectFixupMode::DeleteFixedUpRedirectors);
				}
			}

			const bool bRedirectorCreated = EditorToolRedirectorExistsAtPath(AssetRegistry, Source.ObjectPath);
			const TArray<FName> PostRenameReferencers = EditorToolGetReferencingPackages(AssetRegistry, Source.PackageName);
			StructuredContent->SetBoolField(TEXT("redirectorCreated"), bRedirectorCreated);
			StructuredContent->SetNumberField(TEXT("referencesUpdated"), FMath::Max(0, PreRenameReferencers.Num() - PostRenameReferencers.Num()));
			StructuredContent->SetStringField(TEXT("normalizedSourcePath"), Source.ObjectPath);
			StructuredContent->SetStringField(TEXT("normalizedDestinationPath"), Destination.ObjectPath);

			return MakeExecutionResult(
				FString::Printf(
					TEXT("Moved asset %s to %s. redirectorCreated=%s referencesUpdated=%d"),
					*Source.ObjectPath,
					*Destination.ObjectPath,
					bRedirectorCreated ? TEXT("true") : TEXT("false"),
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("referencesUpdated")))),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteRedirectorFixup(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			FString RequestedPath = TEXT("/Game");
			bool bDryRun = true;
			bool bRecursive = true;
			bool bFailOnAnyError = false;
			Arguments.TryGetStringField(TEXT("path"), RequestedPath);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("recursive"), bRecursive);
			Arguments.TryGetBoolField(TEXT("failOnAnyError"), bFailOnAnyError);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("path"), RequestedPath);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("recursive"), bRecursive);
			StructuredContent->SetNumberField(TEXT("redirectorsFound"), 0);
			StructuredContent->SetNumberField(TEXT("redirectorsFixed"), 0);
			StructuredContent->SetNumberField(TEXT("affectedAssets"), 0);
			StructuredContent->SetArrayField(TEXT("failures"), TArray<TSharedPtr<FJsonValue>>());

			FString NormalizedPath;
			FString FailureReason;
			if (!EditorToolTryNormalizeGameContentPath(RequestedPath, NormalizedPath, FailureReason))
			{
				return MakeEditorToolStructuredError(TEXT("INVALID_PATH"), FailureReason, StructuredContent);
			}
			StructuredContent->SetStringField(TEXT("path"), NormalizedPath);

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			if (AssetRegistry.IsLoadingAssets() || AssetRegistry.IsGathering())
			{
				return MakeEditorToolStructuredError(
					TEXT("ASSET_REGISTRY_NOT_READY"),
					TEXT("The asset registry is still discovering or scanning assets; retry after discovery completes."),
					StructuredContent);
			}

			FARFilter Filter;
			Filter.PackagePaths.Add(*NormalizedPath);
			Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = bRecursive;

			TArray<FAssetData> RedirectorAssets;
			AssetRegistry.GetAssets(Filter, RedirectorAssets);
			StructuredContent->SetNumberField(TEXT("redirectorsFound"), RedirectorAssets.Num());

			TSet<FName> AffectedPackages;
			TArray<UObjectRedirector*> RedirectorsToFix;
			TArray<FString> RedirectorObjectPaths;
			TArray<TPair<FString, FString>> Failures;
			for (const FAssetData& RedirectorAsset : RedirectorAssets)
			{
				const FString RedirectorObjectPath = RedirectorAsset.GetSoftObjectPath().ToString();
				RedirectorObjectPaths.Add(RedirectorObjectPath);
				for (const FName& Referencer : EditorToolGetReferencingPackages(AssetRegistry, RedirectorAsset.PackageName.ToString()))
				{
					AffectedPackages.Add(Referencer);
				}

				if (!bDryRun)
				{
					if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(RedirectorAsset.GetAsset()))
					{
						RedirectorsToFix.Add(Redirector);
					}
					else
					{
						Failures.Add(TPair<FString, FString>(RedirectorObjectPath, TEXT("Failed to load redirector object.")));
					}
				}
			}
			StructuredContent->SetNumberField(TEXT("affectedAssets"), AffectedPackages.Num());

			if (bDryRun)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run: found %d redirector(s) under %s affecting %d asset package(s)."),
						RedirectorAssets.Num(),
						*NormalizedPath,
						AffectedPackages.Num()),
					StructuredContent,
					false);
			}

			if (RedirectorsToFix.Num() > 0)
			{
				FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
				AssetToolsModule.Get().FixupReferencers(RedirectorsToFix, false, ERedirectFixupMode::DeleteFixedUpRedirectors);
			}

			int32 RedirectorsFixed = 0;
			for (const FString& RedirectorObjectPath : RedirectorObjectPaths)
			{
				if (EditorToolRedirectorExistsAtPath(AssetRegistry, RedirectorObjectPath))
				{
					if (!Failures.ContainsByPredicate([&RedirectorObjectPath](const TPair<FString, FString>& Failure)
					{
						return Failure.Key == RedirectorObjectPath;
					}))
					{
						Failures.Add(TPair<FString, FString>(RedirectorObjectPath, TEXT("Redirector still exists after fixup.")));
					}
				}
				else
				{
					++RedirectorsFixed;
				}
			}

			StructuredContent->SetNumberField(TEXT("redirectorsFixed"), RedirectorsFixed);
			StructuredContent->SetArrayField(TEXT("failures"), EditorToolMakeRedirectorFailureArray(Failures));

			const FString Text = FString::Printf(
				TEXT("Fixed %d of %d redirector(s) under %s. affectedAssets=%d failures=%d"),
				RedirectorsFixed,
				RedirectorAssets.Num(),
				*NormalizedPath,
				AffectedPackages.Num(),
				Failures.Num());

			if (bFailOnAnyError && Failures.Num() > 0)
			{
				return MakeEditorToolStructuredError(TEXT("FIXUP_FAILED"), Text, StructuredContent);
			}

			return MakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteDependencyRemap(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			FString FromAssetPath;
			FString ToAssetPath;
			Arguments.TryGetStringField(TEXT("fromAssetPath"), FromAssetPath);
			Arguments.TryGetStringField(TEXT("toAssetPath"), ToAssetPath);
			bool bDryRun = true;
			bool bDeleteSourceAfter = false;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("deleteSourceAfter"), bDeleteSourceAfter);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("fromAssetPath"), FromAssetPath);
			StructuredContent->SetStringField(TEXT("toAssetPath"), ToAssetPath);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("deleteSourceAfter"), bDeleteSourceAfter);
			StructuredContent->SetNumberField(TEXT("referencingAssets"), 0);
			StructuredContent->SetNumberField(TEXT("referencesRemapped"), 0);
			StructuredContent->SetBoolField(TEXT("sourceDeleted"), false);

			FEditorToolAssetPath Source;
			FString FailureReason;
			if (!EditorToolTryNormalizeAssetPath(FromAssetPath, Source, FailureReason))
			{
				return MakeEditorToolStructuredError(TEXT("SOURCE_NOT_FOUND"), FailureReason, StructuredContent);
			}

			FEditorToolAssetPath Target;
			if (!EditorToolTryNormalizeAssetPath(ToAssetPath, Target, FailureReason))
			{
				return MakeEditorToolStructuredError(TEXT("DESTINATION_NOT_FOUND"), FailureReason, StructuredContent);
			}

			if (Source.ObjectPath == Target.ObjectPath)
			{
				return MakeEditorToolStructuredError(TEXT("REMAP_FAILED"), TEXT("fromAssetPath and toAssetPath must refer to different assets."), StructuredContent);
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;

			UObject* SourceAsset = EditorToolLoadAssetForMigration(EditorAssetSubsystem, Source.ObjectPath, FailureReason);
			if (!SourceAsset)
			{
				return MakeEditorToolStructuredError(TEXT("SOURCE_NOT_FOUND"), FailureReason, StructuredContent);
			}

			UObject* TargetAsset = EditorToolLoadAssetForMigration(EditorAssetSubsystem, Target.ObjectPath, FailureReason);
			if (!TargetAsset)
			{
				return MakeEditorToolStructuredError(TEXT("DESTINATION_NOT_FOUND"), FailureReason, StructuredContent);
			}

			if (SourceAsset->GetClass() != TargetAsset->GetClass())
			{
				const FString Message = FString::Printf(
					TEXT("Cannot remap %s (%s) to %s (%s) because their classes differ."),
					*Source.ObjectPath,
					*SourceAsset->GetClass()->GetPathName(),
					*Target.ObjectPath,
					*TargetAsset->GetClass()->GetPathName());
				return MakeEditorToolStructuredError(TEXT("TYPE_MISMATCH"), Message, StructuredContent);
			}

			const TArray<FName> PreRemapReferencers = EditorToolGetReferencingPackages(AssetRegistry, Source.PackageName);
			StructuredContent->SetNumberField(TEXT("referencingAssets"), PreRemapReferencers.Num());

			if (bDryRun)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run: would remap %d asset package(s) from %s to %s."),
						PreRemapReferencers.Num(),
						*Source.ObjectPath,
						*Target.ObjectPath),
					StructuredContent,
					false);
			}

			TArray<UObject*> ObjectsToConsolidate;
			ObjectsToConsolidate.Add(SourceAsset);
			TSet<UObject*> ObjectsToConsolidateWithin;
			TSet<UObject*> ObjectsToNotConsolidateWithin;
			TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
			ObjectTools::FConsolidationResults ConsolidationResults = ObjectTools::ConsolidateObjects(
				TargetAsset,
				ObjectsToConsolidate,
				ObjectsToConsolidateWithin,
				ObjectsToNotConsolidateWithin,
				bDeleteSourceAfter,
				true);

			if (ConsolidationResults.InvalidConsolidationObjs.Num() > 0 || ConsolidationResults.FailedConsolidationObjs.Num() > 0)
			{
				const FString Message = FString::Printf(
					TEXT("Reference consolidation failed. invalid=%d failed=%d"),
					ConsolidationResults.InvalidConsolidationObjs.Num(),
					ConsolidationResults.FailedConsolidationObjs.Num());
				return MakeEditorToolStructuredError(TEXT("REMAP_FAILED"), Message, StructuredContent);
			}

			const TArray<FName> PostRemapReferencers = EditorToolGetReferencingPackages(AssetRegistry, Source.PackageName);
			StructuredContent->SetNumberField(TEXT("referencesRemapped"), FMath::Max(0, PreRemapReferencers.Num() - PostRemapReferencers.Num()));

			const bool bSourceDeleted = bDeleteSourceAfter
				&& EditorToolIsAssetDeletedOrRedirected(AssetRegistry, EditorAssetSubsystem, Source.ObjectPath);
			StructuredContent->SetBoolField(TEXT("sourceDeleted"), bSourceDeleted);

			if (bDeleteSourceAfter && PostRemapReferencers.Num() > 0)
			{
				const FString Message = FString::Printf(
					TEXT("Source asset '%s' still has %d referencer(s) after consolidation."),
					*Source.ObjectPath,
					PostRemapReferencers.Num());
				return MakeEditorToolStructuredError(TEXT("SOURCE_STILL_REFERENCED"), Message, StructuredContent);
			}

			if (bDeleteSourceAfter && !bSourceDeleted)
			{
				const FString Message = FString::Printf(TEXT("Source asset '%s' was not deleted after consolidation."), *Source.ObjectPath);
				return MakeEditorToolStructuredError(TEXT("REMAP_FAILED"), Message, StructuredContent);
			}

			return MakeExecutionResult(
				FString::Printf(
					TEXT("Remapped references from %s to %s. referencesRemapped=%d sourceDeleted=%s"),
					*Source.ObjectPath,
					*Target.ObjectPath,
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("referencesRemapped"))),
					bSourceDeleted ? TEXT("true") : TEXT("false")),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteProjectVersionMigration(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			FString TargetEngineVersion;
			Arguments.TryGetStringField(TEXT("targetEngineVersion"), TargetEngineVersion);
			TargetEngineVersion = TargetEngineVersion.TrimStartAndEnd();
			bool bDryRun = true;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			Arguments.TryGetStringField(TEXT("projectFilePath"), ProjectFilePath);
			ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);
			FPaths::NormalizeFilename(ProjectFilePath);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("projectFilePath"), ProjectFilePath);
			StructuredContent->SetStringField(TEXT("currentEngineVersion"), FString());
			StructuredContent->SetStringField(TEXT("targetEngineVersion"), TargetEngineVersion);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("migrationApplied"), false);
			StructuredContent->SetBoolField(TEXT("restartRecommended"), false);

			TArray<FString> CompatibilityWarnings;
			TArray<FString> ManualSteps;
			ManualSteps.Add(TEXT("Close Unreal Editor before rebuilding against the target engine."));
			ManualSteps.Add(TEXT("Switch Engine Version on the .uproject if UnrealVersionSelector is required on this machine."));
			ManualSteps.Add(TEXT("Regenerate project files for the target engine."));
			ManualSteps.Add(TEXT("Rebuild plugin binaries against the target engine."));
			ManualSteps.Add(FString::Printf(TEXT("Reopen the project with Unreal Engine %s."), *TargetEngineVersion));

			auto FinalizeArrays = [&StructuredContent, &CompatibilityWarnings, &ManualSteps]()
			{
				StructuredContent->SetArrayField(TEXT("compatibilityWarnings"), EditorToolMakeJsonStringArray(CompatibilityWarnings));
				StructuredContent->SetArrayField(TEXT("manualSteps"), EditorToolMakeJsonStringArray(ManualSteps));
			};

			if (TargetEngineVersion != TEXT("5.6") && TargetEngineVersion != TEXT("5.7"))
			{
				FinalizeArrays();
				return MakeEditorToolStructuredError(
					TEXT("INVALID_TARGET_VERSION"),
					TEXT("targetEngineVersion must be either \"5.6\" or \"5.7\"."),
					StructuredContent);
			}

			if (!FPaths::FileExists(ProjectFilePath))
			{
				FinalizeArrays();
				return MakeEditorToolStructuredError(
					TEXT("PROJECT_FILE_NOT_FOUND"),
					FString::Printf(TEXT("Project file '%s' was not found."), *ProjectFilePath),
					StructuredContent);
			}

			TSharedPtr<FJsonObject> ProjectJson;
			FString FailureReason;
			if (!EditorToolTryLoadJsonObjectFromFile(ProjectFilePath, ProjectJson, FailureReason))
			{
				FinalizeArrays();
				return MakeEditorToolStructuredError(TEXT("PROJECT_FILE_INVALID_JSON"), FailureReason, StructuredContent);
			}

			FString CurrentEngineVersion;
			ProjectJson->TryGetStringField(TEXT("EngineAssociation"), CurrentEngineVersion);
			StructuredContent->SetStringField(TEXT("currentEngineVersion"), CurrentEngineVersion);

			const FString ProjectModuleName = FPaths::GetBaseFilename(ProjectFilePath);
			bool bFoundProjectModule = false;
			const TArray<TSharedPtr<FJsonValue>>* ModulesArray = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Modules"), ModulesArray) && ModulesArray)
			{
				for (const TSharedPtr<FJsonValue>& ModuleValue : *ModulesArray)
				{
					const TSharedPtr<FJsonObject> ModuleObject = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
					FString ModuleName;
					if (ModuleObject.IsValid()
						&& ModuleObject->TryGetStringField(TEXT("Name"), ModuleName)
						&& ModuleName == ProjectModuleName)
					{
						bFoundProjectModule = true;
						break;
					}
				}
			}
			if (!bFoundProjectModule)
			{
				CompatibilityWarnings.Add(FString::Printf(
					TEXT("No module named '%s' was found in the .uproject Modules array; verify target/project file generation manually."),
					*ProjectModuleName));
			}

			const FString PluginDescriptorPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UnrealMcp"), TEXT("UnrealMcp.uplugin"));
			TSharedPtr<FJsonObject> PluginJson;
			if (FPaths::FileExists(PluginDescriptorPath) && EditorToolTryLoadJsonObjectFromFile(PluginDescriptorPath, PluginJson, FailureReason))
			{
				FString PluginEngineVersion;
				if (PluginJson->TryGetStringField(TEXT("EngineVersion"), PluginEngineVersion)
					&& EditorToolCompareVersionTriplets(TargetEngineVersion, PluginEngineVersion) < 0)
				{
					CompatibilityWarnings.Add(FString::Printf(
						TEXT("UnrealMcp.uplugin declares EngineVersion %s, which is above requested target %s."),
						*PluginEngineVersion,
						*TargetEngineVersion));
				}
			}
			else
			{
				CompatibilityWarnings.Add(TEXT("Could not read Plugins/UnrealMcp/UnrealMcp.uplugin to verify EngineVersion compatibility."));
			}

			if (!bDryRun)
			{
				ProjectJson->SetStringField(TEXT("EngineAssociation"), TargetEngineVersion);
				if (!EditorToolTrySaveJsonObjectToFile(ProjectFilePath, ProjectJson.ToSharedRef(), FailureReason))
				{
					FinalizeArrays();
					return MakeEditorToolStructuredError(TEXT("WRITE_FAILED"), FailureReason, StructuredContent);
				}
				StructuredContent->SetBoolField(TEXT("migrationApplied"), true);
				StructuredContent->SetBoolField(TEXT("restartRecommended"), true);
			}

			FinalizeArrays();
			return MakeExecutionResult(
				FString::Printf(
					TEXT("%s project EngineAssociation from '%s' to '%s'. warnings=%d"),
					bDryRun ? TEXT("Dry run: would migrate") : TEXT("Migrated"),
					CurrentEngineVersion.IsEmpty() ? TEXT("<unset>") : *CurrentEngineVersion,
					*TargetEngineVersion,
					CompatibilityWarnings.Num()),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteTailLog(const FJsonObject& Arguments)
		{
			const int32 RequestedLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("lines"), 120), 500);
			FString ContainsFilter;
			Arguments.TryGetStringField(TEXT("contains"), ContainsFilter);
			ContainsFilter = ContainsFilter.TrimStartAndEnd();

			const FString RawEditorLogPath = FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
			FString EditorLogPath = FPaths::ConvertRelativePathToFull(RawEditorLogPath);
			FPaths::NormalizeFilename(EditorLogPath);
			FString FullLogText;
			if (!FFileHelper::LoadFileToString(FullLogText, *EditorLogPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read editor log '%s' (raw path: '%s')."), *EditorLogPath, *RawEditorLogPath), nullptr, true);
			}

			TArray<FString> AllLines;
			FullLogText.ParseIntoArrayLines(AllLines);

			TArray<FString> MatchingLines;
			MatchingLines.Reserve(AllLines.Num());
			if (ContainsFilter.IsEmpty())
			{
				MatchingLines = AllLines;
			}
			else
			{
				for (const FString& Line : AllLines)
				{
					if (Line.Contains(ContainsFilter, ESearchCase::IgnoreCase))
					{
						MatchingLines.Add(Line);
					}
				}
			}

			const int32 StartIndex = FMath::Max(0, MatchingLines.Num() - RequestedLines);
			TArray<FString> ReturnedLines;
			for (int32 Index = StartIndex; Index < MatchingLines.Num(); ++Index)
			{
				ReturnedLines.Add(MatchingLines[Index]);
			}

			const FString TailText = ReturnedLines.Num() > 0
				? FString::Join(ReturnedLines, TEXT("\n"))
				: (ContainsFilter.IsEmpty()
					? TEXT("The editor log is empty.")
					: FString::Printf(TEXT("No log lines matched '%s'."), *ContainsFilter));

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("logPath"), EditorLogPath);
			StructuredContent->SetStringField(TEXT("rawLogPath"), RawEditorLogPath);
			StructuredContent->SetNumberField(TEXT("requestedLines"), RequestedLines);
			StructuredContent->SetNumberField(TEXT("matchedLineCount"), MatchingLines.Num());
			StructuredContent->SetNumberField(TEXT("returnedLineCount"), ReturnedLines.Num());
			StructuredContent->SetStringField(TEXT("text"), TailText);
			if (!ContainsFilter.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("contains"), ContainsFilter);
			}

			return MakeExecutionResult(TailText, StructuredContent, false);
		}

		bool IsEditorPlaying()
		{
			return GEditor
				&& (GEditor->PlayWorld != nullptr
					|| GEditor->bIsSimulatingInEditor
					|| GEditor->GetPlaySessionRequest().IsSet());
		}

		FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName)
		{
			return MakeExecutionResult(
				FString::Printf(TEXT("Tool '%s' is blocked while Play In Editor is active or starting."), *ToolName),
				nullptr,
				true);
		}

		bool EditorToolTryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues)
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

		UWorld* EditorToolResolveConsoleWorld(const FString& RequestedTarget, FString& OutResolvedTarget, FString& OutFailureReason)
		{
			OutResolvedTarget = TEXT("editor");
			OutFailureReason.Reset();

			if (!GEditor)
			{
				OutFailureReason = TEXT("GEditor is unavailable.");
				return nullptr;
			}

			const FString NormalizedTarget = RequestedTarget.TrimStartAndEnd().ToLower();
			const bool bIsAuto = NormalizedTarget.IsEmpty() || NormalizedTarget == TEXT("auto");
			const bool bWantsPie = NormalizedTarget == TEXT("pie");
			const bool bWantsEditor = NormalizedTarget == TEXT("editor");

			if (!bIsAuto && !bWantsPie && !bWantsEditor)
			{
				OutFailureReason = FString::Printf(TEXT("Unknown console target '%s'. Use auto, editor, or pie."), *RequestedTarget);
				return nullptr;
			}

			if ((bIsAuto || bWantsPie) && GEditor->PlayWorld != nullptr)
			{
				OutResolvedTarget = TEXT("pie");
				return GEditor->PlayWorld;
			}

			if (bWantsPie)
			{
				OutFailureReason = TEXT("No Play In Editor world is currently active.");
				return nullptr;
			}

			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (!EditorWorld)
			{
				OutFailureReason = TEXT("The editor world is unavailable.");
				return nullptr;
			}

			OutResolvedTarget = TEXT("editor");
			return EditorWorld;
		}

		IPythonScriptPlugin* EditorToolLoadPythonScriptPlugin()
		{
			static const FName PythonScriptPluginModuleName(TEXT("PythonScriptPlugin"));
			if (IPythonScriptPlugin* PythonPlugin = FModuleManager::GetModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName))
			{
				return PythonPlugin;
			}

			return FModuleManager::LoadModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName);
		}

		bool EditorToolTryParsePythonFileExecutionScope(const FString& ScopeString, EPythonFileExecutionScope& OutScope)
		{
			if (ScopeString.Equals(TEXT("private"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Private;
				return true;
			}

			if (ScopeString.Equals(TEXT("public"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Public;
				return true;
			}

			return false;
		}

		FString EditorToolQuoteShellArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));

			const bool bNeedsQuotes = Escaped.Contains(TEXT(" "))
				|| Escaped.Contains(TEXT("\t"))
				|| Escaped.Contains(TEXT("\""));

			return bNeedsQuotes
				? FString::Printf(TEXT("\"%s\""), *Escaped)
				: Escaped;
		}

		bool EditorToolResolvePythonScriptPath(
			const FString& RequestedPath,
			bool bAllowOutsideProject,
			FString& OutResolvedPath,
			FString& OutFailureReason)
		{
			OutResolvedPath.Reset();
			OutFailureReason.Reset();

			const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				OutFailureReason = TEXT("The scriptPath argument is required.");
				return false;
			}

			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::CollapseRelativeDirectories(ProjectDir);

			FString ResolvedPath = FPaths::IsRelative(TrimmedPath)
				? FPaths::Combine(ProjectDir, TrimmedPath)
				: TrimmedPath;
			ResolvedPath = FPaths::ConvertRelativePathToFull(ResolvedPath);
			FPaths::NormalizeFilename(ResolvedPath);
			FPaths::CollapseRelativeDirectories(ResolvedPath);

			if (!ResolvedPath.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("Python script path must end with .py: %s"), *ResolvedPath);
				return false;
			}

			if (!FPaths::FileExists(ResolvedPath))
			{
				OutFailureReason = FString::Printf(TEXT("Python script file does not exist: %s"), *ResolvedPath);
				return false;
			}

			if (!bAllowOutsideProject && !ResolvedPath.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("Python script must be inside the project directory unless allowOutsideProject=true. path=%s project=%s"), *ResolvedPath, *ProjectDir);
				return false;
			}

			OutResolvedPath = ResolvedPath;
			return true;
		}

		FUnrealMcpExecutionResult ExecuteStartPie(const FJsonObject& Arguments)
		{
			if (!GEditor)
			{
				return MakeExecutionResult(TEXT("GEditor is unavailable."), nullptr, true);
			}

			if (IsEditorPlaying())
			{
				return MakeExecutionResult(TEXT("A Play In Editor session is already active or queued."), nullptr, true);
			}

			bool bSimulate = false;
			Arguments.TryGetBoolField(TEXT("simulate"), bSimulate);

			FRequestPlaySessionParams SessionParams;
			if (bSimulate)
			{
				SessionParams.WorldType = EPlaySessionWorldType::SimulateInEditor;
			}

			GEditor->RequestPlaySession(SessionParams);

			const bool bQueued = GEditor->GetPlaySessionRequest().IsSet();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetBoolField(TEXT("requested"), true);
			StructuredContent->SetBoolField(TEXT("simulate"), bSimulate);
			StructuredContent->SetBoolField(TEXT("playRequestPending"), bQueued);

			return MakeExecutionResult(
				FString::Printf(
					TEXT("Requested %s session. queued=%s"),
					bSimulate ? TEXT("Simulate In Editor") : TEXT("Play In Editor"),
					bQueued ? TEXT("true") : TEXT("false")),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ExecuteStopPie()
		{
			if (!GEditor)
			{
				return MakeExecutionResult(TEXT("GEditor is unavailable."), nullptr, true);
			}

			const bool bWasPIE = GEditor->PlayWorld != nullptr;
			const bool bWasSimulating = GEditor->bIsSimulatingInEditor;
			const bool bHadQueuedRequest = GEditor->GetPlaySessionRequest().IsSet();
			if (!bWasPIE && !bWasSimulating && !bHadQueuedRequest)
			{
				return MakeExecutionResult(TEXT("No Play In Editor session is running or queued."), nullptr, true);
			}

			GEditor->RequestEndPlayMap();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetBoolField(TEXT("requested"), true);
			StructuredContent->SetBoolField(TEXT("wasPlayInEditor"), bWasPIE);
			StructuredContent->SetBoolField(TEXT("wasSimulatingInEditor"), bWasSimulating);
			StructuredContent->SetBoolField(TEXT("hadQueuedPlayRequest"), bHadQueuedRequest);

			return MakeExecutionResult(TEXT("Requested Play In Editor shutdown."), StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteConsoleCommand(const FJsonObject& Arguments)
		{
			FString Command;
			if (!Arguments.TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'command'."), nullptr, true);
			}

			FString RequestedTarget = TEXT("auto");
			Arguments.TryGetStringField(TEXT("target"), RequestedTarget);

			FString ResolvedTarget;
			FString FailureReason;
			UWorld* TargetWorld = EditorToolResolveConsoleWorld(RequestedTarget, ResolvedTarget, FailureReason);
			if (!TargetWorld)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FStringOutputDevice OutputDevice;
			const bool bExecuted = GEditor && GEditor->Exec(TargetWorld, *Command, OutputDevice);
			const FString CapturedOutput = FString(OutputDevice).TrimStartAndEnd();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("command"), Command);
			StructuredContent->SetStringField(TEXT("target"), ResolvedTarget);
			StructuredContent->SetStringField(TEXT("worldPath"), TargetWorld->GetPathName());
			StructuredContent->SetBoolField(TEXT("success"), bExecuted);
			StructuredContent->SetStringField(TEXT("output"), CapturedOutput);

			FString Text = FString::Printf(
				TEXT("Console command '%s' executed on %s world. success=%s"),
				*Command,
				*ResolvedTarget,
				bExecuted ? TEXT("true") : TEXT("false"));
			if (!CapturedOutput.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n%s"), *CapturedOutput);
			}

			return MakeExecutionResult(Text, StructuredContent, !bExecuted);
		}

		FUnrealMcpExecutionResult ExecutePythonCommand(const FJsonObject& Arguments)
		{
			FString Command;
			if (!Arguments.TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'command'."), nullptr, true);
			}

			FString ModeString = TEXT("Auto");
			FString ScopeString = TEXT("Private");
			bool bAutoMode = true;
			bool bForceEnable = true;
			bool bUnattended = true;
			Arguments.TryGetStringField(TEXT("mode"), ModeString);
			Arguments.TryGetStringField(TEXT("scope"), ScopeString);
			Arguments.TryGetBoolField(TEXT("autoMode"), bAutoMode);
			Arguments.TryGetBoolField(TEXT("forceEnable"), bForceEnable);
			Arguments.TryGetBoolField(TEXT("unattended"), bUnattended);

			const FString RequestedModeString = ModeString.TrimStartAndEnd().IsEmpty() ? TEXT("Auto") : ModeString.TrimStartAndEnd();
			const FString TrimmedCommand = Command.TrimStartAndEnd();
			const bool bLooksLikeMultiStatementPython = Command.Contains(TEXT("\n"))
				|| Command.Contains(TEXT("\r"))
				|| Command.Contains(TEXT(";"));
			const bool bContainsAssignmentLike = TrimmedCommand.Contains(TEXT("="))
				&& !TrimmedCommand.Contains(TEXT("=="))
				&& !TrimmedCommand.Contains(TEXT("!="))
				&& !TrimmedCommand.Contains(TEXT("<="))
				&& !TrimmedCommand.Contains(TEXT(">="));
			const bool bLooksLikePythonStatement = TrimmedCommand.StartsWith(TEXT("import "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("from "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("for "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("while "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("if "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("with "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("def "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("class "), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("try:"), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("pass"), ESearchCase::IgnoreCase)
				|| TrimmedCommand.StartsWith(TEXT("return "), ESearchCase::IgnoreCase)
				|| bContainsAssignmentLike;
			bool bAutoModeChanged = false;
			if (RequestedModeString.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
			{
				ModeString = (bLooksLikeMultiStatementPython || bLooksLikePythonStatement) ? TEXT("ExecuteFile") : TEXT("EvaluateStatement");
				bAutoModeChanged = true;
			}
			else if (bAutoMode
				&& !RequestedModeString.Equals(TEXT("ExecuteFile"), ESearchCase::IgnoreCase)
				&& bLooksLikeMultiStatementPython)
			{
				ModeString = TEXT("ExecuteFile");
				bAutoModeChanged = true;
			}
			else
			{
				ModeString = RequestedModeString;
			}

			EPythonCommandExecutionMode ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
			if (!LexTryParseString(ExecutionMode, *ModeString))
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Unknown Python execution mode '%s'. Use Auto, ExecuteFile, ExecuteStatement, or EvaluateStatement."), *ModeString),
					nullptr,
					true);
			}

			EPythonFileExecutionScope FileExecutionScope = EPythonFileExecutionScope::Private;
			if (!EditorToolTryParsePythonFileExecutionScope(ScopeString, FileExecutionScope))
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Unknown Python scope '%s'. Use Private or Public."), *ScopeString),
					nullptr,
					true);
			}

			IPythonScriptPlugin* PythonPlugin = EditorToolLoadPythonScriptPlugin();
			if (!PythonPlugin)
			{
				return MakeExecutionResult(
					TEXT("PythonScriptPlugin is not loaded. Enable the Python Script Plugin for the editor and restart Unreal Editor."),
					nullptr,
					true);
			}

			if (bForceEnable && !PythonPlugin->IsPythonInitialized())
			{
				PythonPlugin->ForceEnablePythonAtRuntime();
			}

			if (!PythonPlugin->IsPythonAvailable())
			{
				return MakeExecutionResult(TEXT("Python support is not available in the current editor session."), nullptr, true);
			}

			if (!PythonPlugin->IsPythonInitialized())
			{
				return MakeExecutionResult(
					TEXT("Python is not initialized. Re-open the editor after enabling the Python Script Plugin, or retry with forceEnable=true."),
					nullptr,
					true);
			}

			FPythonCommandEx PythonCommand;
			PythonCommand.Command = Command;
			PythonCommand.ExecutionMode = ExecutionMode;
			PythonCommand.FileExecutionScope = FileExecutionScope;
			PythonCommand.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

			const bool bSucceeded = PythonPlugin->ExecPythonCommandEx(PythonCommand);

			TArray<TSharedPtr<FJsonValue>> LogOutputArray;
			TArray<FString> LogLines;
			LogLines.Reserve(PythonCommand.LogOutput.Num());

			for (const FPythonLogOutputEntry& LogEntry : PythonCommand.LogOutput)
			{
				TSharedPtr<FJsonObject> LogObject = MakeShared<FJsonObject>();
				LogObject->SetStringField(TEXT("type"), LexToString(LogEntry.Type));
				LogObject->SetStringField(TEXT("output"), LogEntry.Output);
				LogOutputArray.Add(MakeShared<FJsonValueObject>(LogObject));
				LogLines.Add(FString::Printf(TEXT("[%s] %s"), LexToString(LogEntry.Type), *LogEntry.Output));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("command"), Command);
			StructuredContent->SetStringField(TEXT("requestedMode"), RequestedModeString);
			StructuredContent->SetStringField(TEXT("mode"), LexToString(ExecutionMode));
			StructuredContent->SetStringField(TEXT("scope"), FileExecutionScope == EPythonFileExecutionScope::Public ? TEXT("Public") : TEXT("Private"));
			StructuredContent->SetBoolField(TEXT("autoMode"), bAutoMode);
			StructuredContent->SetBoolField(TEXT("autoModeChanged"), bAutoModeChanged);
			StructuredContent->SetBoolField(TEXT("forceEnable"), bForceEnable);
			StructuredContent->SetBoolField(TEXT("unattended"), bUnattended);
			StructuredContent->SetBoolField(TEXT("success"), bSucceeded);
			StructuredContent->SetStringField(TEXT("commandResult"), PythonCommand.CommandResult);
			StructuredContent->SetNumberField(TEXT("logCount"), PythonCommand.LogOutput.Num());
			StructuredContent->SetArrayField(TEXT("logOutput"), LogOutputArray);

			FString Text = FString::Printf(
				TEXT("Executed Python command. success=%s mode=%s scope=%s"),
				bSucceeded ? TEXT("true") : TEXT("false"),
				LexToString(ExecutionMode),
				FileExecutionScope == EPythonFileExecutionScope::Public ? TEXT("Public") : TEXT("Private"));
			if (bAutoModeChanged)
			{
				Text += FString::Printf(TEXT(" (requested %s, auto-adjusted)"), *RequestedModeString);
			}

			if (!PythonCommand.CommandResult.IsEmpty())
			{
				Text += FString::Printf(TEXT("\nResult:\n%s"), *PythonCommand.CommandResult);
			}

			if (LogLines.Num() > 0)
			{
				Text += TEXT("\nLog:\n") + FString::Join(LogLines, TEXT("\n"));
			}

			return MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		FUnrealMcpExecutionResult ExecutePythonFile(const FJsonObject& Arguments)
		{
			FString ScriptPath;
			if (!Arguments.TryGetStringField(TEXT("scriptPath"), ScriptPath) || ScriptPath.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("Missing required field 'scriptPath'."), nullptr, true);
			}

			TArray<FString> ScriptArgs;
			EditorToolTryGetStringArrayField(Arguments, TEXT("args"), ScriptArgs);

			bool bAllowOutsideProject = false;
			Arguments.TryGetBoolField(TEXT("allowOutsideProject"), bAllowOutsideProject);

			FString ResolvedScriptPath;
			FString ResolveFailureReason;
			if (!EditorToolResolvePythonScriptPath(ScriptPath, bAllowOutsideProject, ResolvedScriptPath, ResolveFailureReason))
			{
				return MakeExecutionResult(ResolveFailureReason, nullptr, true);
			}

			FString Command = EditorToolQuoteShellArgument(ResolvedScriptPath);
			for (const FString& ScriptArg : ScriptArgs)
			{
				Command += TEXT(" ");
				Command += EditorToolQuoteShellArgument(ScriptArg);
			}

			FString ScopeString = TEXT("Private");
			bool bForceEnable = true;
			bool bUnattended = true;
			Arguments.TryGetStringField(TEXT("scope"), ScopeString);
			Arguments.TryGetBoolField(TEXT("forceEnable"), bForceEnable);
			Arguments.TryGetBoolField(TEXT("unattended"), bUnattended);

			TSharedPtr<FJsonObject> ForwardArguments = MakeShared<FJsonObject>();
			ForwardArguments->SetStringField(TEXT("command"), Command);
			ForwardArguments->SetStringField(TEXT("mode"), TEXT("ExecuteFile"));
			ForwardArguments->SetStringField(TEXT("scope"), ScopeString);
			ForwardArguments->SetBoolField(TEXT("forceEnable"), bForceEnable);
			ForwardArguments->SetBoolField(TEXT("unattended"), bUnattended);

			FUnrealMcpExecutionResult ExecutionResult = ExecutePythonCommand(*ForwardArguments);
			if (ExecutionResult.StructuredContent.IsValid())
			{
				ExecutionResult.StructuredContent->SetStringField(TEXT("scriptPath"), ResolvedScriptPath);

				TArray<TSharedPtr<FJsonValue>> ArgsArray;
				for (const FString& ScriptArg : ScriptArgs)
				{
					ArgsArray.Add(MakeShared<FJsonValueString>(ScriptArg));
				}
				ExecutionResult.StructuredContent->SetArrayField(TEXT("args"), ArgsArray);
				ExecutionResult.StructuredContent->SetBoolField(TEXT("allowOutsideProject"), bAllowOutsideProject);
			}

			ExecutionResult.Text = FString::Printf(TEXT("Executed Python script file %s.\n%s"), *ResolvedScriptPath, *ExecutionResult.Text);
			return ExecutionResult;
		}

		FUnrealMcpExecutionResult ExecuteMapCheck(const FString& ToolName)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!EditorWorld)
			{
				return MakeExecutionResult(TEXT("The editor world is unavailable."), nullptr, true);
			}

			FStringOutputDevice OutputDevice;
			const bool bExecuted = GEditor && GEditor->Exec(EditorWorld, TEXT("MAP CHECK DONTDISPLAYDIALOG"), OutputDevice);

			FMessageLog MapCheckLog(TEXT("MapCheck"));
			const int32 ErrorCount = MapCheckLog.NumMessages(EMessageSeverity::Error);
			const int32 WarningOrHigherCount = MapCheckLog.NumMessages(EMessageSeverity::Warning);
			const int32 WarningCount = FMath::Max(0, WarningOrHigherCount - ErrorCount);
			const FString CapturedOutput = FString(OutputDevice).TrimStartAndEnd();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("map"), EditorWorld->GetOutermost()->GetName());
			StructuredContent->SetBoolField(TEXT("success"), bExecuted);
			StructuredContent->SetNumberField(TEXT("errorCount"), ErrorCount);
			StructuredContent->SetNumberField(TEXT("warningCount"), WarningCount);
			StructuredContent->SetStringField(TEXT("output"), CapturedOutput);

			FString Text = FString::Printf(
				TEXT("Map Check completed for %s. success=%s errors=%d warnings=%d"),
				*EditorWorld->GetOutermost()->GetName(),
				bExecuted ? TEXT("true") : TEXT("false"),
				ErrorCount,
				WarningCount);
			if (!CapturedOutput.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n%s"), *CapturedOutput);
			}

			return MakeExecutionResult(Text, StructuredContent, !bExecuted || ErrorCount > 0);
		}

		UObject* EditorToolLoadAssetFromAnyPath(
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

		FUnrealMcpExecutionResult ExecuteOpenMap(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			FString MapPath;
			if (!Arguments.TryGetStringField(TEXT("path"), MapPath) || MapPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
			}

			FString FailureReason;
			const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(MapPath, FailureReason);
			if (ObjectPath.IsEmpty())
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve map path: %s"), *FailureReason), nullptr, true);
			}

			TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
			const bool bLoaded = UEditorLoadingAndSavingUtils::LoadMap(ObjectPath) != nullptr;
			if (!bLoaded)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to open map '%s'."), *ObjectPath), nullptr, true);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("path"), ObjectPath);
			return MakeExecutionResult(FString::Printf(TEXT("Opened map %s."), *ObjectPath), StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteOpenAsset(const FJsonObject& Arguments)
		{
			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			if (!EditorAssetSubsystem || !AssetEditorSubsystem)
			{
				return MakeExecutionResult(TEXT("Asset editor subsystems are unavailable."), nullptr, true);
			}

			FString AssetPath;
			if (!Arguments.TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
			}

			FString ObjectPath;
			FString FailureReason;
			UObject* Asset = EditorToolLoadAssetFromAnyPath(EditorAssetSubsystem, AssetPath, ObjectPath, FailureReason);
			if (!Asset)
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			const bool bOpened = AssetEditorSubsystem->OpenEditorForAsset(Asset);
			if (!bOpened)
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to open an editor for '%s'."), *ObjectPath), nullptr, true);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
			StructuredContent->SetStringField(TEXT("classPath"), Asset->GetClass()->GetPathName());
			return MakeExecutionResult(FString::Printf(TEXT("Opened asset %s."), *ObjectPath), StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteSyncContentBrowser(const FJsonObject& Arguments)
		{
			UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			if (!EditorAssetSubsystem)
			{
				return MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString RequestedPath;
			if (!Arguments.TryGetStringField(TEXT("path"), RequestedPath) || RequestedPath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
			}

			FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
			const FAssetData AssetData = EditorAssetSubsystem->FindAssetData(RequestedPath);
			if (AssetData.IsValid())
			{
				ContentBrowserModule.Get().SyncBrowserToAssets(TArray<FAssetData>{AssetData}, false, true);

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("objectPath"), AssetData.GetSoftObjectPath().ToString());
				return MakeExecutionResult(
					FString::Printf(TEXT("Synced Content Browser to asset %s."), *AssetData.GetSoftObjectPath().ToString()),
					StructuredContent,
					false);
			}

			FString FailureReason;
			const FString FolderPath = EditorScriptingHelpers::ConvertAnyPathToLongPackagePath(RequestedPath, FailureReason);
			if (FolderPath.IsEmpty())
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unable to resolve path '%s': %s"), *RequestedPath, *FailureReason), nullptr, true);
			}

			ContentBrowserModule.Get().SyncBrowserToFolders(TArray<FString>{FolderPath}, false, true);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("folderPath"), FolderPath);
			return MakeExecutionResult(FString::Printf(TEXT("Synced Content Browser to folder %s."), *FolderPath), StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteSaveDirtyPackages(const FString& ToolName, const FJsonObject& Arguments)
		{
			if (IsEditorPlaying())
			{
				return MakePieBlockedResult(ToolName);
			}

			bool bSaveMaps = true;
			bool bSaveAssets = true;
			Arguments.TryGetBoolField(TEXT("saveMaps"), bSaveMaps);
			Arguments.TryGetBoolField(TEXT("saveAssets"), bSaveAssets);

			const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(bSaveMaps, bSaveAssets);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetBoolField(TEXT("saved"), bSaved);
			StructuredContent->SetBoolField(TEXT("saveMaps"), bSaveMaps);
			StructuredContent->SetBoolField(TEXT("saveAssets"), bSaveAssets);

			const FString Text = FString::Printf(
				TEXT("SaveDirtyPackages completed. saved=%s saveMaps=%s saveAssets=%s"),
				bSaved ? TEXT("true") : TEXT("false"),
				bSaveMaps ? TEXT("true") : TEXT("false"),
				bSaveAssets ? TEXT("true") : TEXT("false"));

			return MakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteListMaps()
		{
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			FARFilter Filter;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;

			TArray<FAssetData> AssetData;
			AssetRegistryModule.Get().GetAssets(Filter, AssetData);
			AssetData.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.PackageName.ToString() < B.PackageName.ToString();
			});

			TArray<TSharedPtr<FJsonValue>> MapsArray;
			TArray<FString> TextLines;
			for (const FAssetData& Asset : AssetData)
			{
				TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
				AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
				AssetObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
				AssetObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
				MapsArray.Add(MakeShared<FJsonValueObject>(AssetObject));
				TextLines.Add(Asset.PackageName.ToString());
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("count"), AssetData.Num());
			StructuredContent->SetArrayField(TEXT("maps"), MapsArray);

			const FString Text = TextLines.Num() > 0
				? FString::Printf(TEXT("Found %d maps:\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
				: TEXT("Found 0 maps under /Game.");

			return MakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteListAssets(const FJsonObject& Arguments)
		{
			FString Path = TEXT("/Game");
			bool bRecursive = true;
			FString ClassPathFilter;
			Arguments.TryGetStringField(TEXT("path"), Path);
			Arguments.TryGetBoolField(TEXT("recursive"), bRecursive);
			Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
			const int32 Limit = GetPositiveIntArgument(Arguments, TEXT("limit"), EditorToolDefaultListLimit);

			if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
			{
				return MakeExecutionResult(TEXT("The path argument must be a Content Browser path like /Game."), nullptr, true);
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			FARFilter Filter;
			Filter.PackagePaths.Add(*Path);
			Filter.bRecursivePaths = bRecursive;

			TArray<FAssetData> AssetData;
			AssetRegistryModule.Get().GetAssets(Filter, AssetData);
			AssetData.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
			});

			int32 TotalMatches = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> AssetsArray;
			TArray<FString> TextLines;

			for (const FAssetData& Asset : AssetData)
			{
				if (!ClassPathFilter.IsEmpty())
				{
					const FString AssetClassPath = Asset.AssetClassPath.ToString();
					if (!AssetClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
						&& !AssetClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
					{
						continue;
					}
				}

				++TotalMatches;

				if (AssetsArray.Num() >= Limit)
				{
					bTruncated = true;
					continue;
				}

				AssetsArray.Add(MakeShared<FJsonValueObject>(MakeAssetObject(Asset)));
				TextLines.Add(DescribeAsset(Asset));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("path"), Path);
			StructuredContent->SetBoolField(TEXT("recursive"), bRecursive);
			StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
			StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
			StructuredContent->SetNumberField(TEXT("returnedCount"), AssetsArray.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("assets"), AssetsArray);

			FString Text;
			if (TextLines.Num() > 0)
			{
				Text = FString::Printf(TEXT("Found %d assets under %s"), TotalMatches, *Path);
				if (!ClassPathFilter.IsEmpty())
				{
					Text += FString::Printf(TEXT(" filtered by %s"), *ClassPathFilter);
				}
				if (bTruncated)
				{
					Text += FString::Printf(TEXT(" (showing first %d)"), AssetsArray.Num());
				}
				Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
			}
			else
			{
				Text = FString::Printf(TEXT("Found 0 assets under %s."), *Path);
			}

			return MakeExecutionResult(Text, StructuredContent, false);
		}

		FUnrealMcpExecutionResult ExecuteListSelectedAssets()
		{
			const TArray<FAssetData> SelectedAssets = GetSelectedAssets();

			TArray<TSharedPtr<FJsonValue>> AssetsArray;
			TArray<FString> TextLines;
			for (const FAssetData& Asset : SelectedAssets)
			{
				AssetsArray.Add(MakeShared<FJsonValueObject>(MakeAssetObject(Asset)));
				TextLines.Add(DescribeAsset(Asset));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("count"), SelectedAssets.Num());
			StructuredContent->SetArrayField(TEXT("assets"), AssetsArray);

			const FString Text = TextLines.Num() > 0
				? FString::Printf(TEXT("Selected assets (%d):\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
				: TEXT("No assets are currently selected in the Content Browser.");

			return MakeExecutionResult(Text, StructuredContent, false);
		}
	}

	bool TryExecuteEditorTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.editor_status"))
		{
			OutResult = ExecuteEditorStatus();
			return true;
		}

		if (ToolName == TEXT("unreal.editor.engine_version"))
		{
			OutResult = ExecuteEditorEngineVersion();
			return true;
		}

		if (ToolName == TEXT("unreal.project_settings_get"))
		{
			OutResult = ExecuteProjectSettingsGet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.asset_move"))
		{
			OutResult = ExecuteAssetMove(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.redirector_fixup"))
		{
			OutResult = ExecuteRedirectorFixup(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.dependency_remap"))
		{
			OutResult = ExecuteDependencyRemap(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.project_version_migration"))
		{
			OutResult = ExecuteProjectVersionMigration(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.tail_log"))
		{
			OutResult = ExecuteTailLog(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.start_pie"))
		{
			OutResult = ExecuteStartPie(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.stop_pie"))
		{
			OutResult = ExecuteStopPie();
			return true;
		}

		if (ToolName == TEXT("unreal.execute_console_command"))
		{
			OutResult = ExecuteConsoleCommand(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.execute_python_file"))
		{
			OutResult = ExecutePythonFile(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.execute_python"))
		{
			OutResult = ExecutePythonCommand(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.map_check"))
		{
			OutResult = ExecuteMapCheck(ToolName);
			return true;
		}

		if (ToolName == TEXT("unreal.open_map"))
		{
			OutResult = ExecuteOpenMap(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.open_asset"))
		{
			OutResult = ExecuteOpenAsset(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.sync_content_browser"))
		{
			OutResult = ExecuteSyncContentBrowser(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.save_dirty_packages"))
		{
			OutResult = ExecuteSaveDirtyPackages(ToolName, Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.list_maps"))
		{
			OutResult = ExecuteListMaps();
			return true;
		}

		if (ToolName == TEXT("unreal.list_assets"))
		{
			OutResult = ExecuteListAssets(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.list_selected_assets"))
		{
			OutResult = ExecuteListSelectedAssets();
			return true;
		}

		return false;
	}
}
