#include "UnrealMcpPythonToolBridge.h"

#include "UnrealMcpModule.h"
#include "UnrealMcpSharedPathResolver.h"
#include "UnrealMcpToolHandlerRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UnrealMcp
{
namespace UnrealMcpPythonToolBridge
{
	namespace
	{
		static const FString AllowedPythonHandlerPrefix = TEXT("Tools/UnrealMcpPyTools/");
		static const FString AllowedPythonHandlerSuffix = TEXT("/main.py");
		static const FString ResultBeginSentinel = TEXT("UNREAL_MCP_PY_RESULT_BEGIN");
		static const FString ResultEndSentinel = TEXT("UNREAL_MCP_PY_RESULT_END");

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

		FString NormalizePythonHandlerPath(const FString& HandlerPath)
		{
			FString Normalized = HandlerPath.TrimStartAndEnd();
			FPaths::NormalizeFilename(Normalized);
			while (Normalized.StartsWith(TEXT("./")))
			{
				Normalized.RightChopInline(2);
			}
			return Normalized;
		}

		bool HasUnsafePathSegment(const FString& NormalizedPath)
		{
			TArray<FString> Segments;
			NormalizedPath.ParseIntoArray(Segments, TEXT("/"), true);
			for (const FString& Segment : Segments)
			{
				if (Segment == TEXT(".") || Segment == TEXT(".."))
				{
					return true;
				}
			}
			return false;
		}

		bool ValidatePythonHandlerPath(const FString& HandlerPath, FString& OutNormalizedPath, FString& OutFailureReason)
		{
			OutNormalizedPath = NormalizePythonHandlerPath(HandlerPath);
			if (!OutNormalizedPath.StartsWith(AllowedPythonHandlerPrefix, ESearchCase::CaseSensitive)
				|| !OutNormalizedPath.EndsWith(AllowedPythonHandlerSuffix, ESearchCase::CaseSensitive))
			{
				OutFailureReason = FString::Printf(
					TEXT("Python handler path '%s' is outside the allowed Tools/UnrealMcpPyTools/*/main.py layout."),
					*OutNormalizedPath);
				return false;
			}

			if (HasUnsafePathSegment(OutNormalizedPath))
			{
				OutFailureReason = FString::Printf(TEXT("Python handler path '%s' contains unsafe relative path segments."), *OutNormalizedPath);
				return false;
			}

			return true;
		}

		FString PreviewString(const FString& Text, int32 MaxCharacters = 4000)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			if (Trimmed.Len() <= MaxCharacters)
			{
				return Trimmed;
			}
			return Trimmed.Left(MaxCharacters) + TEXT("...");
		}

		TSharedPtr<FJsonObject> MakeBridgeMetadataObject(
			const FToolHandlerRegistryEntry& HandlerEntry,
			const FString& Action,
			const FString& Reason,
			const FString& AbsoluteHandlerPath,
			const FString& ActualSha256,
			const FToolsReadResolution* HandlerResolution = nullptr)
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), Action);
			StructuredContent->SetStringField(TEXT("handlerName"), HandlerEntry.HandlerName);
			StructuredContent->SetStringField(TEXT("pythonHandlerPath"), HandlerEntry.PythonHandlerPath);
			StructuredContent->SetStringField(TEXT("pythonHandlerAbsolutePath"), AbsoluteHandlerPath);
			if (HandlerResolution)
			{
				StructuredContent->SetBoolField(TEXT("pythonHandlerFound"), HandlerResolution->bFound);
				StructuredContent->SetStringField(TEXT("pythonHandlerSourceKind"), LexToString(HandlerResolution->SourceKind));
				StructuredContent->SetArrayField(TEXT("pythonHandlerCandidates"), MakeToolsReadCandidateValues(*HandlerResolution));
				if (!HandlerResolution->Warning.IsEmpty())
				{
					StructuredContent->SetStringField(TEXT("pythonHandlerResolutionWarning"), HandlerResolution->Warning);
				}
			}
			StructuredContent->SetStringField(TEXT("pythonExpectedSha256"), HandlerEntry.PythonHandlerSha256);
			StructuredContent->SetStringField(TEXT("pythonActualSha256"), ActualSha256);
			StructuredContent->SetNumberField(TEXT("pythonImportAllowListSize"), HandlerEntry.PythonImportAllowList.Num());
			StructuredContent->SetStringField(TEXT("reason"), Reason);
			return StructuredContent;
		}

		void AddBridgeMetadata(
			const TSharedPtr<FJsonObject>& StructuredContent,
			const FToolHandlerRegistryEntry& HandlerEntry,
			const FString& AbsoluteHandlerPath,
			const FString& ActualSha256,
			const FToolsReadResolution& HandlerResolution,
			bool bPythonExecutionSucceeded)
		{
			if (!StructuredContent.IsValid())
			{
				return;
			}

			StructuredContent->SetStringField(TEXT("handlerName"), HandlerEntry.HandlerName);
			StructuredContent->SetStringField(TEXT("pythonHandlerPath"), HandlerEntry.PythonHandlerPath);
			StructuredContent->SetStringField(TEXT("pythonHandlerAbsolutePath"), AbsoluteHandlerPath);
			StructuredContent->SetBoolField(TEXT("pythonHandlerFound"), HandlerResolution.bFound);
			StructuredContent->SetStringField(TEXT("pythonHandlerSourceKind"), LexToString(HandlerResolution.SourceKind));
			StructuredContent->SetArrayField(TEXT("pythonHandlerCandidates"), MakeToolsReadCandidateValues(HandlerResolution));
			if (!HandlerResolution.Warning.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("pythonHandlerResolutionWarning"), HandlerResolution.Warning);
			}
			StructuredContent->SetStringField(TEXT("pythonExpectedSha256"), HandlerEntry.PythonHandlerSha256);
			StructuredContent->SetStringField(TEXT("pythonActualSha256"), ActualSha256);
			StructuredContent->SetNumberField(TEXT("pythonImportAllowListSize"), HandlerEntry.PythonImportAllowList.Num());
			StructuredContent->SetBoolField(TEXT("pythonExecutionSucceeded"), bPythonExecutionSucceeded);
		}

		FUnrealMcpExecutionResult MakeBridgeResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError)
		{
			FUnrealMcpExecutionResult Result;
			Result.Text = Text;
			Result.StructuredContent = StructuredContent;
			Result.bIsError = bIsError;
			return Result;
		}

		FUnrealMcpExecutionResult MakeBridgeError(
			const FToolHandlerRegistryEntry& HandlerEntry,
			const FString& Reason,
			const FString& AbsoluteHandlerPath = FString(),
			const FString& ActualSha256 = FString(),
			const FToolsReadResolution* HandlerResolution = nullptr)
		{
			return MakeBridgeResult(
				Reason,
				MakeBridgeMetadataObject(HandlerEntry, TEXT("python_registered_tool_bridge_failed"), Reason, AbsoluteHandlerPath, ActualSha256, HandlerResolution),
				true);
		}

		bool TrySerializeArguments(const FJsonObject& Arguments, FString& OutJson)
		{
			TSharedPtr<FJsonObject> ArgumentObject = MakeShared<FJsonObject>();
			ArgumentObject->Values = Arguments.Values;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
			return FJsonSerializer::Serialize(ArgumentObject.ToSharedRef(), Writer);
		}

		FString MakePythonStringLiteral(const FString& Value)
		{
			FString Result = TEXT("\"");
			for (TCHAR Character : Value)
			{
				switch (Character)
				{
				case TEXT('\\'):
					Result += TEXT("\\\\");
					break;
				case TEXT('"'):
					Result += TEXT("\\\"");
					break;
				case TEXT('\n'):
					Result += TEXT("\\n");
					break;
				case TEXT('\r'):
					Result += TEXT("\\r");
					break;
				case TEXT('\t'):
					Result += TEXT("\\t");
					break;
				default:
					if (Character < 0x20)
					{
						Result += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
					}
					else
					{
						Result.AppendChar(Character);
					}
					break;
				}
			}
			Result += TEXT("\"");
			return Result;
		}

		FString MakePythonStringArrayLiteral(const TArray<FString>& Values)
		{
			FString Result = TEXT("[");
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0)
				{
					Result += TEXT(", ");
				}
				Result += MakePythonStringLiteral(Values[Index]);
			}
			Result += TEXT("]");
			return Result;
		}

		bool StringContainsDeniedRecursionTool(const FString& Value)
		{
			return Value.Contains(TEXT("execute_python"), ESearchCase::IgnoreCase)
				|| Value.Contains(TEXT("execute_python_file"), ESearchCase::IgnoreCase);
		}

		bool TryFindDeniedRecursionString(const TSharedPtr<FJsonValue>& Value, const FString& JsonPath, FString& OutMatchedPath, FString& OutMatchedValue);

		bool TryFindDeniedRecursionStringInObject(const FJsonObject& Object, const FString& JsonPath, FString& OutMatchedPath, FString& OutMatchedValue)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
			{
				const FString ChildPath = JsonPath.IsEmpty() ? Pair.Key : FString::Printf(TEXT("%s.%s"), *JsonPath, *Pair.Key);
				if (TryFindDeniedRecursionString(Pair.Value, ChildPath, OutMatchedPath, OutMatchedValue))
				{
					return true;
				}
			}
			return false;
		}

		bool TryFindDeniedRecursionString(const TSharedPtr<FJsonValue>& Value, const FString& JsonPath, FString& OutMatchedPath, FString& OutMatchedValue)
		{
			if (!Value.IsValid())
			{
				return false;
			}

			switch (Value->Type)
			{
			case EJson::String:
			{
				FString StringValue;
				if (Value->TryGetString(StringValue) && StringContainsDeniedRecursionTool(StringValue))
				{
					OutMatchedPath = JsonPath;
					OutMatchedValue = PreviewString(StringValue, 500);
					return true;
				}
				break;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> ObjectValue = Value->AsObject();
				if (ObjectValue.IsValid() && TryFindDeniedRecursionStringInObject(*ObjectValue, JsonPath, OutMatchedPath, OutMatchedValue))
				{
					return true;
				}
				break;
			}
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& ArrayValue = Value->AsArray();
				for (int32 Index = 0; Index < ArrayValue.Num(); ++Index)
				{
					const FString ChildPath = FString::Printf(TEXT("%s[%d]"), *JsonPath, Index);
					if (TryFindDeniedRecursionString(ArrayValue[Index], ChildPath, OutMatchedPath, OutMatchedValue))
					{
						return true;
					}
				}
				break;
			}
			default:
				break;
			}

			return false;
		}

		IPythonScriptPlugin* LoadPythonScriptPlugin()
		{
			static const FName PythonScriptPluginModuleName(TEXT("PythonScriptPlugin"));
			if (IPythonScriptPlugin* PythonPlugin = FModuleManager::GetModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName))
			{
				return PythonPlugin;
			}

			return FModuleManager::LoadModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName);
		}

		FString BuildPythonWrapperCommand(const FString& ParentDirectory, const FString& ArgumentsJson, const TArray<FString>& AllowList)
		{
			return FString::Printf(
				TEXT("import builtins, importlib, json, sys, traceback\n")
				TEXT("_unreal_mcp_parent = %s\n")
				TEXT("_unreal_mcp_args_json = %s\n")
				TEXT("_unreal_mcp_user_allow_list = %s\n")
				TEXT("_unreal_mcp_original_import = builtins.__import__\n")
				TEXT("_unreal_mcp_original_import_module = importlib.import_module\n")
				TEXT("_unreal_mcp_stdlib_seed = {\n")
				TEXT("    'abc', 'argparse', 'array', 'base64', 'binascii', 'bisect', 'codecs', 'collections',\n")
				TEXT("    'contextlib', 'copy', 'copyreg', 'dataclasses', 'datetime', 'encodings', 'enum',\n")
				TEXT("    'errno', 'functools', 'gc', 'genericpath', 'hashlib', 'importlib', 'inspect',\n")
				TEXT("    'io', 'itertools', 'json', 'linecache', 'locale', 'math', 'ntpath', 'operator',\n")
				TEXT("    'os', 'pathlib', 'platform', 'posixpath', 're', 'reprlib', 'signal', 'site',\n")
				TEXT("    'stat', 'string', 'sys', 'textwrap', 'threading', 'time', 'token', 'tokenize',\n")
				TEXT("    'traceback', 'types', 'typing', 'unicodedata', 'warnings', 'weakref',\n")
				TEXT("    '_collections_abc', '_weakrefset'\n")
				TEXT("}\n")
				TEXT("_unreal_mcp_allowed_roots = set(str(_name).split('.', 1)[0] for _name in sys.builtin_module_names)\n")
				TEXT("_unreal_mcp_stdlib_names = getattr(sys, 'stdlib_module_names', _unreal_mcp_stdlib_seed)\n")
				TEXT("_unreal_mcp_allowed_roots.update(str(_name).split('.', 1)[0] for _name in _unreal_mcp_stdlib_names)\n")
				TEXT("_unreal_mcp_allowed_roots.discard('subprocess')\n")
				TEXT("_unreal_mcp_allowed_roots.add('unreal')\n")
				TEXT("_unreal_mcp_allowed_roots.update(str(_name).strip().split('.', 1)[0] for _name in _unreal_mcp_user_allow_list if str(_name).strip())\n")
				TEXT("def _unreal_mcp_import_root(_name):\n")
				TEXT("    return str(_name or '').split('.', 1)[0]\n")
				TEXT("def _unreal_mcp_check_import(_name):\n")
				TEXT("    _root = _unreal_mcp_import_root(_name)\n")
				TEXT("    if not _root or _root in _unreal_mcp_allowed_roots:\n")
				TEXT("        return\n")
				TEXT("    raise ImportError(\"Unreal MCP Python import denied: module '\" + str(_name) + \"' (root '\" + _root + \"') is not in the implicit stdlib/unreal set or pythonImportAllowList. Add '\" + _root + \"' to pythonImportAllowList for this tool if it is required.\")\n")
				TEXT("def _unreal_mcp_import_hook(name, globals=None, locals=None, fromlist=(), level=0):\n")
				TEXT("    if level == 0:\n")
				TEXT("        _unreal_mcp_check_import(name)\n")
				TEXT("    return _unreal_mcp_original_import(name, globals, locals, fromlist, level)\n")
				TEXT("def _unreal_mcp_import_module_hook(_name, _package=None):\n")
				TEXT("    if not str(_name or '').startswith('.'):\n")
				TEXT("        _unreal_mcp_check_import(_name)\n")
				TEXT("    return _unreal_mcp_original_import_module(_name, _package)\n")
				TEXT("if _unreal_mcp_parent not in sys.path:\n")
				TEXT("    sys.path.insert(0, _unreal_mcp_parent)\n")
				TEXT("builtins.__import__ = _unreal_mcp_import_hook\n")
				TEXT("importlib.import_module = _unreal_mcp_import_module_hook\n")
				TEXT("try:\n")
				TEXT("    sys.modules.pop('main', None)\n")
				TEXT("    main = _unreal_mcp_original_import_module('main')\n")
				TEXT("    _unreal_mcp_args = json.loads(_unreal_mcp_args_json)\n")
				TEXT("    _unreal_mcp_result = main.execute(_unreal_mcp_args)\n")
				TEXT("except BaseException as exc:\n")
				TEXT("    _unreal_mcp_result = {'isError': True, 'error': str(exc), 'traceback': traceback.format_exc()}\n")
				TEXT("finally:\n")
				TEXT("    builtins.__import__ = _unreal_mcp_original_import\n")
				TEXT("    importlib.import_module = _unreal_mcp_original_import_module\n")
				TEXT("print('%s ' + json.dumps(_unreal_mcp_result, ensure_ascii=False, default=str) + ' %s')\n"),
				*MakePythonStringLiteral(ParentDirectory),
				*MakePythonStringLiteral(ArgumentsJson),
				*MakePythonStringArrayLiteral(AllowList),
				*ResultBeginSentinel,
				*ResultEndSentinel);
		}

		void AppendLine(FString& Target, const FString& Line)
		{
			if (Line.IsEmpty())
			{
				return;
			}
			if (!Target.IsEmpty())
			{
				Target += TEXT("\n");
			}
			Target += Line;
		}

		void CollectPythonCommandOutput(const FPythonCommandEx& PythonCommand, FString& OutStdout, FString& OutStderr)
		{
			AppendLine(OutStdout, PythonCommand.CommandResult);
			for (const FPythonLogOutputEntry& LogEntry : PythonCommand.LogOutput)
			{
				// Renamed from `LogType` to avoid C4459 (shadows UE global `LogType`
				// from UObject/UnrealType.h) on UE 5.6 Windows MSVC where the warning
				// is treated as an error. macOS clang does not flag this. See issue #2.
				const FString LogEntryType = LexToString(LogEntry.Type);
				if (LogEntryType.Contains(TEXT("Error"), ESearchCase::IgnoreCase))
				{
					AppendLine(OutStderr, LogEntry.Output);
				}
				else
				{
					AppendLine(OutStdout, LogEntry.Output);
				}
			}
		}

		FString BuildCombinedPythonOutput(const FString& Stdout, const FString& Stderr)
		{
			FString Combined;
			AppendLine(Combined, Stdout);
			AppendLine(Combined, Stderr);
			return Combined;
		}

		bool TryExtractSentinelJson(const FString& CapturedOutput, FString& OutResultJson)
		{
			const int32 BeginIndex = CapturedOutput.Find(ResultBeginSentinel, ESearchCase::CaseSensitive);
			if (BeginIndex == INDEX_NONE)
			{
				return false;
			}

			const int32 JsonStartIndex = BeginIndex + ResultBeginSentinel.Len();
			const int32 EndIndex = CapturedOutput.Find(ResultEndSentinel, ESearchCase::CaseSensitive, ESearchDir::FromStart, JsonStartIndex);
			if (EndIndex == INDEX_NONE || EndIndex <= JsonStartIndex)
			{
				return false;
			}

			OutResultJson = CapturedOutput.Mid(JsonStartIndex, EndIndex - JsonStartIndex).TrimStartAndEnd();
			return !OutResultJson.IsEmpty();
		}
	}

	FUnrealMcpExecutionResult ExecutePythonRegisteredTool(const FToolHandlerRegistryEntry& HandlerEntry, const FJsonObject& Arguments)
	{
		FString NormalizedHandlerPath;
		FString FailureReason;
		if (!ValidatePythonHandlerPath(HandlerEntry.PythonHandlerPath, NormalizedHandlerPath, FailureReason))
		{
			return MakeBridgeError(HandlerEntry, FailureReason);
		}

		const FToolsReadResolution HandlerResolution = ResolveToolsReadSubpath(
			NormalizedHandlerPath,
			{ TEXT("main.py") });
		const FString AbsoluteHandlerPath = HandlerResolution.Path;
		if (!HandlerResolution.bFound)
		{
			return MakeBridgeError(
				HandlerEntry,
				FString::Printf(TEXT("Python handler file does not exist: '%s'."), *AbsoluteHandlerPath),
				AbsoluteHandlerPath,
				FString(),
				&HandlerResolution);
		}

		TArray<uint8> HandlerBytes;
		if (!FFileHelper::LoadFileToArray(HandlerBytes, *AbsoluteHandlerPath))
		{
			return MakeBridgeError(
				HandlerEntry,
				FString::Printf(TEXT("Failed to read Python handler file: '%s'."), *AbsoluteHandlerPath),
				AbsoluteHandlerPath,
				FString(),
				&HandlerResolution);
		}

		const FString ActualSha256 = Sha256Bytes(HandlerBytes).ToLower();
		const FString ExpectedSha256 = HandlerEntry.PythonHandlerSha256.TrimStartAndEnd().ToLower();
		if (!ActualSha256.Equals(ExpectedSha256, ESearchCase::CaseSensitive))
		{
			return MakeBridgeError(
				HandlerEntry,
				FString::Printf(
					TEXT("Python handler sha256 mismatch for '%s'. expected=%s actual=%s."),
					*NormalizedHandlerPath,
					*ExpectedSha256,
					*ActualSha256),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		FString MatchedRecursionPath;
		FString MatchedRecursionValue;
		if (TryFindDeniedRecursionStringInObject(Arguments, TEXT("arguments"), MatchedRecursionPath, MatchedRecursionValue))
		{
			return MakeBridgeError(
				HandlerEntry,
				FString::Printf(
					TEXT("Recursion denied: Python registered tools may not request execute_python or execute_python_file (matched %s='%s')."),
					*MatchedRecursionPath,
					*MatchedRecursionValue),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		FString ArgumentsJson;
		if (!TrySerializeArguments(Arguments, ArgumentsJson))
		{
			return MakeBridgeError(
				HandlerEntry,
				TEXT("Failed to serialize Python registered tool arguments as JSON."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		IPythonScriptPlugin* PythonPlugin = LoadPythonScriptPlugin();
		if (!PythonPlugin)
		{
			return MakeBridgeError(
				HandlerEntry,
				TEXT("PythonScriptPlugin is not loaded. Enable the Python Script Plugin for the editor and restart Unreal Editor."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		if (!PythonPlugin->IsPythonInitialized())
		{
			PythonPlugin->ForceEnablePythonAtRuntime();
		}

		if (!PythonPlugin->IsPythonAvailable())
		{
			return MakeBridgeError(
				HandlerEntry,
				TEXT("Python support is not available in the current editor session."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		if (!PythonPlugin->IsPythonInitialized())
		{
			return MakeBridgeError(
				HandlerEntry,
				TEXT("Python is not initialized after requesting runtime enablement."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
		}

		FPythonCommandEx PythonCommand;
		PythonCommand.Command = BuildPythonWrapperCommand(FPaths::GetPath(AbsoluteHandlerPath), ArgumentsJson, HandlerEntry.PythonImportAllowList);
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;
		PythonCommand.Flags = EPythonCommandFlags::Unattended;

		const bool bExecuted = PythonPlugin->ExecPythonCommandEx(PythonCommand);

		FString Stdout;
		FString Stderr;
		CollectPythonCommandOutput(PythonCommand, Stdout, Stderr);
		const FString CapturedOutput = BuildCombinedPythonOutput(Stdout, Stderr);

		FString ResultJson;
		if (!TryExtractSentinelJson(CapturedOutput, ResultJson))
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeBridgeMetadataObject(
				HandlerEntry,
				TEXT("python_registered_tool_bridge_missing_sentinel"),
				TEXT("Python registered tool output did not contain the required result sentinel."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
			StructuredContent->SetBoolField(TEXT("pythonExecutionSucceeded"), bExecuted);
			StructuredContent->SetStringField(TEXT("stdoutPreview"), PreviewString(Stdout));
			StructuredContent->SetStringField(TEXT("stderrPreview"), PreviewString(Stderr));
			return MakeBridgeResult(
				TEXT("Python registered tool output did not contain UNREAL_MCP_PY_RESULT_BEGIN/END; see stdoutPreview/stderrPreview."),
				StructuredContent,
				true);
		}

		TSharedPtr<FJsonObject> ParsedResult;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
		if (!FJsonSerializer::Deserialize(Reader, ParsedResult) || !ParsedResult.IsValid())
		{
			TSharedPtr<FJsonObject> StructuredContent = MakeBridgeMetadataObject(
				HandlerEntry,
				TEXT("python_registered_tool_bridge_parse_failed"),
				TEXT("Python registered tool sentinel payload was not a JSON object."),
				AbsoluteHandlerPath,
				ActualSha256,
				&HandlerResolution);
			StructuredContent->SetStringField(TEXT("resultJsonPreview"), PreviewString(ResultJson));
			StructuredContent->SetStringField(TEXT("stdoutPreview"), PreviewString(Stdout));
			StructuredContent->SetStringField(TEXT("stderrPreview"), PreviewString(Stderr));
			return MakeBridgeResult(TEXT("Python registered tool returned malformed JSON in the result sentinel."), StructuredContent, true);
		}

		bool bPythonReturnedError = false;
		ParsedResult->TryGetBoolField(TEXT("isError"), bPythonReturnedError);
		AddBridgeMetadata(ParsedResult, HandlerEntry, AbsoluteHandlerPath, ActualSha256, HandlerResolution, bExecuted);

		FString Text = FString::Printf(
			TEXT("Python registered tool '%s' %s."),
			*HandlerEntry.HandlerName,
			(bExecuted && !bPythonReturnedError) ? TEXT("completed") : TEXT("failed"));
		if (bPythonReturnedError)
		{
			FString PythonError;
			if (ParsedResult->TryGetStringField(TEXT("error"), PythonError) && !PythonError.IsEmpty())
			{
				Text += TEXT(" ") + PreviewString(PythonError, 500);
			}
		}

		return MakeBridgeResult(Text, ParsedResult, bPythonReturnedError || !bExecuted);
	}
}
}
