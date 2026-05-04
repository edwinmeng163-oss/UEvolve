#include "UnrealMcpEditorTools.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorScriptingHelpers.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "IContentBrowserSingleton.h"
#include "IPythonScriptPlugin.h"
#include "Logging/MessageLog.h"
#include "Modules/ModuleManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "PlayInEditorDataTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UnrealMcpModule.h"
#include "UnrealMcpSettings.h"

namespace UnrealMcp
{
	FString NormalizeEndpointPath(const FString& EndpointPath);
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	TArray<FAssetData> GetSelectedAssets();
	TSharedPtr<FJsonObject> MakeAssetObject(const FAssetData& Asset);
	FString DescribeAsset(const FAssetData& Asset);

	namespace
	{
		static constexpr int32 EditorToolDefaultListLimit = 200;

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

		bool EditorToolIsEditorPlaying()
		{
			return GEditor
				&& (GEditor->PlayWorld != nullptr
					|| GEditor->bIsSimulatingInEditor
					|| GEditor->GetPlaySessionRequest().IsSet());
		}

		FUnrealMcpExecutionResult EditorToolMakePieBlockedResult(const FString& ToolName)
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

			if (EditorToolIsEditorPlaying())
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
			if (EditorToolIsEditorPlaying())
			{
				return EditorToolMakePieBlockedResult(ToolName);
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
			if (EditorToolIsEditorPlaying())
			{
				return EditorToolMakePieBlockedResult(ToolName);
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
			if (EditorToolIsEditorPlaying())
			{
				return EditorToolMakePieBlockedResult(ToolName);
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
