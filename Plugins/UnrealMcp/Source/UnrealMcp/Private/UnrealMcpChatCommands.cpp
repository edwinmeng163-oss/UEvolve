#include "UnrealMcpModule.h"

namespace UnrealMcp
{
	FString GetCommandRemainder(const FString& Input, const FString& Command);
	bool MatchesCommand(const FString& Input, const FString& Command);
	TSharedPtr<FJsonObject> MakeEmptyObject();
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	TSharedPtr<FJsonObject> MakeExecutionStructuredMessage(const FString& Text);
	FUnrealMcpExecutionResult MakeExecutionResult(
		const FString& Text,
		const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
		bool bIsError = false);
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteChatCommand(const FString& Input) const
{
	const FString TrimmedInput = Input.TrimStartAndEnd();
	if (TrimmedInput.IsEmpty())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("Enter a command. Try /help."), nullptr, true);
	}

	if (TrimmedInput.StartsWith(TEXT("/tool "), ESearchCase::IgnoreCase))
	{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/tool"));
		if (Remainder.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /tool <tool-name> <json-args>"), nullptr, true);
		}

		FString ToolName = Remainder;
		FString JsonText;
		int32 SeparatorIndex = INDEX_NONE;
		if (Remainder.FindChar(TEXT(' '), SeparatorIndex))
		{
			ToolName = Remainder.Left(SeparatorIndex).TrimStartAndEnd();
			JsonText = Remainder.Mid(SeparatorIndex + 1).TrimStartAndEnd();
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		if (!JsonText.IsEmpty() && !UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /tool."), nullptr, true);
		}

		return ExecuteTool(ToolName, *ArgumentsObject);
	}

		if (TrimmedInput.Equals(TEXT("/help"), ESearchCase::IgnoreCase))
		{
				return UnrealMcp::MakeExecutionResult(
					TEXT("Commands:\n")
					TEXT("/help\n")
					TEXT("/ask <prompt>  (handled by the chat panel AI)\n")
					TEXT("/reset_ai  (handled by the chat panel AI)\n")
					TEXT("/stop_ai  (handled by the chat panel AI)\n")
					TEXT("Plain text in the chat panel is also treated as an AI ask.\n")
					TEXT("/status\n")
					TEXT("/pie [simulate]\n")
					TEXT("/stop_pie\n")
					TEXT("/console stat fps\n")
					TEXT("/log [lines]\n")
					TEXT("/map_check\n")
					TEXT("/maps\n")
					TEXT("/assets [/Game/Path]\n")
					TEXT("/selected assets\n")
					TEXT("/selected actors\n")
						TEXT("/select PlayerStart\n")
						TEXT("/move_selected 0 0 300 [pitch yaw roll]\n")
						TEXT("/set_props {\"selectedOnly\":true,\"properties\":{\"Tags\":[\"Encounter\"],\"RootComponent.RelativeScale3D\":{\"X\":1.25,\"Y\":1.25,\"Z\":1.25}}}\n")
						TEXT("/layout_selected 400 300 [columns] [startX startY startZ]\n")
						TEXT("/layout_circle 1200 [startAngle] [arcDegrees] [centerX centerY centerZ]\n")
						TEXT("/actors [filter]\n")
					TEXT("/open_map /Game/TopDown/Maps/Lvl_TopDown\n")
					TEXT("/open_asset /Game/Variant_TwinStick/Blueprints/BP_TwinStickCharacter\n")
						TEXT("/browse /Game/Variant_TwinStick\n")
						TEXT("/spawn /Script/Engine.PointLight 0 0 150 ChatLight\n")
						TEXT("/spawn_batch {\"classPath\":\"/Script/Engine.PointLight\",\"items\":[{\"x\":0,\"y\":0,\"z\":150,\"label\":\"Light_A\"},{\"x\":300,\"y\":0,\"z\":150,\"label\":\"Light_B\"}]}\n")
						TEXT("/py import unreal; print(unreal.EditorLevelLibrary.get_selected_level_actors())\n")
						TEXT("/py_eval unreal.EditorLevelLibrary.get_editor_world().get_name()\n")
						TEXT("/py_file Tools/mcp_test_script.py\n")
						TEXT("/compile_bp /Game/Blueprints/BP_Test\n")
						TEXT("/compile_bps /Game/TopDown\n")
						TEXT("/new_bp /Game/Blueprints/BP_Test /Script/Engine.Actor\n")
						TEXT("/delete_selected\n")
						TEXT("/save\n")
						TEXT("/tool unreal.spawn_actor {\"classPath\":\"/Script/Engine.PointLight\",\"x\":0,\"y\":0,\"z\":150,\"label\":\"ChatLight\"}\n")
						TEXT("/tool unreal.tail_log {\"lines\":80,\"contains\":\"Error\"}\n")
						TEXT("/tool unreal.select_actors {\"paths\":[\"/Game/TopDown/Lvl_TopDown.Lvl_TopDown:PersistentLevel.PlayerStart_0\"]}\n")
						TEXT("/tool unreal.execute_python {\"command\":\"import unreal\\nprint(unreal.EditorLevelLibrary.get_editor_world())\"}"),
						UnrealMcp::MakeExecutionStructuredMessage(TEXT("help")),
						false);
				}

		if (TrimmedInput.Equals(TEXT("/status"), ESearchCase::IgnoreCase))
		{
			return ExecuteTool(TEXT("unreal.editor_status"), *UnrealMcp::MakeEmptyObject());
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/pie")))
		{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/pie"));
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetBoolField(TEXT("simulate"), Remainder.Equals(TEXT("simulate"), ESearchCase::IgnoreCase));
			return ExecuteTool(TEXT("unreal.start_pie"), *ArgumentsObject);
		}

		if (TrimmedInput.Equals(TEXT("/stop_pie"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/stop pie"), ESearchCase::IgnoreCase))
		{
			return ExecuteTool(TEXT("unreal.stop_pie"), *UnrealMcp::MakeEmptyObject());
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/console")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/console"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /console <command>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("target"), TEXT("auto"));
			return ExecuteTool(TEXT("unreal.execute_console_command"), *ArgumentsObject);
		}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/log")))
			{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/log"));
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			if (!Remainder.IsEmpty())
			{
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() > 0)
				{
					int32 ParsedLines = 0;
					if (LexTryParseString(ParsedLines, *Tokens[0]))
					{
						ArgumentsObject->SetNumberField(TEXT("lines"), FMath::Max(1, ParsedLines));
						if (Tokens.Num() > 1)
						{
							TArray<FString> FilterTokens;
							for (int32 Index = 1; Index < Tokens.Num(); ++Index)
							{
								FilterTokens.Add(Tokens[Index]);
							}
							ArgumentsObject->SetStringField(TEXT("contains"), FString::Join(FilterTokens, TEXT(" ")));
						}
					}
					else
					{
						ArgumentsObject->SetStringField(TEXT("contains"), Remainder);
					}
				}
			}

				return ExecuteTool(TEXT("unreal.tail_log"), *ArgumentsObject);
			}

			if (TrimmedInput.Equals(TEXT("/map_check"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/map check"), ESearchCase::IgnoreCase))
			{
				return ExecuteTool(TEXT("unreal.map_check"), *UnrealMcp::MakeEmptyObject());
			}

			if (TrimmedInput.Equals(TEXT("/maps"), ESearchCase::IgnoreCase))
			{
				return ExecuteTool(TEXT("unreal.list_maps"), *UnrealMcp::MakeEmptyObject());
			}

	if (TrimmedInput.Equals(TEXT("/selected assets"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/selected_assets"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.list_selected_assets"), *UnrealMcp::MakeEmptyObject());
	}

	if (TrimmedInput.Equals(TEXT("/selected actors"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/selected_actors"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.list_selected_actors"), *UnrealMcp::MakeEmptyObject());
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/assets")))
	{
		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/assets"));
		ArgumentsObject->SetStringField(TEXT("path"), Path.IsEmpty() ? TEXT("/Game") : Path);
		ArgumentsObject->SetBoolField(TEXT("recursive"), true);
		return ExecuteTool(TEXT("unreal.list_assets"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/actors")))
		{
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			const FString Filter = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/actors"));
			if (!Filter.IsEmpty())
		{
			ArgumentsObject->SetStringField(TEXT("filter"), Filter);
			}
			return ExecuteTool(TEXT("unreal.list_level_actors"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/select")))
		{
			const FString Filter = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/select"));
			if (Filter.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /select <filter>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("filter"), Filter);
			ArgumentsObject->SetBoolField(TEXT("clearSelection"), true);
			return ExecuteTool(TEXT("unreal.select_actors"), *ArgumentsObject);
		}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/move_selected")))
			{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/move_selected"));
			TArray<FString> Tokens;
			Remainder.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() < 3)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /move_selected <x> <y> <z> [pitch yaw roll]"), nullptr, true);
			}

			double NumericValues[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
			for (int32 Index = 0; Index < Tokens.Num() && Index < 6; ++Index)
			{
				if (!LexTryParseString(NumericValues[Index], *Tokens[Index]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /move_selected <x> <y> <z> [pitch yaw roll]"), nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetNumberField(TEXT("x"), NumericValues[0]);
			ArgumentsObject->SetNumberField(TEXT("y"), NumericValues[1]);
			ArgumentsObject->SetNumberField(TEXT("z"), NumericValues[2]);
			if (Tokens.Num() > 3)
			{
				ArgumentsObject->SetNumberField(TEXT("pitch"), NumericValues[3]);
			}
			if (Tokens.Num() > 4)
			{
				ArgumentsObject->SetNumberField(TEXT("yaw"), NumericValues[4]);
			}
			if (Tokens.Num() > 5)
			{
				ArgumentsObject->SetNumberField(TEXT("roll"), NumericValues[5]);
			}

				return ExecuteTool(TEXT("unreal.set_actor_transform"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/set_props")) || UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/setprops")))
			{
				const FString JsonText = UnrealMcp::GetCommandRemainder(
					TrimmedInput,
					TrimmedInput.StartsWith(TEXT("/setprops"), ESearchCase::IgnoreCase) ? TEXT("/setprops") : TEXT("/set_props"));
				if (JsonText.IsEmpty())
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /set_props {\"selectedOnly\":true,\"properties\":{\"Tags\":[\"Encounter\"]}}"), nullptr, true);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				if (!UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /set_props."), nullptr, true);
				}

				if (!ArgumentsObject->HasField(TEXT("selectedOnly"))
					&& !ArgumentsObject->HasField(TEXT("filter"))
					&& !ArgumentsObject->HasField(TEXT("classPath"))
					&& !ArgumentsObject->HasField(TEXT("paths")))
				{
					ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				}

				return ExecuteTool(TEXT("unreal.batch_set_actor_properties"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/layout_selected")))
			{
				const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/layout_selected"));
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() < 2)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_selected <spacingX> <spacingY> [columns] [startX startY startZ]"), nullptr, true);
				}

				double SpacingX = 0.0;
				double SpacingY = 0.0;
				if (!LexTryParseString(SpacingX, *Tokens[0]) || !LexTryParseString(SpacingY, *Tokens[1]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_selected <spacingX> <spacingY> [columns] [startX startY startZ]"), nullptr, true);
				}

				int32 Columns = 5;
				int32 StartTokenIndex = 2;
				if (Tokens.Num() >= 3)
				{
					if (!LexTryParseString(Columns, *Tokens[2]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("The optional columns value must be an integer."), nullptr, true);
					}
					StartTokenIndex = 3;
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				ArgumentsObject->SetNumberField(TEXT("spacingX"), SpacingX);
				ArgumentsObject->SetNumberField(TEXT("spacingY"), SpacingY);
				ArgumentsObject->SetNumberField(TEXT("columns"), FMath::Max(1, Columns));

				if (Tokens.Num() > StartTokenIndex)
				{
					if (Tokens.Num() - StartTokenIndex != 3)
					{
						return UnrealMcp::MakeExecutionResult(TEXT("If you provide a start position, include all three values: startX startY startZ."), nullptr, true);
					}

					double StartX = 0.0;
					double StartY = 0.0;
					double StartZ = 0.0;
					if (!LexTryParseString(StartX, *Tokens[StartTokenIndex])
						|| !LexTryParseString(StartY, *Tokens[StartTokenIndex + 1])
						|| !LexTryParseString(StartZ, *Tokens[StartTokenIndex + 2]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("The optional start position values must be numeric."), nullptr, true);
					}

					ArgumentsObject->SetNumberField(TEXT("startX"), StartX);
					ArgumentsObject->SetNumberField(TEXT("startY"), StartY);
					ArgumentsObject->SetNumberField(TEXT("startZ"), StartZ);
				}

				return ExecuteTool(TEXT("unreal.layout_actors_grid"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/layout_circle")))
			{
				const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/layout_circle"));
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() < 1)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_circle <radius> [startAngle] [arcDegrees] [centerX centerY centerZ]"), nullptr, true);
				}

				double Radius = 0.0;
				if (!LexTryParseString(Radius, *Tokens[0]) || Radius <= 0.0)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The radius value must be a positive number."), nullptr, true);
				}

				double StartAngle = 0.0;
				double ArcDegrees = 360.0;
				if (Tokens.Num() > 1 && !LexTryParseString(StartAngle, *Tokens[1]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The optional startAngle value must be numeric."), nullptr, true);
				}
				if (Tokens.Num() > 2 && !LexTryParseString(ArcDegrees, *Tokens[2]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The optional arcDegrees value must be numeric."), nullptr, true);
				}

				if (Tokens.Num() != 1 && Tokens.Num() != 2 && Tokens.Num() != 3 && Tokens.Num() != 6)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Provide either only radius, radius+angles, or radius+angles+centerX centerY centerZ."), nullptr, true);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				ArgumentsObject->SetNumberField(TEXT("radius"), Radius);
				ArgumentsObject->SetNumberField(TEXT("startAngleDegrees"), StartAngle);
				ArgumentsObject->SetNumberField(TEXT("arcDegrees"), ArcDegrees);

				if (Tokens.Num() == 6)
				{
					double CenterX = 0.0;
					double CenterY = 0.0;
					double CenterZ = 0.0;
					if (!LexTryParseString(CenterX, *Tokens[3])
						|| !LexTryParseString(CenterY, *Tokens[4])
						|| !LexTryParseString(CenterZ, *Tokens[5]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("centerX, centerY, and centerZ must be numeric."), nullptr, true);
					}

					ArgumentsObject->SetNumberField(TEXT("centerX"), CenterX);
					ArgumentsObject->SetNumberField(TEXT("centerY"), CenterY);
					ArgumentsObject->SetNumberField(TEXT("centerZ"), CenterZ);
				}

				return ExecuteTool(TEXT("unreal.layout_actors_circle"), *ArgumentsObject);
			}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/open_map")))
		{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/open_map"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /open_map /Game/Path/To/Map"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.open_map"), *ArgumentsObject);
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/open_asset")))
	{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/open_asset"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /open_asset /Game/Path/To/Asset"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.open_asset"), *ArgumentsObject);
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/browse")))
	{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/browse"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /browse /Game/Path/Or/Asset"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.sync_content_browser"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/compile_bp")))
		{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/compile_bp"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /compile_bp /Game/Path/To/Blueprint"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("path"), Path);
			return ExecuteTool(TEXT("unreal.compile_blueprint"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/compile_bps")))
		{
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/compile_bps"));
			ArgumentsObject->SetStringField(TEXT("path"), Path.IsEmpty() ? TEXT("/Game") : Path);
			ArgumentsObject->SetBoolField(TEXT("recursive"), true);
			return ExecuteTool(TEXT("unreal.compile_blueprints_in_path"), *ArgumentsObject);
		}

	if (TrimmedInput.Equals(TEXT("/delete_selected"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.destroy_selected_actors"), *UnrealMcp::MakeEmptyObject());
	}

	if (TrimmedInput.Equals(TEXT("/save"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.save_dirty_packages"), *UnrealMcp::MakeEmptyObject());
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/new_bp")))
	{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/new_bp"));
		TArray<FString> Tokens;
		Remainder.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 1)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /new_bp /Game/Blueprints/BP_Name [/Script/Engine.Actor]"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("assetPath"), Tokens[0]);
		ArgumentsObject->SetStringField(TEXT("parentClass"), Tokens.Num() >= 2 ? Tokens[1] : TEXT("/Script/Engine.Actor"));
		ArgumentsObject->SetBoolField(TEXT("openAfterCreate"), true);
		ArgumentsObject->SetBoolField(TEXT("compile"), true);
		return ExecuteTool(TEXT("unreal.create_blueprint_class"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/spawn")))
		{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/spawn"));
		TArray<FString> Tokens;
		Remainder.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 1)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /spawn <classPath> [x y z [pitch yaw roll]] [label]"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("classPath"), Tokens[0]);

		int32 TokenIndex = 1;
		double NumericValues[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
		int32 ParsedNumericCount = 0;
		for (; TokenIndex < Tokens.Num() && ParsedNumericCount < 6; ++TokenIndex)
		{
			double ParsedValue = 0.0;
			if (!LexTryParseString(ParsedValue, *Tokens[TokenIndex]))
			{
				break;
			}

			NumericValues[ParsedNumericCount++] = ParsedValue;
		}

		if (ParsedNumericCount > 0)
		{
			ArgumentsObject->SetNumberField(TEXT("x"), NumericValues[0]);
		}
		if (ParsedNumericCount > 1)
		{
			ArgumentsObject->SetNumberField(TEXT("y"), NumericValues[1]);
		}
		if (ParsedNumericCount > 2)
		{
			ArgumentsObject->SetNumberField(TEXT("z"), NumericValues[2]);
		}
		if (ParsedNumericCount > 3)
		{
			ArgumentsObject->SetNumberField(TEXT("pitch"), NumericValues[3]);
		}
		if (ParsedNumericCount > 4)
		{
			ArgumentsObject->SetNumberField(TEXT("yaw"), NumericValues[4]);
		}
		if (ParsedNumericCount > 5)
		{
			ArgumentsObject->SetNumberField(TEXT("roll"), NumericValues[5]);
		}

		if (TokenIndex < Tokens.Num())
		{
			TArray<FString> LabelTokens;
			for (int32 LabelIndex = TokenIndex; LabelIndex < Tokens.Num(); ++LabelIndex)
			{
				LabelTokens.Add(Tokens[LabelIndex]);
			}

			FString Label = FString::Join(LabelTokens, TEXT(" "));
			if (!Label.IsEmpty())
			{
				ArgumentsObject->SetStringField(TEXT("label"), Label);
			}
		}

			return ExecuteTool(TEXT("unreal.spawn_actor"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/spawn_batch")))
		{
			const FString JsonText = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/spawn_batch"));
			if (JsonText.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /spawn_batch {\"classPath\":\"/Script/Engine.PointLight\",\"items\":[{\"x\":0,\"y\":0,\"z\":150}]}"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			if (!UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /spawn_batch."), nullptr, true);
			}

			if (!ArgumentsObject->HasField(TEXT("selectSpawned")))
			{
				ArgumentsObject->SetBoolField(TEXT("selectSpawned"), true);
			}

			return ExecuteTool(TEXT("unreal.spawn_actor_batch"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py_eval")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py_eval"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py_eval <python-expression>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("mode"), TEXT("EvaluateStatement"));
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			return ExecuteTool(TEXT("unreal.execute_python"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py_file")))
		{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py_file"));
			TArray<FString> Tokens;
			Remainder.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() < 1)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py_file <scriptPath> [arg1 arg2 ...]"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("scriptPath"), Tokens[0]);
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			ArgumentsObject->SetBoolField(TEXT("allowOutsideProject"), false);

			if (Tokens.Num() > 1)
			{
				TArray<TSharedPtr<FJsonValue>> ArgsArray;
				for (int32 Index = 1; Index < Tokens.Num(); ++Index)
				{
					ArgsArray.Add(MakeShared<FJsonValueString>(Tokens[Index]));
				}
				ArgumentsObject->SetArrayField(TEXT("args"), ArgsArray);
			}

			return ExecuteTool(TEXT("unreal.execute_python_file"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py <python-code-or-file>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("mode"), TEXT("ExecuteFile"));
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			return ExecuteTool(TEXT("unreal.execute_python"), *ArgumentsObject);
		}

		return UnrealMcp::MakeExecutionResult(TEXT("Unknown command. Try /help."), nullptr, true);
	}
