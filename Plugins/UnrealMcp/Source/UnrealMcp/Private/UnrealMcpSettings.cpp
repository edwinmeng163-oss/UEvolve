#include "UnrealMcpSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealMcpModule.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "UnrealMcpSettings"

namespace
{
static constexpr int32 ProviderBackupSchemaVersion = 1;
}

const TCHAR* const UUnrealMcpSettings::BackupFileRelativePath = TEXT("UnrealMcp/providers.backup.json");
UUnrealMcpSettings::UUnrealMcpSettings()
{
	CategoryName = TEXT("Plugins");
	AllowedOrigins =
	{
		TEXT("http://localhost"),
		TEXT("http://127.0.0.1"),
		TEXT("https://localhost"),
		TEXT("https://127.0.0.1")
	};
}

void UUnrealMcpSettings::WriteProvidersBackup() const
{
	const UEnum* ProviderKindEnum = StaticEnum<EAiProviderKind>();
	if (!ProviderKindEnum)
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to write AI provider backup: EAiProviderKind enum is unavailable."));
		return;
	}
	const FString BackupPath = FPaths::Combine(FPaths::ProjectSavedDir(), BackupFileRelativePath);
	const FString BackupDir = FPaths::GetPath(BackupPath);
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*BackupDir) && !FileManager.MakeDirectory(*BackupDir, true))
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to create AI provider backup directory '%s'."), *BackupDir);
		return;
	}
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("schemaVersion"), ProviderBackupSchemaVersion);
	RootObject->SetStringField(TEXT("savedAt"), FDateTime::UtcNow().ToIso8601());
	RootObject->SetStringField(TEXT("activeProviderId"), ActiveProviderId);
	TArray<TSharedPtr<FJsonValue>> ProviderValues;
	ProviderValues.Reserve(Providers.Num());
	for (const FAiProviderConfig& Provider : Providers)
	{
		TSharedPtr<FJsonObject> ProviderObject = MakeShared<FJsonObject>();
		ProviderObject->SetStringField(TEXT("id"), Provider.Id);
		ProviderObject->SetStringField(TEXT("displayName"), Provider.DisplayName);
		ProviderObject->SetStringField(TEXT("kind"), ProviderKindEnum->GetNameStringByValue(static_cast<int64>(Provider.Kind)));
		ProviderObject->SetStringField(TEXT("baseUrl"), Provider.BaseUrl);
		ProviderObject->SetStringField(TEXT("apiKey"), Provider.ApiKey);
		ProviderObject->SetStringField(TEXT("model"), Provider.Model);
		ProviderObject->SetStringField(TEXT("reasoningEffort"), Provider.ReasoningEffort);
		ProviderObject->SetNumberField(TEXT("maxOutputTokens"), Provider.MaxOutputTokens);
		ProviderObject->SetStringField(TEXT("codexBinaryPath"), Provider.CodexBinaryPath);
		ProviderObject->SetStringField(TEXT("codexExtraArgs"), Provider.CodexExtraArgs);
		ProviderValues.Add(MakeShared<FJsonValueObject>(ProviderObject));
	}
	RootObject->SetArrayField(TEXT("providers"), ProviderValues);
	FString JsonText;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(RootObject, Writer))
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to serialize AI provider backup JSON."));
		return;
	}
	// Security note: this backup stores ApiKey in plaintext, matching the ini.
	// Saved/ is gitignored, and ProjectSavedDir is intended for machine-local state.
	if (!FFileHelper::SaveStringToFile(JsonText, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to write AI provider backup '%s'."), *BackupPath);
	}
}

bool UUnrealMcpSettings::LoadProvidersBackup()
{
	const FString BackupPath = FPaths::Combine(FPaths::ProjectSavedDir(), BackupFileRelativePath);
	if (!FPaths::FileExists(BackupPath))
	{
		return false;
	}
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *BackupPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to read AI provider backup '%s'."), *BackupPath);
		return false;
	}
	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to parse AI provider backup '%s'."), *BackupPath);
		return false;
	}
	double SchemaVersion = 0.0;
	if (!RootObject->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) ||
		SchemaVersion != static_cast<double>(ProviderBackupSchemaVersion))
	{
		FString FileSchemaVersion = TEXT("missing");
		const TSharedPtr<FJsonValue> SchemaVersionValue = RootObject->TryGetField(TEXT("schemaVersion"));
		if (SchemaVersionValue.IsValid())
		{
			FileSchemaVersion = SchemaVersionValue->Type == EJson::Number
				? FString::SanitizeFloat(SchemaVersionValue->AsNumber())
				: TEXT("non-numeric");
		}
		UE_LOG(LogUnrealMcp, Warning, TEXT("[UnrealMcp] Ignoring AI provider backup '%s' due to schemaVersion mismatch: file=%s expected=%d."),
			*BackupPath,
			*FileSchemaVersion,
			ProviderBackupSchemaVersion);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* ProviderValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("providers"), ProviderValues) || !ProviderValues)
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] AI provider backup '%s' has no providers array."), *BackupPath);
		return false;
	}
	const UEnum* ProviderKindEnum = StaticEnum<EAiProviderKind>();
	if (!ProviderKindEnum)
	{
		UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Failed to load AI provider backup: EAiProviderKind enum is unavailable."));
		return false;
	}
	FString RestoredActiveProviderId;
	RootObject->TryGetStringField(TEXT("activeProviderId"), RestoredActiveProviderId);
	TArray<FAiProviderConfig> RestoredProviders;
	RestoredProviders.Reserve(ProviderValues->Num());
	for (const TSharedPtr<FJsonValue>& ProviderValue : *ProviderValues)
	{
		if (!ProviderValue.IsValid() || ProviderValue->Type != EJson::Object || !ProviderValue->AsObject().IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject> ProviderObject = ProviderValue->AsObject();
		FString KindName;
		if (!ProviderObject->TryGetStringField(TEXT("kind"), KindName))
		{
			continue;
		}
		const int64 KindValue = ProviderKindEnum->GetValueByNameString(KindName);
		if (KindValue == INDEX_NONE || KindValue < 0 || KindValue > static_cast<int64>(EAiProviderKind::CodexAppServer))
		{
			UE_LOG(LogTemp, Warning, TEXT("[UnrealMcp] Skipping AI provider backup entry with unknown kind '%s'."), *KindName);
			continue;
		}
		FAiProviderConfig Provider;
		ProviderObject->TryGetStringField(TEXT("id"), Provider.Id);
		ProviderObject->TryGetStringField(TEXT("displayName"), Provider.DisplayName);
		Provider.Kind = static_cast<EAiProviderKind>(KindValue);
		ProviderObject->TryGetStringField(TEXT("baseUrl"), Provider.BaseUrl);
		ProviderObject->TryGetStringField(TEXT("apiKey"), Provider.ApiKey);
		ProviderObject->TryGetStringField(TEXT("model"), Provider.Model);
		ProviderObject->TryGetStringField(TEXT("reasoningEffort"), Provider.ReasoningEffort);
		double MaxOutputTokens = Provider.MaxOutputTokens;
		if (ProviderObject->TryGetNumberField(TEXT("maxOutputTokens"), MaxOutputTokens))
		{
			Provider.MaxOutputTokens = static_cast<int32>(MaxOutputTokens);
		}
		ProviderObject->TryGetStringField(TEXT("codexBinaryPath"), Provider.CodexBinaryPath);
		ProviderObject->TryGetStringField(TEXT("codexExtraArgs"), Provider.CodexExtraArgs);
		RestoredProviders.Add(MoveTemp(Provider));
	}
	if (RestoredProviders.Num() == 0)
	{
		return false;
	}
	Providers = MoveTemp(RestoredProviders);
	ActiveProviderId = RestoredActiveProviderId;
	return true;
}

void UUnrealMcpSettings::PostInitProperties()
{
	Super::PostInitProperties();
	if (!OpenAIApiKey.TrimStartAndEnd().IsEmpty())
	{
		const bool bHasMigratedDefaultProvider = Providers.ContainsByPredicate(
			[](const FAiProviderConfig& Provider)
			{
				return Provider.Id == TEXT("openai-default");
			});
		if (!bHasMigratedDefaultProvider && Providers.Num() == 0)
		{
			FAiProviderConfig& Provider = Providers.AddDefaulted_GetRef();
			Provider.Id = TEXT("openai-default");
			Provider.DisplayName = TEXT("OpenAI (migrated)");
			Provider.Kind = EAiProviderKind::OpenAiResponses;
			Provider.BaseUrl = OpenAIResponsesUrl;
			Provider.ApiKey = OpenAIApiKey;
			Provider.Model = OpenAIModel;
			Provider.ReasoningEffort = OpenAIReasoningEffort;
			Provider.MaxOutputTokens = AiMaxOutputTokens;
			ActiveProviderId = TEXT("openai-default");

			SaveConfig();
			UE_LOG(LogTemp, Display, TEXT("[UnrealMcp] Migrated legacy OpenAI settings to Providers[0]."));
		}
	}
	if (Providers.Num() == 0 && OpenAIApiKey.TrimStartAndEnd().IsEmpty() && LoadProvidersBackup())
	{
		SaveConfig();
		UE_LOG(LogTemp, Display, TEXT("[UnrealMcp] Restored AI providers from Saved/UnrealMcp/providers.backup.json after UE rewrote the ini section."));
	}
	if (Providers.Num() > 0)
	{
		WriteProvidersBackup();
	}
}

FName UUnrealMcpSettings::GetContainerName() const
{
	return TEXT("Project");
}

FName UUnrealMcpSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UUnrealMcpSettings::GetSectionName() const
{
	return TEXT("UnrealMcp");
}

const FAiProviderConfig* UUnrealMcpSettings::FindActiveProvider() const
{
	for (const FAiProviderConfig& Provider : Providers)
	{
		if (Provider.Id == ActiveProviderId)
		{
			return &Provider;
		}
	}

	return nullptr;
}

#if WITH_EDITOR
FText UUnrealMcpSettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "Unreal MCP");
}

FText UUnrealMcpSettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription", "Runs a local MCP server inside Unreal Editor and can optionally connect the in-editor chat panel to an AI model for tool-using assistant workflows.");
}

void UUnrealMcpSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	const FName MemberPropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	const FString PropertyNameString = PropertyName.ToString();
	const FString MemberPropertyNameString = MemberPropertyName.ToString();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UUnrealMcpSettings, ActiveProviderId)
		|| MemberPropertyName == GET_MEMBER_NAME_CHECKED(UUnrealMcpSettings, ActiveProviderId)
		|| PropertyNameString.Contains(TEXT("Provider"))
		|| MemberPropertyNameString.Contains(TEXT("Provider")))
	{
		WriteProvidersBackup();
	}
}
#endif

#undef LOCTEXT_NAMESPACE
