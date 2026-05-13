#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "FileUtilities/ZipArchiveWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealMcpSharedPathResolver.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	namespace
	{
		struct FToolPackageEntry
		{
			FString Name;
			TArray<uint8> Data;
			FDateTime TimestampUtc;
			FString Kind;
		};

		struct FExportableScaffoldInfo
		{
			FString ToolName;
			FString ScaffoldDir;
			FString SourceKind;
			bool bExists = false;
			bool bHasMetadata = false;
			bool bHasDescriptor = false;
			bool bHasTests = false;
			bool bHasRegistryPatch = false;
			TSharedPtr<FJsonObject> RegistryPatchObject;
		};

		const FString& ToolPackageKindFull()
		{
			static const FString Value = TEXT("full");
			return Value;
		}

		const FString& ToolPackageKindRegistryOnly()
		{
			static const FString Value = TEXT("registry-only");
			return Value;
		}

		FString BytesToUtf8String(const TArray<uint8>& Data)
		{
			if (Data.Num() == 0)
			{
				return FString();
			}
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Data.GetData()), Data.Num());
			return FString(Converter.Length(), Converter.Get());
		}

		TArray<uint8> Utf8StringToBytes(const FString& Text)
		{
			FTCHARToUTF8 Converter(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
			return Bytes;
		}

		uint32 Sha256RotateRight(uint32 Value, uint32 Shift)
		{
			return (Value >> Shift) | (Value << (32 - Shift));
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			static const uint32 Constants[64] =
			{
				0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
				0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
				0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
				0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
				0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
				0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
				0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
				0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
			};

			TArray<uint8> Padded = Bytes;
			const uint64 BitLength = static_cast<uint64>(Bytes.Num()) * 8ull;
			Padded.Add(0x80u);
			while ((Padded.Num() % 64) != 56)
			{
				Padded.Add(0u);
			}
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Padded.Add(static_cast<uint8>((BitLength >> Shift) & 0xffu));
			}

			uint32 Hash[8] =
			{
				0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
			};

			for (int32 ChunkOffset = 0; ChunkOffset < Padded.Num(); ChunkOffset += 64)
			{
				uint32 Words[64] = {};
				for (int32 Index = 0; Index < 16; ++Index)
				{
					const int32 Offset = ChunkOffset + Index * 4;
					Words[Index] =
						(static_cast<uint32>(Padded[Offset]) << 24)
						| (static_cast<uint32>(Padded[Offset + 1]) << 16)
						| (static_cast<uint32>(Padded[Offset + 2]) << 8)
						| static_cast<uint32>(Padded[Offset + 3]);
				}
				for (int32 Index = 16; Index < 64; ++Index)
				{
					const uint32 S0 = Sha256RotateRight(Words[Index - 15], 7) ^ Sha256RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
					const uint32 S1 = Sha256RotateRight(Words[Index - 2], 17) ^ Sha256RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
					Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
				}

				uint32 A = Hash[0];
				uint32 B = Hash[1];
				uint32 C = Hash[2];
				uint32 D = Hash[3];
				uint32 E = Hash[4];
				uint32 F = Hash[5];
				uint32 G = Hash[6];
				uint32 H = Hash[7];

				for (int32 Index = 0; Index < 64; ++Index)
				{
					const uint32 S1 = Sha256RotateRight(E, 6) ^ Sha256RotateRight(E, 11) ^ Sha256RotateRight(E, 25);
					const uint32 Choice = (E & F) ^ ((~E) & G);
					const uint32 Temp1 = H + S1 + Choice + Constants[Index] + Words[Index];
					const uint32 S0 = Sha256RotateRight(A, 2) ^ Sha256RotateRight(A, 13) ^ Sha256RotateRight(A, 22);
					const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
					const uint32 Temp2 = S0 + Majority;

					H = G;
					G = F;
					F = E;
					E = D + Temp1;
					D = C;
					C = B;
					B = A;
					A = Temp1 + Temp2;
				}

				Hash[0] += A;
				Hash[1] += B;
				Hash[2] += C;
				Hash[3] += D;
				Hash[4] += E;
				Hash[5] += F;
				Hash[6] += G;
				Hash[7] += H;
			}

			static const TCHAR Hex[] = TEXT("0123456789abcdef");
			FString Result;
			Result.Reserve(64);
			for (uint32 Word : Hash)
			{
				for (int32 Shift = 24; Shift >= 0; Shift -= 8)
				{
					const uint8 Byte = static_cast<uint8>((Word >> Shift) & 0xffu);
					Result.AppendChar(Hex[(Byte >> 4) & 0x0f]);
					Result.AppendChar(Hex[Byte & 0x0f]);
				}
			}
			return Result;
		}

		FString SanitizePackageSegment(FString Value)
		{
			Value = Value.TrimStartAndEnd();
			FString Result;
			for (TCHAR Character : Value)
			{
				if (FChar::IsAlnum(Character) || Character == TEXT('.') || Character == TEXT('_') || Character == TEXT('-'))
				{
					Result.AppendChar(Character);
				}
				else
				{
					Result.AppendChar(TEXT('_'));
				}
			}
			while (Result.Contains(TEXT("__")))
			{
				Result = Result.Replace(TEXT("__"), TEXT("_"));
			}
			Result.RemoveFromStart(TEXT("."));
			Result.RemoveFromEnd(TEXT("."));
			return Result.IsEmpty() ? TEXT("tool-package") : Result.Left(120);
		}

		bool IsSafePackageEntryName(const FString& Name)
		{
			FString Normalized = Name;
			FPaths::NormalizeFilename(Normalized);
			return !Normalized.IsEmpty()
				&& FPaths::IsRelative(Normalized)
				&& !Normalized.StartsWith(TEXT("/"))
				&& !Normalized.Contains(TEXT(".."))
				&& !Normalized.Contains(TEXT("\\"))
				&& !Normalized.StartsWith(TEXT("."));
		}

		FString NormalizePackageEntryName(const FString& Name)
		{
			FString Normalized = Name;
			FPaths::NormalizeFilename(Normalized);
			while (Normalized.StartsWith(TEXT("/")))
			{
				Normalized.RightChopInline(1);
			}
			return Normalized;
		}

		FString GetPackageOutputRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/Packages")));
		}

		FString GetSharedRepoRoot()
		{
			TArray<FString> RegistryCandidates;
			FString RegistryRoot;
			ResolveSharedRepoRoot(TEXT("UnrealMcpToolRegistry"), { TEXT("tools.json") }, RegistryRoot, RegistryCandidates);
			const FString ToolsRoot = FPaths::GetPath(RegistryRoot);
			const FString RepoRoot = FPaths::GetPath(ToolsRoot);
			return RepoRoot.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()) : RepoRoot;
		}

		FString GetSourceRepoCommit()
		{
			const FString RepoRoot = GetSharedRepoRoot();
			FString HeadText;
			const FString HeadPath = FPaths::Combine(RepoRoot, TEXT(".git/HEAD"));
			if (!FFileHelper::LoadFileToString(HeadText, *HeadPath))
			{
				return TEXT("unknown");
			}
			HeadText = HeadText.TrimStartAndEnd();
			if (HeadText.StartsWith(TEXT("ref:")))
			{
				FString RefPath = HeadText.RightChop(4).TrimStartAndEnd();
				FString RefText;
				if (FFileHelper::LoadFileToString(RefText, *FPaths::Combine(RepoRoot, TEXT(".git"), RefPath)))
				{
					return RefText.TrimStartAndEnd();
				}
			}
			return HeadText.IsEmpty() ? TEXT("unknown") : HeadText;
		}

		TSharedPtr<FJsonObject> MakeRegistryEntryJson(const FToolRegistryEntry& Entry)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Entry.Name);
			Object->SetStringField(TEXT("category"), Entry.Category);
			Object->SetStringField(TEXT("handlerName"), Entry.HandlerName.IsEmpty() ? Entry.Name : Entry.HandlerName);
			Object->SetStringField(TEXT("exposure"), Entry.Exposure == EToolExposure::LegacyHidden ? TEXT("legacy_hidden") : TEXT("visible"));
			Object->SetStringField(TEXT("riskLevel"), LexToString(Entry.Policy.RiskLevel));
			Object->SetBoolField(TEXT("requiresWrite"), Entry.Policy.bRequiresWrite);
			Object->SetBoolField(TEXT("requiresBuild"), Entry.Policy.bRequiresBuild);
			Object->SetBoolField(TEXT("requiresExternalProcess"), Entry.Policy.bRequiresExternalProcess);
			Object->SetBoolField(TEXT("requiresRestart"), Entry.Policy.bRequiresRestart);
			Object->SetBoolField(TEXT("requiresProjectMemory"), Entry.Policy.bRequiresProjectMemory);
			Object->SetBoolField(TEXT("requiresLock"), Entry.Policy.bRequiresLock);
			Object->SetBoolField(TEXT("dryRunSupport"), Entry.Policy.bDryRunSupport);
			Object->SetBoolField(TEXT("preflightSupport"), Entry.Policy.bPreflightSupport);
			Object->SetBoolField(TEXT("postcheckSupport"), Entry.Policy.bPostcheckSupport);
			Object->SetStringField(TEXT("testCoverage"), Entry.Policy.TestCoverage.IsEmpty() ? TEXT("missing") : Entry.Policy.TestCoverage);
			Object->SetStringField(TEXT("owner"), Entry.Policy.Owner.IsEmpty() ? TEXT("UEvolve Core") : Entry.Policy.Owner);
			Object->SetStringField(TEXT("docsPath"), Entry.Policy.DocsPath.IsEmpty() ? TEXT("README.md#tool-coverage") : Entry.Policy.DocsPath);
			Object->SetStringField(TEXT("reason"), Entry.Policy.Reason);
			Object->SetStringField(TEXT("notes"), Entry.Notes);
			return Object;
		}

		bool FindRegistryEntryJson(const FString& ToolName, TSharedPtr<FJsonObject>& OutEntryObject)
		{
			TArray<FString> RegistryCandidates;
			FString RegistryRoot;
			ResolveSharedRepoRoot(TEXT("UnrealMcpToolRegistry"), { TEXT("tools.json") }, RegistryRoot, RegistryCandidates);
			const FString RegistryPath = FPaths::Combine(RegistryRoot, TEXT("tools.json"));
			TSharedPtr<FJsonObject> RegistryObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(RegistryPath, RegistryObject, FailureReason) || !RegistryObject.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!RegistryObject->TryGetArrayField(TEXT("tools"), Tools) || !Tools)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}
				FString Name;
				if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), Name) && Name.Equals(ToolName, ESearchCase::CaseSensitive))
				{
					OutEntryObject = ToolValue->AsObject();
					return true;
				}
			}
			return false;
		}

		void AddPackageEntry(TArray<FToolPackageEntry>& Entries, const FString& Name, const TArray<uint8>& Data, const FString& Kind, const FDateTime& TimestampUtc = FDateTime::UtcNow())
		{
			const FString EntryName = NormalizePackageEntryName(Name);
			if (!IsSafePackageEntryName(EntryName))
			{
				return;
			}
			for (FToolPackageEntry& Existing : Entries)
			{
				if (Existing.Name.Equals(EntryName, ESearchCase::IgnoreCase))
				{
					Existing.Data = Data;
					Existing.Kind = Kind;
					Existing.TimestampUtc = TimestampUtc;
					return;
				}
			}
			Entries.Add({ EntryName, Data, TimestampUtc, Kind });
		}

		bool AddFilePackageEntry(TArray<FToolPackageEntry>& Entries, const FString& SourcePath, const FString& EntryName, const FString& Kind)
		{
			TArray<uint8> Data;
			if (!FFileHelper::LoadFileToArray(Data, *SourcePath))
			{
				return false;
			}
			AddPackageEntry(Entries, EntryName, Data, Kind, IFileManager::Get().GetTimeStamp(*SourcePath));
			return true;
		}

		FString MakeRelativePathUnder(const FString& Path, const FString& Root)
		{
			FString Relative = FPaths::ConvertRelativePathToFull(Path);
			FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
			FPaths::NormalizeDirectoryName(FullRoot);
			if (!FullRoot.EndsWith(TEXT("/")))
			{
				FullRoot += TEXT("/");
			}
			FPaths::MakePathRelativeTo(Relative, *FullRoot);
			FPaths::NormalizeFilename(Relative);
			return Relative;
		}

		void AddDirectoryPackageEntries(TArray<FToolPackageEntry>& Entries, const FString& SourceDir, const FString& EntryPrefix, const FString& Kind)
		{
			if (!FPaths::DirectoryExists(SourceDir))
			{
				return;
			}
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*"), true, false);
			Files.Sort();
			for (const FString& File : Files)
			{
				const FString Relative = MakeRelativePathUnder(File, SourceDir);
				AddFilePackageEntry(Entries, File, FPaths::Combine(EntryPrefix, Relative), Kind);
			}
		}

		bool TestFileTargetsTool(const FString& TestFile, const FString& ToolName)
		{
			TSharedPtr<FJsonObject> TestObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(TestFile, TestObject, FailureReason) || !TestObject.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* RequestObject = nullptr;
			TSharedPtr<FJsonObject> EffectiveRequest = TestObject;
			if (TestObject->TryGetObjectField(TEXT("request"), RequestObject) && RequestObject && (*RequestObject).IsValid())
			{
				EffectiveRequest = *RequestObject;
			}
			const TSharedPtr<FJsonObject>* Params = nullptr;
			if (!EffectiveRequest->TryGetObjectField(TEXT("params"), Params) || !Params || !(*Params).IsValid())
			{
				return false;
			}
			FString Name;
			return (*Params)->TryGetStringField(TEXT("name"), Name) && Name.Equals(ToolName, ESearchCase::CaseSensitive);
		}

		void AddMatchingTestEntries(TArray<FToolPackageEntry>& Entries, const FString& ToolName)
		{
			TArray<FString> TestRootCandidates;
			FString TestRoot;
			if (!ResolveSharedRepoRoot(TEXT("UnrealMcpTests"), { TEXT("*.json") }, TestRoot, TestRootCandidates))
			{
				return;
			}
			TArray<FString> TestFiles;
			IFileManager::Get().FindFilesRecursive(TestFiles, *TestRoot, TEXT("*.json"), true, false);
			TestFiles.Sort();
			for (const FString& TestFile : TestFiles)
			{
				if (TestFileTargetsTool(TestFile, ToolName))
				{
					AddFilePackageEntry(Entries, TestFile, FPaths::Combine(TEXT("tests"), MakeRelativePathUnder(TestFile, TestRoot)), TEXT("test"));
				}
			}
		}

		void AddDocsEntry(TArray<FToolPackageEntry>& Entries, const TSharedPtr<FJsonObject>& RegistryEntry)
		{
			FString DocsPath;
			if (!RegistryEntry.IsValid() || !RegistryEntry->TryGetStringField(TEXT("docsPath"), DocsPath) || DocsPath.TrimStartAndEnd().IsEmpty())
			{
				return;
			}
			DocsPath = DocsPath.TrimStartAndEnd();
			FString Anchor;
			if (DocsPath.Split(TEXT("#"), &DocsPath, &Anchor))
			{
				DocsPath = DocsPath.TrimStartAndEnd();
			}
			if (DocsPath.IsEmpty())
			{
				return;
			}
			const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(GetSharedRepoRoot(), DocsPath));
			if (FPaths::FileExists(FullPath))
			{
				AddFilePackageEntry(Entries, FullPath, FPaths::Combine(TEXT("docs"), DocsPath), TEXT("doc"));
			}
		}

		bool ResolveOptionalScaffoldDir(const FJsonObject& Arguments, const FString& ToolName, FString& OutScaffoldDir, FString& OutFailureReason)
		{
			FString ScaffoldDir;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
			if (!ScaffoldDir.IsEmpty())
			{
				return ResolveProjectPathInsideProject(ScaffoldDir, OutScaffoldDir, OutFailureReason);
			}

			FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			FString ResolvedOutputRoot;
			if (!ResolveProjectPathInsideProject(OutputRoot, ResolvedOutputRoot, OutFailureReason))
			{
				return false;
			}
			OutScaffoldDir = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
			return true;
		}

		bool HasDescriptorFirstPatchFragment(const FString& ScaffoldDir)
		{
			return FPaths::FileExists(FPaths::Combine(ScaffoldDir, TEXT("ToolRegistryPatch.json")))
				|| FPaths::FileExists(FPaths::Combine(ScaffoldDir, TEXT("ToolRegistrar.patch.cpp")))
				|| FPaths::FileExists(FPaths::Combine(ScaffoldDir, TEXT("CategoryHandlerFunction.patch.cpp")));
		}

		bool HasScaffoldTests(const FString& ScaffoldDir)
		{
			if (FPaths::FileExists(FPaths::Combine(ScaffoldDir, TEXT("TestRequest.json"))))
			{
				return true;
			}
			TArray<FString> TestFiles;
			IFileManager::Get().FindFilesRecursive(TestFiles, *ScaffoldDir, TEXT("*Test*.json"), true, false);
			return TestFiles.Num() > 0;
		}

		TSharedPtr<FJsonObject> LoadRegistryPatchObject(const FString& ScaffoldDir)
		{
			TSharedPtr<FJsonObject> RegistryPatchObject;
			FString FailureReason;
			if (LoadJsonObjectFromFile(FPaths::Combine(ScaffoldDir, TEXT("ToolRegistryPatch.json")), RegistryPatchObject, FailureReason)
				&& RegistryPatchObject.IsValid())
			{
				return RegistryPatchObject;
			}
			return nullptr;
		}

		FString ReadScaffoldMetadataToolName(const FString& ScaffoldDir)
		{
			TSharedPtr<FJsonObject> MetadataObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(FPaths::Combine(ScaffoldDir, TEXT("ScaffoldMetadata.json")), MetadataObject, FailureReason)
				|| !MetadataObject.IsValid())
			{
				return FString();
			}
			FString ToolName;
			MetadataObject->TryGetStringField(TEXT("toolName"), ToolName);
			return ToolName.TrimStartAndEnd();
		}

		FExportableScaffoldInfo MakeScaffoldInfo(const FString& ScaffoldDir, const FString& SourceKind)
		{
			FExportableScaffoldInfo Info;
			Info.ScaffoldDir = FPaths::ConvertRelativePathToFull(ScaffoldDir);
			FPaths::NormalizeFilename(Info.ScaffoldDir);
			Info.SourceKind = SourceKind;
			Info.bExists = FPaths::DirectoryExists(Info.ScaffoldDir);
			Info.bHasMetadata = FPaths::FileExists(FPaths::Combine(Info.ScaffoldDir, TEXT("ScaffoldMetadata.json")));
			Info.bHasDescriptor = HasDescriptorFirstPatchFragment(Info.ScaffoldDir);
			Info.bHasTests = HasScaffoldTests(Info.ScaffoldDir);
			Info.RegistryPatchObject = LoadRegistryPatchObject(Info.ScaffoldDir);
			Info.bHasRegistryPatch = Info.RegistryPatchObject.IsValid();
			Info.ToolName = ReadScaffoldMetadataToolName(Info.ScaffoldDir);
			if (Info.ToolName.IsEmpty() && Info.RegistryPatchObject.IsValid())
			{
				Info.RegistryPatchObject->TryGetStringField(TEXT("name"), Info.ToolName);
				Info.ToolName = Info.ToolName.TrimStartAndEnd();
			}
			if (Info.ToolName.IsEmpty())
			{
				Info.ToolName = FString::Printf(TEXT("unreal.%s"), *SanitizeMcpToolIdForPath(FPaths::GetCleanFilename(Info.ScaffoldDir)));
			}
			return Info;
		}

		TSharedPtr<FJsonObject> MakeScaffoldInfoObject(const FExportableScaffoldInfo& Info)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("toolName"), Info.ToolName);
			Object->SetStringField(TEXT("scaffoldDir"), Info.ScaffoldDir);
			Object->SetStringField(TEXT("sourceKind"), Info.SourceKind);
			Object->SetBoolField(TEXT("exists"), Info.bExists);
			Object->SetBoolField(TEXT("hasMetadata"), Info.bHasMetadata);
			Object->SetBoolField(TEXT("hasDescriptor"), Info.bHasDescriptor);
			Object->SetBoolField(TEXT("hasTests"), Info.bHasTests);
			Object->SetBoolField(TEXT("hasRegistryPatch"), Info.bHasRegistryPatch);
			return Object;
		}

		void AddSpecificScaffoldCandidates(const FString& BareName, TArray<FExportableScaffoldInfo>& OutCandidates)
		{
			TArray<FString> ProjectRootCandidates;
			FString ProjectScaffoldRoot;
			ResolveSharedRepoRoot(TEXT("UnrealMcpToolScaffolds"), {}, ProjectScaffoldRoot, ProjectRootCandidates);
			if (!ProjectScaffoldRoot.IsEmpty())
			{
				OutCandidates.Add(MakeScaffoldInfo(FPaths::Combine(ProjectScaffoldRoot, BareName), TEXT("project")));
			}

			const FString SavedScaffoldRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/TestScaffolds")));
			OutCandidates.Add(MakeScaffoldInfo(FPaths::Combine(SavedScaffoldRoot, BareName), TEXT("savedTest")));
		}

		void AddScaffoldInfosFromRoot(const FString& Root, const FString& SourceKind, TArray<FExportableScaffoldInfo>& OutInfos)
		{
			if (!FPaths::DirectoryExists(Root))
			{
				return;
			}

			TArray<FString> MetadataFiles;
			IFileManager::Get().FindFilesRecursive(MetadataFiles, *Root, TEXT("ScaffoldMetadata.json"), true, false);
			MetadataFiles.Sort();
			TSet<FString> SeenDirs;
			for (const FString& MetadataFile : MetadataFiles)
			{
				const FString ScaffoldDir = FPaths::GetPath(MetadataFile);
				if (SeenDirs.Contains(ScaffoldDir))
				{
					continue;
				}
				SeenDirs.Add(ScaffoldDir);
				OutInfos.Add(MakeScaffoldInfo(ScaffoldDir, SourceKind));
			}
		}

		void FindExportableScaffoldInfos(TArray<FExportableScaffoldInfo>& OutInfos)
		{
			OutInfos.Reset();

			TArray<FString> ProjectRootCandidates;
			FString ProjectScaffoldRoot;
			ResolveSharedRepoRoot(TEXT("UnrealMcpToolScaffolds"), { TEXT("ScaffoldMetadata.json") }, ProjectScaffoldRoot, ProjectRootCandidates);
			AddScaffoldInfosFromRoot(ProjectScaffoldRoot, TEXT("project"), OutInfos);

			const FString SavedScaffoldRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/TestScaffolds")));
			AddScaffoldInfosFromRoot(SavedScaffoldRoot, TEXT("savedTest"), OutInfos);
		}

		bool FindPortableScaffoldForTool(const FString& ToolName, FExportableScaffoldInfo& OutInfo, TArray<TSharedPtr<FJsonValue>>& OutCandidateValues)
		{
			TArray<FExportableScaffoldInfo> Candidates;
			AddSpecificScaffoldCandidates(SanitizeMcpToolIdForPath(ToolName), Candidates);

			bool bHasFallback = false;
			for (const FExportableScaffoldInfo& Candidate : Candidates)
			{
				OutCandidateValues.Add(MakeShared<FJsonValueObject>(MakeScaffoldInfoObject(Candidate)));
				if (!bHasFallback && Candidate.bExists)
				{
					OutInfo = Candidate;
					bHasFallback = true;
				}
				if (Candidate.ToolName.Equals(ToolName, ESearchCase::CaseSensitive)
					&& Candidate.bHasMetadata
					&& Candidate.bHasDescriptor)
				{
					OutInfo = Candidate;
					return true;
				}
			}
			return false;
		}

		FString MakeNoPortableScaffoldError(const FString& ToolName)
		{
			return FString::Printf(
				TEXT("Tool '%s' has no portable scaffold under Tools/UnrealMcpToolScaffolds or Saved/UnrealMcp/TestScaffolds. Only tools you authored via unreal.scaffold_mcp_tool can be exported. Use unreal.tools.list_exportable to see what is shippable."),
				*ToolName);
		}

		TSharedPtr<FJsonObject> MakeEntryManifestObject(const FToolPackageEntry& Entry)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("path"), Entry.Name);
			Object->SetStringField(TEXT("kind"), Entry.Kind);
			Object->SetNumberField(TEXT("sizeBytes"), Entry.Data.Num());
			Object->SetStringField(TEXT("sha256"), Sha256Bytes(Entry.Data));
			return Object;
		}

		TSharedPtr<FJsonObject> BuildManifestObject(const FString& ToolName, const FString& Version, const FString& PackageKind, const TArray<FToolPackageEntry>& Entries)
		{
			TArray<TSharedPtr<FJsonValue>> FileValues;
			for (const FToolPackageEntry& Entry : Entries)
			{
				if (Entry.Name == TEXT("manifest.json"))
				{
					continue;
				}
				FileValues.Add(MakeShared<FJsonValueObject>(MakeEntryManifestObject(Entry)));
			}

			TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
			Manifest->SetStringField(TEXT("schema"), TEXT("UEvolve.ToolPackage.v1"));
			Manifest->SetStringField(TEXT("kind"), PackageKind);
			Manifest->SetStringField(TEXT("toolName"), ToolName);
			Manifest->SetStringField(TEXT("version"), Version);
			Manifest->SetStringField(TEXT("created_at"), FDateTime::UtcNow().ToIso8601());
			Manifest->SetStringField(TEXT("source_repo_commit"), GetSourceRepoCommit());
			Manifest->SetArrayField(TEXT("files"), FileValues);
			return Manifest;
		}

		bool WriteZipPackage(const FString& PackagePath, const TArray<FToolPackageEntry>& Entries, FString& OutFailureReason)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackagePath), true);
			IFileHandle* FileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*PackagePath);
			if (!FileHandle)
			{
				OutFailureReason = FString::Printf(TEXT("Failed to create package zip '%s'."), *PackagePath);
				return false;
			}
			{
				FZipArchiveWriter Writer(FileHandle, EZipArchiveOptions::Deflate | EZipArchiveOptions::RemoveDuplicate);
				for (const FToolPackageEntry& Entry : Entries)
				{
					Writer.AddFile(Entry.Name, Entry.Data, Entry.TimestampUtc);
				}
			}
			return true;
		}

		bool ResolvePackagePathForRead(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason)
		{
			FString CleanPath = RequestedPath.TrimStartAndEnd();
			if (CleanPath.IsEmpty())
			{
				OutFailureReason = TEXT("packagePath is required.");
				return false;
			}
			if (FPaths::IsRelative(CleanPath))
			{
				FString ProjectRelative = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CleanPath));
				if (FPaths::FileExists(ProjectRelative))
				{
					OutPath = ProjectRelative;
					return true;
				}
				OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(GetPackageOutputRoot(), CleanPath));
				return true;
			}
			OutPath = FPaths::ConvertRelativePathToFull(CleanPath);
			return true;
		}

		bool ReadZipPackage(const FString& PackagePath, TMap<FString, TArray<uint8>>& OutEntries, FString& OutFailureReason)
		{
			IFileHandle* FileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*PackagePath);
			if (!FileHandle)
			{
				OutFailureReason = FString::Printf(TEXT("Failed to open package zip '%s'."), *PackagePath);
				return false;
			}

			FZipArchiveReader Reader(FileHandle);
			if (!Reader.IsValid())
			{
				OutFailureReason = FString::Printf(TEXT("Package is not a valid zip archive: %s"), *PackagePath);
				return false;
			}

			for (const FString& FileName : Reader.GetFileNames())
			{
				const FString EntryName = NormalizePackageEntryName(FileName);
				if (!IsSafePackageEntryName(EntryName))
				{
					OutFailureReason = FString::Printf(TEXT("Package contains unsafe entry path '%s'."), *FileName);
					return false;
				}
				TArray<uint8> Data;
				if (!Reader.TryReadFile(FileName, Data))
				{
					OutFailureReason = FString::Printf(TEXT("Failed to read package entry '%s'."), *FileName);
					return false;
				}
				OutEntries.Add(EntryName, MoveTemp(Data));
			}
			return true;
		}

		bool ValidateManifestHashes(const TSharedPtr<FJsonObject>& Manifest, const TMap<FString, TArray<uint8>>& Entries, TArray<TSharedPtr<FJsonValue>>& OutFileValues, FString& OutFailureReason)
		{
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("files"), Files) || !Files)
			{
				OutFailureReason = TEXT("Package manifest is missing files[].");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& FileValue : *Files)
			{
				if (!FileValue.IsValid() || FileValue->Type != EJson::Object || !FileValue->AsObject().IsValid())
				{
					OutFailureReason = TEXT("Package manifest files[] contains a non-object entry.");
					return false;
				}
				TSharedPtr<FJsonObject> FileObject = FileValue->AsObject();
				FString Path;
				FString ExpectedHash;
				if (!FileObject->TryGetStringField(TEXT("path"), Path) || !FileObject->TryGetStringField(TEXT("sha256"), ExpectedHash))
				{
					OutFailureReason = TEXT("Package manifest file entry is missing path or sha256.");
					return false;
				}
				Path = NormalizePackageEntryName(Path);
				const TArray<uint8>* Data = Entries.Find(Path);
				if (!Data)
				{
					OutFailureReason = FString::Printf(TEXT("Package is missing manifest entry '%s'."), *Path);
					return false;
				}
				const FString ActualHash = Sha256Bytes(*Data);
				if (!ActualHash.Equals(ExpectedHash, ESearchCase::IgnoreCase))
				{
					OutFailureReason = FString::Printf(TEXT("Package entry '%s' sha256 mismatch."), *Path);
					return false;
				}
				TSharedPtr<FJsonObject> FileSummary = MakeShared<FJsonObject>();
				FileSummary->SetStringField(TEXT("path"), Path);
				FileSummary->SetStringField(TEXT("sha256"), ActualHash);
				FileSummary->SetNumberField(TEXT("sizeBytes"), Data->Num());
				FString Kind;
				if (FileObject->TryGetStringField(TEXT("kind"), Kind))
				{
					FileSummary->SetStringField(TEXT("kind"), Kind);
				}
				OutFileValues.Add(MakeShared<FJsonValueObject>(FileSummary));
			}
			return true;
		}

		bool WriteBytesToFile(const FString& Path, const TArray<uint8>& Data, bool bOverwrite, FString& OutFailureReason)
		{
			if (FPaths::FileExists(Path) && !bOverwrite)
			{
				OutFailureReason = FString::Printf(TEXT("Refusing to overwrite existing file '%s'."), *Path);
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveArrayToFile(Data, *Path))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write file '%s'."), *Path);
				return false;
			}
			return true;
		}

		bool LoadRegistryForAppend(const FString& RegistryPath, TSharedPtr<FJsonObject>& OutRegistryObject, const TSharedPtr<FJsonObject>& NewEntry, bool& bOutDuplicate, FString& OutFailureReason)
		{
			bOutDuplicate = false;
			if (!LoadJsonObjectFromFile(RegistryPath, OutRegistryObject, OutFailureReason) || !OutRegistryObject.IsValid())
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!OutRegistryObject->TryGetArrayField(TEXT("tools"), Tools) || !Tools)
			{
				OutFailureReason = FString::Printf(TEXT("ToolRegistry '%s' is missing tools[]."), *RegistryPath);
				return false;
			}

			FString NewName;
			NewEntry->TryGetStringField(TEXT("name"), NewName);
			for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
			{
				if (ToolValue.IsValid() && ToolValue->Type == EJson::Object && ToolValue->AsObject().IsValid())
				{
					FString ExistingName;
					if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), ExistingName) && ExistingName.Equals(NewName, ESearchCase::CaseSensitive))
					{
						bOutDuplicate = true;
						return true;
					}
				}
			}
			return true;
		}

		FString MakeRegistryNotInitializedMessage()
		{
			return TEXT("unreal.tools.import_package requires a writable `Tools/UnrealMcpToolRegistry/tools.json` at the project root. Drop-in plugin installs do not provision this.\n")
				TEXT("To enable tool-package import:\n")
				TEXT("1. Copy `<UE>/Engine/Plugins/UnrealMcp/Resources/ToolRegistry/tools.json` (or the plugin's `Resources/ToolRegistry/tools.json`) into `<YourProject>/Tools/UnrealMcpToolRegistry/tools.json`.\n")
				TEXT("2. Add `Tools/UnrealMcpToolRegistry/` to your project's gitignore allowlist if you want to track imports in your project's git.\n")
				TEXT("3. Retry the import.");
		}
	}

	FUnrealMcpExecutionResult ExportToolPackage(const FJsonObject& Arguments)
	{
		FString ToolName;
		FString Version;
		FString PackagePath;
		bool bDryRun = true;
		bool bAllowRegistryOnly = false;
		Arguments.TryGetStringField(TEXT("toolName"), ToolName);
		Arguments.TryGetStringField(TEXT("version"), Version);
		Arguments.TryGetStringField(TEXT("packagePath"), PackagePath);
		Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
		Arguments.TryGetBoolField(TEXT("allowRegistryOnly"), bAllowRegistryOnly);
		ToolName = ToolName.TrimStartAndEnd();
		Version = Version.TrimStartAndEnd();
		if (ToolName.IsEmpty())
		{
			return MakeExecutionResult(TEXT("toolName is required."), nullptr, true);
		}
		if (Version.IsEmpty())
		{
			Version = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
		}

		FExportableScaffoldInfo ScaffoldInfo;
		TArray<TSharedPtr<FJsonValue>> ScaffoldCandidateValues;
		const bool bScaffoldGatePassed = FindPortableScaffoldForTool(ToolName, ScaffoldInfo, ScaffoldCandidateValues);

		TSharedPtr<FJsonObject> RegistryEntryObject;
		FString RegistryEntrySource;
		if (!FindRegistryEntryJson(ToolName, RegistryEntryObject))
		{
			FString ScaffoldPatchToolName;
			if (ScaffoldInfo.RegistryPatchObject.IsValid())
			{
				ScaffoldInfo.RegistryPatchObject->TryGetStringField(TEXT("name"), ScaffoldPatchToolName);
				ScaffoldPatchToolName = ScaffoldPatchToolName.TrimStartAndEnd();
			}
			if (ScaffoldInfo.RegistryPatchObject.IsValid() && ScaffoldPatchToolName.Equals(ToolName, ESearchCase::CaseSensitive))
			{
				RegistryEntryObject = ScaffoldInfo.RegistryPatchObject;
				RegistryEntrySource = TEXT("scaffoldPatch");
			}
			else if (const FToolRegistryEntry* RegistryEntry = FindToolRegistryEntry(ToolName))
			{
				RegistryEntryObject = MakeRegistryEntryJson(*RegistryEntry);
				RegistryEntrySource = TEXT("liveRegistry");
			}
		}
		else
		{
			RegistryEntrySource = TEXT("tools.json");
		}

		const bool bHasRegistryEntry = RegistryEntryObject.IsValid();
		const bool bPortableScaffold = bScaffoldGatePassed && bHasRegistryEntry;
		const FString PackageKind = bPortableScaffold ? ToolPackageKindFull() : ToolPackageKindRegistryOnly();

		TSharedPtr<FJsonObject> GateObject = MakeShared<FJsonObject>();
		GateObject->SetBoolField(TEXT("passed"), bPortableScaffold);
		GateObject->SetStringField(TEXT("kind"), PackageKind);
		GateObject->SetBoolField(TEXT("allowRegistryOnly"), bAllowRegistryOnly);
		GateObject->SetBoolField(TEXT("hasRegistryEntry"), bHasRegistryEntry);
		GateObject->SetStringField(TEXT("registryEntrySource"), RegistryEntrySource);
		GateObject->SetObjectField(TEXT("selectedScaffold"), MakeScaffoldInfoObject(ScaffoldInfo));
		GateObject->SetArrayField(TEXT("candidateScaffolds"), ScaffoldCandidateValues);

		if (!bPortableScaffold && !bAllowRegistryOnly)
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("tools_export_package"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("version"), Version);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("allowRegistryOnly"), bAllowRegistryOnly);
			StructuredContent->SetStringField(TEXT("kind"), TEXT("refused"));
			StructuredContent->SetObjectField(TEXT("scaffoldGate"), GateObject);
			return MakeExecutionResult(MakeNoPortableScaffoldError(ToolName), StructuredContent, true);
		}

		if (!bHasRegistryEntry)
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("tools_export_package"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("version"), Version);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("allowRegistryOnly"), bAllowRegistryOnly);
			StructuredContent->SetStringField(TEXT("kind"), PackageKind);
			StructuredContent->SetObjectField(TEXT("scaffoldGate"), GateObject);
			return MakeExecutionResult(FString::Printf(TEXT("Tool '%s' has no ToolRegistry entry to package."), *ToolName), StructuredContent, true);
		}

		TArray<FToolPackageEntry> Entries;
		AddPackageEntry(Entries, TEXT("registry/tool.json"), Utf8StringToBytes(JsonObjectToString(RegistryEntryObject) + TEXT("\n")), TEXT("registry"));

		if (bPortableScaffold)
		{
			AddDirectoryPackageEntries(Entries, ScaffoldInfo.ScaffoldDir, TEXT("scaffold"), TEXT("scaffold"));
		}
		FString FailureReason;
		AddMatchingTestEntries(Entries, ToolName);
		AddDocsEntry(Entries, RegistryEntryObject);

		TSharedPtr<FJsonObject> Manifest = BuildManifestObject(ToolName, Version, PackageKind, Entries);
		AddPackageEntry(Entries, TEXT("manifest.json"), Utf8StringToBytes(JsonObjectToString(Manifest) + TEXT("\n")), TEXT("manifest"));

		if (PackagePath.TrimStartAndEnd().IsEmpty())
		{
			PackagePath = FPaths::Combine(GetPackageOutputRoot(), FString::Printf(TEXT("%s-%s.zip"), *SanitizePackageSegment(ToolName), *SanitizePackageSegment(Version)));
		}
		else if (!ResolveProjectPathInsideProject(PackagePath, PackagePath, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		PackagePath = FPaths::ConvertRelativePathToFull(PackagePath);

		TArray<TSharedPtr<FJsonValue>> EntryValues;
		for (const FToolPackageEntry& Entry : Entries)
		{
			EntryValues.Add(MakeShared<FJsonValueObject>(MakeEntryManifestObject(Entry)));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("tools_export_package"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("version"), Version);
		StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
		StructuredContent->SetBoolField(TEXT("allowRegistryOnly"), bAllowRegistryOnly);
		StructuredContent->SetStringField(TEXT("kind"), PackageKind);
		StructuredContent->SetStringField(TEXT("packagePath"), PackagePath);
		StructuredContent->SetStringField(TEXT("scaffoldDir"), bPortableScaffold ? ScaffoldInfo.ScaffoldDir : FString());
		StructuredContent->SetBoolField(TEXT("scaffoldIncluded"), bPortableScaffold);
		StructuredContent->SetObjectField(TEXT("scaffoldGate"), GateObject);
		StructuredContent->SetObjectField(TEXT("manifestPreview"), Manifest);
		StructuredContent->SetArrayField(TEXT("entries"), EntryValues);
		StructuredContent->SetNumberField(TEXT("entryCount"), Entries.Num());
		if (!bPortableScaffold)
		{
			StructuredContent->SetStringField(TEXT("warning"), TEXT("Registry-only expert mode: this package does not contain portable handler implementation patches."));
		}

		if (bDryRun)
		{
			return MakeExecutionResult(FString::Printf(TEXT("Dry run: would export %s tool package for '%s' with %d entries."), *PackageKind, *ToolName, Entries.Num()), StructuredContent, false);
		}

		if (!WriteZipPackage(PackagePath, Entries, FailureReason))
		{
			return MakeExecutionResult(FailureReason, StructuredContent, true);
		}
		StructuredContent->SetObjectField(TEXT("packageFile"), MakeFileInfoObject(PackagePath));
		return MakeExecutionResult(FString::Printf(TEXT("Exported %s tool package for '%s' to %s."), *PackageKind, *ToolName, *PackagePath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ListExportableToolPackages(const FJsonObject& Arguments)
	{
		(void)Arguments;
		TArray<FExportableScaffoldInfo> ScaffoldInfos;
		FindExportableScaffoldInfos(ScaffoldInfos);
		ScaffoldInfos.Sort([](const FExportableScaffoldInfo& Left, const FExportableScaffoldInfo& Right)
		{
			if (Left.SourceKind != Right.SourceKind)
			{
				return Left.SourceKind < Right.SourceKind;
			}
			return Left.ToolName < Right.ToolName;
		});

		TArray<TSharedPtr<FJsonValue>> ItemValues;
		for (const FExportableScaffoldInfo& Info : ScaffoldInfos)
		{
			if (!Info.bHasMetadata)
			{
				continue;
			}
			ItemValues.Add(MakeShared<FJsonValueObject>(MakeScaffoldInfoObject(Info)));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("tools_list_exportable"));
		StructuredContent->SetArrayField(TEXT("items"), ItemValues);
		StructuredContent->SetNumberField(TEXT("itemCount"), ItemValues.Num());
		if (ItemValues.Num() > 0 && ItemValues[0].IsValid() && ItemValues[0]->Type == EJson::Object)
		{
			StructuredContent->SetObjectField(TEXT("firstItem"), ItemValues[0]->AsObject());
		}

		return MakeExecutionResult(FString::Printf(TEXT("Found %d scaffold-backed tools that can be exported."), ItemValues.Num()), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ImportToolPackage(const FJsonObject& Arguments)
	{
		FString RequestedPackagePath;
		bool bDryRun = true;
		bool bOverwriteScaffold = false;
		bool bAcceptRegistryOnly = false;
		Arguments.TryGetStringField(TEXT("packagePath"), RequestedPackagePath);
		Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
		Arguments.TryGetBoolField(TEXT("overwriteScaffold"), bOverwriteScaffold);
		Arguments.TryGetBoolField(TEXT("acceptRegistryOnly"), bAcceptRegistryOnly);

		FString PackagePath;
		FString FailureReason;
		if (!ResolvePackagePathForRead(RequestedPackagePath, PackagePath, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		if (!FPaths::FileExists(PackagePath))
		{
			return MakeExecutionResult(FString::Printf(TEXT("Package path was not found: %s"), *PackagePath), nullptr, true);
		}

		TMap<FString, TArray<uint8>> PackageEntries;
		if (!ReadZipPackage(PackagePath, PackageEntries, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		const TArray<uint8>* ManifestBytes = PackageEntries.Find(TEXT("manifest.json"));
		const TArray<uint8>* RegistryBytes = PackageEntries.Find(TEXT("registry/tool.json"));
		if (!ManifestBytes || !RegistryBytes)
		{
			return MakeExecutionResult(TEXT("Package must contain manifest.json and registry/tool.json."), nullptr, true);
		}
		TSharedPtr<FJsonObject> Manifest;
		TSharedPtr<FJsonObject> RegistryEntry;
		if (!LoadJsonObject(BytesToUtf8String(*ManifestBytes), Manifest) || !Manifest.IsValid())
		{
			return MakeExecutionResult(TEXT("manifest.json is not valid JSON."), nullptr, true);
		}
		if (!LoadJsonObject(BytesToUtf8String(*RegistryBytes), RegistryEntry) || !RegistryEntry.IsValid())
		{
			return MakeExecutionResult(TEXT("registry/tool.json is not valid JSON."), nullptr, true);
		}

		TArray<TSharedPtr<FJsonValue>> ValidatedFileValues;
		if (!ValidateManifestHashes(Manifest, PackageEntries, ValidatedFileValues, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		FString ToolName;
		Manifest->TryGetStringField(TEXT("toolName"), ToolName);
		FString PackageKind = ToolPackageKindFull();
		Manifest->TryGetStringField(TEXT("kind"), PackageKind);
		PackageKind = PackageKind.TrimStartAndEnd().IsEmpty() ? ToolPackageKindFull() : PackageKind.TrimStartAndEnd();
		FString RegistryToolName;
		RegistryEntry->TryGetStringField(TEXT("name"), RegistryToolName);
		if (ToolName.IsEmpty() || !ToolName.Equals(RegistryToolName, ESearchCase::CaseSensitive))
		{
			return MakeExecutionResult(TEXT("Package manifest toolName must match registry/tool.json name."), nullptr, true);
		}

		TArray<FString> RegistryRootCandidates;
		FString RegistryRoot;
		ResolveSharedRepoRoot(TEXT("UnrealMcpToolRegistry"), { TEXT("tools.json") }, RegistryRoot, RegistryRootCandidates);
		const FString RegistryPath = FPaths::Combine(RegistryRoot, TEXT("tools.json"));

		if (!FPaths::FileExists(RegistryPath))
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("tools_import_package"));
			StructuredContent->SetStringField(TEXT("packagePath"), PackagePath);
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("kind"), PackageKind);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("acceptRegistryOnly"), bAcceptRegistryOnly);
			StructuredContent->SetStringField(TEXT("registryPath"), RegistryPath);
			StructuredContent->SetStringField(TEXT("errorCode"), TEXT("REGISTRY_NOT_INITIALIZED"));
			StructuredContent->SetArrayField(TEXT("registryRootCandidates"), MakeSharedRepoRootCandidateValues(RegistryRootCandidates, { TEXT("tools.json") }));
			return MakeExecutionResult(MakeRegistryNotInitializedMessage(), StructuredContent, true);
		}

		TSharedPtr<FJsonObject> RegistryObject;
		bool bDuplicateRegistryEntry = false;
		if (!LoadRegistryForAppend(RegistryPath, RegistryObject, RegistryEntry, bDuplicateRegistryEntry, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		FString ScaffoldRoot;
		if (!ResolveProjectPathInsideProject(TEXT("Tools/UnrealMcpToolScaffolds"), ScaffoldRoot, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		TArray<FString> TestRootCandidates;
		FString TestsRoot;
		ResolveSharedRepoRoot(TEXT("UnrealMcpTests"), { TEXT("*.json") }, TestsRoot, TestRootCandidates);

		TArray<TSharedPtr<FJsonValue>> PlanValues;
		auto AddPlan = [&PlanValues](const FString& Kind, const FString& Source, const FString& Target, bool bWouldWrite, bool bExists)
		{
			TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
			Plan->SetStringField(TEXT("kind"), Kind);
			Plan->SetStringField(TEXT("source"), Source);
			Plan->SetStringField(TEXT("target"), Target);
			Plan->SetBoolField(TEXT("wouldWrite"), bWouldWrite);
			Plan->SetBoolField(TEXT("exists"), bExists);
			PlanValues.Add(MakeShared<FJsonValueObject>(Plan));
		};

		AddPlan(TEXT("registry"), TEXT("registry/tool.json"), RegistryPath, !bDuplicateRegistryEntry, FPaths::FileExists(RegistryPath));
		for (const TPair<FString, TArray<uint8>>& EntryPair : PackageEntries)
		{
			if (EntryPair.Key.StartsWith(TEXT("scaffold/")))
			{
				const FString Relative = EntryPair.Key.RightChop(9);
				const FString Target = FPaths::ConvertRelativePathToFull(FPaths::Combine(ScaffoldRoot, SanitizeMcpToolIdForPath(ToolName), Relative));
				AddPlan(TEXT("scaffold"), EntryPair.Key, Target, bOverwriteScaffold || !FPaths::FileExists(Target), FPaths::FileExists(Target));
			}
			else if (EntryPair.Key.StartsWith(TEXT("tests/")))
			{
				const FString Relative = EntryPair.Key.RightChop(6);
				const FString Target = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestsRoot, Relative));
				AddPlan(TEXT("test"), EntryPair.Key, Target, !FPaths::FileExists(Target), FPaths::FileExists(Target));
			}
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("tools_import_package"));
		StructuredContent->SetStringField(TEXT("packagePath"), PackagePath);
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("kind"), PackageKind);
		StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
		StructuredContent->SetBoolField(TEXT("acceptRegistryOnly"), bAcceptRegistryOnly);
		StructuredContent->SetBoolField(TEXT("duplicateRegistryEntry"), bDuplicateRegistryEntry);
		StructuredContent->SetStringField(TEXT("registryPath"), RegistryPath);
		StructuredContent->SetStringField(TEXT("scaffoldRoot"), ScaffoldRoot);
		StructuredContent->SetStringField(TEXT("testsRoot"), TestsRoot);
		StructuredContent->SetObjectField(TEXT("manifest"), Manifest);
		StructuredContent->SetArrayField(TEXT("validatedFiles"), ValidatedFileValues);
		StructuredContent->SetArrayField(TEXT("importPlan"), PlanValues);
		StructuredContent->SetArrayField(TEXT("registryRootCandidates"), MakeSharedRepoRootCandidateValues(RegistryRootCandidates, { TEXT("tools.json") }));
		StructuredContent->SetArrayField(TEXT("testRootCandidates"), MakeSharedRepoRootCandidateValues(TestRootCandidates, { TEXT("*.json") }));

		const bool bRegistryOnlyPackage = PackageKind.Equals(ToolPackageKindRegistryOnly(), ESearchCase::IgnoreCase);
		const FString RegistryOnlyMessage = TEXT("Package is registry-only: it only contains a registry entry whose handler is not portable across machines. Re-run with acceptRegistryOnly=true only when the handler already exists locally.");
		if (bRegistryOnlyPackage && !bAcceptRegistryOnly)
		{
			StructuredContent->SetStringField(TEXT("warning"), RegistryOnlyMessage);
			StructuredContent->SetBoolField(TEXT("registryOnlyImportRefused"), !bDryRun);
			if (!bDryRun)
			{
				return MakeExecutionResult(RegistryOnlyMessage, StructuredContent, true);
			}
		}

		if (bDryRun)
		{
			return MakeExecutionResult(FString::Printf(TEXT("Dry run: validated package for '%s' with %d planned entries."), *ToolName, PlanValues.Num()), StructuredContent, false);
		}
		if (bDuplicateRegistryEntry)
		{
			return MakeExecutionResult(FString::Printf(TEXT("ToolRegistry already contains '%s'; refusing real import."), *ToolName), StructuredContent, true);
		}

		const TArray<TSharedPtr<FJsonValue>>* ExistingTools = nullptr;
		RegistryObject->TryGetArrayField(TEXT("tools"), ExistingTools);
		TArray<TSharedPtr<FJsonValue>> UpdatedTools = ExistingTools ? *ExistingTools : TArray<TSharedPtr<FJsonValue>>();
		UpdatedTools.Add(MakeShared<FJsonValueObject>(RegistryEntry));
		RegistryObject->SetArrayField(TEXT("tools"), UpdatedTools);
		if (!SaveJsonObjectToFile(RegistryObject, RegistryPath, FailureReason))
		{
			return MakeExecutionResult(FailureReason, StructuredContent, true);
		}

		for (const TPair<FString, TArray<uint8>>& EntryPair : PackageEntries)
		{
			if (EntryPair.Key.StartsWith(TEXT("scaffold/")))
			{
				const FString Target = FPaths::ConvertRelativePathToFull(FPaths::Combine(ScaffoldRoot, SanitizeMcpToolIdForPath(ToolName), EntryPair.Key.RightChop(9)));
				if (!WriteBytesToFile(Target, EntryPair.Value, bOverwriteScaffold, FailureReason))
				{
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
			}
			else if (EntryPair.Key.StartsWith(TEXT("tests/")))
			{
				const FString Target = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestsRoot, EntryPair.Key.RightChop(6)));
				if (!WriteBytesToFile(Target, EntryPair.Value, false, FailureReason))
				{
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
			}
		}

		return MakeExecutionResult(FString::Printf(TEXT("Imported tool package for '%s'."), *ToolName), StructuredContent, false);
	}
}
