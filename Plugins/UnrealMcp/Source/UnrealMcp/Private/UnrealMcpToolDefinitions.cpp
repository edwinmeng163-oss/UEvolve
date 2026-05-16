#include "UnrealMcpModule.h"
#include "UnrealMcpToolRegistrar.h"

namespace UnrealMcp
{
	static constexpr int32 DefaultListLimit = 200;

	TSharedPtr<FJsonObject> MakeObjectSchema();
	TSharedPtr<FJsonObject> MakeStringProperty(const FString& Description, const FString& DefaultValue = FString());
	TSharedPtr<FJsonObject> MakeBoolProperty(const FString& Description, bool bDefaultValue);
	TSharedPtr<FJsonObject> MakeNumberProperty(const FString& Description, double DefaultValue);
	TSharedPtr<FJsonObject> MakeStringArrayProperty(const FString& Description);
	TSharedPtr<FJsonObject> MakeFlexibleObjectProperty(const FString& Description);
	TSharedPtr<FJsonObject> MakeObjectArrayProperty(const FString& Description);
	void AddActorQuerySchemaFields(
		const TSharedPtr<FJsonObject>& PropertiesObject,
		bool bIncludeClassPath = true,
		bool bIncludePaths = true,
		bool bIncludeSelectedOnly = true);
	TSharedPtr<FJsonObject> MakeSpawnActorBasicProperties(bool bIncludeClassPath);
	void AddToolDefinition(
		TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FString& Name,
		const FString& Title,
		const FString& Description,
		const TSharedPtr<FJsonObject>& InputSchema);
	FString GetHostBuildPlatformName();
}

void FUnrealMcpModule::AppendToolDefinitions(TArray<TSharedPtr<FJsonValue>>& ToolsArray) const
{
	UnrealMcp::AppendRegisteredToolDefinitions(ToolsArray);

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.editor_status"),
			TEXT("Editor Status"),
			TEXT("Returns the current Unreal Editor status, map, selected counts, engine version, and PIE state."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("simulate"), UnrealMcp::MakeBoolProperty(TEXT("Whether to start Simulate In Editor instead of Play In Editor."), false));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.start_pie"),
			TEXT("Start PIE"),
			TEXT("Requests a Play In Editor or Simulate In Editor session."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.stop_pie"),
			TEXT("Stop PIE"),
			TEXT("Stops or cancels the current Play In Editor session."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("command"), UnrealMcp::MakeStringProperty(TEXT("Editor or PIE console command to execute.")));
		PropertiesObject->SetObjectField(TEXT("target"), UnrealMcp::MakeStringProperty(TEXT("Where to run the command: auto, editor, or pie."), TEXT("auto")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_console_command"),
				TEXT("Execute Console Command"),
				TEXT("Runs an editor or PIE console command and returns the captured output."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("command"), UnrealMcp::MakeStringProperty(TEXT("Literal Python code, an expression, or a .py file path with optional arguments. Multiline code is automatically run as ExecuteFile unless autoMode=false.")));
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Python execution mode: Auto, ExecuteFile, ExecuteStatement, or EvaluateStatement."), TEXT("Auto")));
			PropertiesObject->SetObjectField(TEXT("scope"), UnrealMcp::MakeStringProperty(TEXT("Python file execution scope: Private or Public."), TEXT("Private")));
			PropertiesObject->SetObjectField(TEXT("autoMode"), UnrealMcp::MakeBoolProperty(TEXT("Whether to protect explicit ExecuteStatement/EvaluateStatement requests by switching multiline code to ExecuteFile."), true));
			PropertiesObject->SetObjectField(TEXT("forceEnable"), UnrealMcp::MakeBoolProperty(TEXT("Whether to force Python initialization if the plugin is loaded but not initialized."), true));
			PropertiesObject->SetObjectField(TEXT("unattended"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the Python command in unattended mode."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_python"),
				TEXT("Execute Python"),
				TEXT("Runs a Python command inside the Unreal Editor and returns its result and captured log output."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("scriptPath"), UnrealMcp::MakeStringProperty(TEXT("Relative or absolute .py file path. Relative paths are resolved against the project directory.")));
			PropertiesObject->SetObjectField(TEXT("args"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional command-line arguments passed to the Python script.")));
			PropertiesObject->SetObjectField(TEXT("scope"), UnrealMcp::MakeStringProperty(TEXT("Python file execution scope: Private or Public."), TEXT("Private")));
			PropertiesObject->SetObjectField(TEXT("forceEnable"), UnrealMcp::MakeBoolProperty(TEXT("Whether to force Python initialization if the plugin is loaded but not initialized."), true));
			PropertiesObject->SetObjectField(TEXT("unattended"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the Python command in unattended mode."), true));
			PropertiesObject->SetObjectField(TEXT("allowOutsideProject"), UnrealMcp::MakeBoolProperty(TEXT("Allow scriptPath outside this Unreal project directory."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_python_file"),
				TEXT("Execute Python File"),
				TEXT("Runs a Python script file with optional arguments, with project-directory safety checks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("lines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of log lines to return."), 120.0));
			PropertiesObject->SetObjectField(TEXT("contains"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive substring filter.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.tail_log"),
				TEXT("Tail Editor Log"),
				TEXT("Returns recent lines from the active Unreal Editor log file."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.map_check"),
				TEXT("Map Check"),
				TEXT("Runs Unreal Editor Map Check on the current level and returns summary counts."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_maps"),
			TEXT("List Maps"),
			TEXT("Lists all map assets under /Game."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Content Browser path such as /Game or /Game/Variant_TwinStick."), TEXT("/Game")));
		PropertiesObject->SetObjectField(TEXT("recursive"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include child paths."), true));
		PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional asset class path filter such as /Script/Engine.Blueprint.")));
		PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of assets to return."), UnrealMcp::DefaultListLimit));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_assets"),
			TEXT("List Assets"),
			TEXT("Lists assets under a Content Browser path."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_selected_assets"),
			TEXT("List Selected Assets"),
			TEXT("Lists the assets currently selected in the Content Browser."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, and classes.")));
		PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PointLight.")));
		PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to return."), UnrealMcp::DefaultListLimit));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_level_actors"),
			TEXT("List Level Actors"),
			TEXT("Lists actors in the current editor world."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.list_selected_actors"),
			TEXT("List Selected Actors"),
				TEXT("Lists the actors currently selected in the level editor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PlayerStart.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to select.")));
			PropertiesObject->SetObjectField(TEXT("clearSelection"), UnrealMcp::MakeBoolProperty(TEXT("Whether to clear the current actor selection before selecting matches."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to select."), UnrealMcp::DefaultListLimit));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.select_actors"),
				TEXT("Select Actors"),
				TEXT("Selects actors in the current level using filters or exact actor paths."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("actorPath"), UnrealMcp::MakeStringProperty(TEXT("Exact actor path to move. If omitted, a single selected actor or actorLabel will be used.")));
			PropertiesObject->SetObjectField(TEXT("actorLabel"), UnrealMcp::MakeStringProperty(TEXT("Exact actor label to move when actorPath is not provided.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("New world X location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("New world Y location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("z"), UnrealMcp::MakeNumberProperty(TEXT("New world Z location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("New world pitch. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("New world yaw. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("New world roll. Omit to keep current value."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.set_actor_transform"),
				TEXT("Set Actor Transform"),
				TEXT("Sets world location and rotation values on a level actor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PointLight.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to edit.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), false));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to edit."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("properties"), UnrealMcp::MakeFlexibleObjectProperty(TEXT("Map of property paths to JSON values. Supports nested paths such as RootComponent.RelativeScale3D, PointLightComponent.Intensity, or Tags.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_properties"),
				TEXT("Batch Set Actor Properties"),
				TEXT("Applies one or more property edits to a set of actors selected by query or current selection."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("scaleX"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale X. Omit to preserve the current value."), 1.0));
			PropertiesObject->SetObjectField(TEXT("scaleY"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale Y. Omit to preserve the current value."), 1.0));
			PropertiesObject->SetObjectField(TEXT("scaleZ"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale Z. Omit to preserve the current value."), 1.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_scale"),
				TEXT("Batch Set Actor Scale"),
				TEXT("Sets relative scale values on one or more actors chosen by query or current selection."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Tags to add or assign to the target actors.")));
			PropertiesObject->SetObjectField(TEXT("replaceExisting"), UnrealMcp::MakeBoolProperty(TEXT("Whether to replace the existing actor tags instead of appending unique tags."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_tags"),
				TEXT("Batch Set Actor Tags"),
				TEXT("Adds or replaces actor Tags on one or more actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("intensity"), UnrealMcp::MakeNumberProperty(TEXT("Point light intensity."), 5000.0));
			PropertiesObject->SetObjectField(TEXT("attenuationRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light attenuation radius."), 1000.0));
			PropertiesObject->SetObjectField(TEXT("sourceRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light source radius."), 0.0));
			PropertiesObject->SetObjectField(TEXT("softSourceRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light soft source radius."), 0.0));
			PropertiesObject->SetObjectField(TEXT("temperature"), UnrealMcp::MakeNumberProperty(TEXT("Point light temperature in Kelvin."), 6500.0));
			PropertiesObject->SetObjectField(TEXT("useTemperature"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light should use color temperature."), false));
			PropertiesObject->SetObjectField(TEXT("castShadows"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light should cast shadows."), true));
			PropertiesObject->SetObjectField(TEXT("visible"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light component should be visible."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_point_light_properties"),
				TEXT("Batch Set Point Light Properties"),
				TEXT("Updates common PointLight properties on one or more actors with PointLightComponents."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("staticMeshPath"), UnrealMcp::MakeStringProperty(TEXT("Static mesh asset path to assign, for example /Game/LevelPrototyping/Meshes/SM_Cube.")));
			PropertiesObject->SetObjectField(TEXT("materialPath"), UnrealMcp::MakeStringProperty(TEXT("Optional material or material instance asset path to assign.")));
			PropertiesObject->SetObjectField(TEXT("materialSlot"), UnrealMcp::MakeNumberProperty(TEXT("Material slot index to update when materialPath is provided."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_configure_static_mesh_actors"),
				TEXT("Batch Configure Static Mesh Actors"),
				TEXT("Assigns static meshes and optional materials to one or more actors with StaticMeshComponents."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.StaticMeshActor.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to lay out.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to reposition."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("columns"), UnrealMcp::MakeNumberProperty(TEXT("Number of columns before wrapping to a new row."), 5.0));
			PropertiesObject->SetObjectField(TEXT("spacingX"), UnrealMcp::MakeNumberProperty(TEXT("Horizontal spacing between actors in Unreal units."), 300.0));
			PropertiesObject->SetObjectField(TEXT("spacingY"), UnrealMcp::MakeNumberProperty(TEXT("Vertical spacing between actors in Unreal units."), 300.0));
			PropertiesObject->SetObjectField(TEXT("spacingZ"), UnrealMcp::MakeNumberProperty(TEXT("Z offset applied per row."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startX"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin X. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startY"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin Y. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startZ"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin Z. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform pitch applied to every actor. Omit to keep current rotation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform yaw applied to every actor. Omit to keep current rotation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform roll applied to every actor. Omit to keep current rotation."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.layout_actors_grid"),
				TEXT("Layout Actors Grid"),
				TEXT("Repositions a set of actors into a configurable grid layout."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.StaticMeshActor.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to lay out.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to reposition."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("radius"), UnrealMcp::MakeNumberProperty(TEXT("Circle radius in Unreal units."), 1000.0));
			PropertiesObject->SetObjectField(TEXT("startAngleDegrees"), UnrealMcp::MakeNumberProperty(TEXT("Start angle in degrees."), 0.0));
			PropertiesObject->SetObjectField(TEXT("arcDegrees"), UnrealMcp::MakeNumberProperty(TEXT("Arc coverage in degrees. Use 360 for a full circle."), 360.0));
			PropertiesObject->SetObjectField(TEXT("centerX"), UnrealMcp::MakeNumberProperty(TEXT("Optional center X. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("centerY"), UnrealMcp::MakeNumberProperty(TEXT("Optional center Y. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("centerZ"), UnrealMcp::MakeNumberProperty(TEXT("Optional center Z. Omit to keep each actor's current Z."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignYawToCenter"), UnrealMcp::MakeBoolProperty(TEXT("Whether each actor should face the center point."), false));
			PropertiesObject->SetObjectField(TEXT("yawOffset"), UnrealMcp::MakeNumberProperty(TEXT("Additional yaw offset in degrees when alignYawToCenter is enabled."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.layout_actors_circle"),
				TEXT("Layout Actors Circle"),
				TEXT("Repositions actors around a circular or arc layout."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Map asset path such as /Game/TopDown/Maps/Lvl_TopDown.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.open_map"),
			TEXT("Open Map"),
			TEXT("Loads a map asset into the editor."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Asset path such as /Game/Variant_TwinStick/Blueprints/BP_TwinStickCharacter.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.open_asset"),
			TEXT("Open Asset"),
			TEXT("Loads an asset and opens it in the appropriate editor."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Asset or folder path to focus in the Content Browser.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.sync_content_browser"),
			TEXT("Sync Content Browser"),
			TEXT("Focuses the Content Browser on an asset or folder."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Native or Blueprint actor class path to spawn.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("z"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location Z."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation pitch."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation yaw."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation roll."), 0.0));
			PropertiesObject->SetObjectField(TEXT("sx"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale X. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sy"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale Y. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sz"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale Z. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("Optional actor label after spawning.")));
			PropertiesObject->SetObjectField(TEXT("properties"), UnrealMcp::MakeFlexibleObjectProperty(TEXT("Optional property path map to apply immediately after spawn.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.spawn_actor"),
			TEXT("Spawn Actor"),
				TEXT("Spawns an actor into the current editor world."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Default actor class path used by any item that does not override classPath.")));
			PropertiesObject->SetObjectField(TEXT("items"), UnrealMcp::MakeObjectArrayProperty(TEXT("Array of actor spawn specs. Each item can override classPath, transform, label, scale, and properties.")));
			PropertiesObject->SetObjectField(TEXT("selectSpawned"), UnrealMcp::MakeBoolProperty(TEXT("Whether to select the spawned actors after the batch completes."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.spawn_actor_batch"),
					TEXT("Spawn Actor Batch"),
					TEXT("Spawns multiple actors in one call, with per-item transform and property overrides."),
					InputSchema);
			}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), UnrealMcp::MakeSpawnActorBasicProperties(true));

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_actor_basic"),
				TEXT("Spawn Actor Basic"),
				TEXT("Spawns a single actor with fixed transform, scale, and label fields, without freeform property overrides."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> BatchProperties = MakeShared<FJsonObject>();
			BatchProperties->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Default actor class path used by any batch item that does not override classPath.")));
			BatchProperties->SetObjectField(TEXT("selectSpawned"), UnrealMcp::MakeBoolProperty(TEXT("Whether to select the spawned actors after the batch completes."), true));

			TSharedPtr<FJsonObject> ItemSchema = UnrealMcp::MakeObjectSchema();
			ItemSchema->SetObjectField(TEXT("properties"), UnrealMcp::MakeSpawnActorBasicProperties(true));

			TSharedPtr<FJsonObject> ItemsProperty = MakeShared<FJsonObject>();
			ItemsProperty->SetStringField(TEXT("type"), TEXT("array"));
			ItemsProperty->SetStringField(TEXT("description"), TEXT("Array of actor spawn specs with fixed transform, scale, class, and label fields."));
			ItemsProperty->SetObjectField(TEXT("items"), ItemSchema);
			BatchProperties->SetObjectField(TEXT("items"), ItemsProperty);

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), BatchProperties);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_actor_batch_basic"),
				TEXT("Spawn Actor Batch Basic"),
				TEXT("Spawns multiple actors in batch using a fixed item schema and no freeform property overrides."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = UnrealMcp::MakeSpawnActorBasicProperties(false);
			PropertiesObject->SetObjectField(TEXT("staticMeshPath"), UnrealMcp::MakeStringProperty(TEXT("Static mesh asset path to assign after spawning, for example /Game/LevelPrototyping/Meshes/SM_Cube.")));
			PropertiesObject->SetObjectField(TEXT("materialPath"), UnrealMcp::MakeStringProperty(TEXT("Optional material or material instance asset path to assign.")));
			PropertiesObject->SetObjectField(TEXT("materialSlot"), UnrealMcp::MakeNumberProperty(TEXT("Material slot index to update when materialPath is provided."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_static_mesh_actor"),
				TEXT("Spawn Static Mesh Actor"),
				TEXT("Spawns a StaticMeshActor and assigns a static mesh, with optional material override."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.destroy_selected_actors"),
				TEXT("Destroy Selected Actors"),
				TEXT("Destroys the currently selected actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths. If omitted with no classPath or paths, all level actors are targeted.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example RecastNavMesh or /Script/Engine.PlayerStart.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to clear.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview actors that would be destroyed without deleting them."), true));
			PropertiesObject->SetObjectField(TEXT("confirmClearAll"), UnrealMcp::MakeBoolProperty(TEXT("Required with dryRun=false when no filter, classPath, or paths are provided."), false));
			PropertiesObject->SetObjectField(TEXT("clearSelection"), UnrealMcp::MakeBoolProperty(TEXT("Whether to clear actor selection before and after the operation."), true));
			PropertiesObject->SetObjectField(TEXT("maxPasses"), UnrealMcp::MakeNumberProperty(TEXT("Maximum destroy passes. Extra passes catch actors such as navigation data that can be recreated after the first pass."), 3.0));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum actors to destroy per pass."), 10000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.clear_level_environment"),
				TEXT("Clear Level Environment"),
				TEXT("Destructively clears actors from the current editor level. With no filters it targets all level actors and repeats a few passes to catch regenerated actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("name"), UnrealMcp::MakeStringProperty(TEXT("New member variable name.")));
			PropertiesObject->SetObjectField(TEXT("pinCategory"), UnrealMcp::MakeStringProperty(TEXT("Blueprint pin category: bool, int, int64, float, double, string, name, text, object, class, struct, enum, byte, or wildcard."), TEXT("bool")));
			PropertiesObject->SetObjectField(TEXT("pinSubCategory"), UnrealMcp::MakeStringProperty(TEXT("Optional pin subcategory. For real pins use float or double; otherwise usually blank.")));
			PropertiesObject->SetObjectField(TEXT("subCategoryObjectPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class, struct, or enum path used by object/class/struct/enum pins.")));
			PropertiesObject->SetObjectField(TEXT("containerType"), UnrealMcp::MakeStringProperty(TEXT("Optional container type: none, array, set, or map."), TEXT("none")));
			PropertiesObject->SetObjectField(TEXT("defaultValue"), UnrealMcp::MakeStringProperty(TEXT("Optional default value stored as Blueprint default text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_variable"),
				TEXT("Blueprint Add Variable"),
				TEXT("Adds a member variable to a Blueprint using a fixed K2 pin type schema."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("New function graph name.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_function"),
				TEXT("Blueprint Add Function"),
				TEXT("Creates a user function graph in a Blueprint, or returns the existing graph if it already exists."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("eventName"), UnrealMcp::MakeStringProperty(TEXT("Event function name, for example ReceiveBeginPlay. If ownerClassPath is custom, creates a custom event with this name."), TEXT("ReceiveBeginPlay")));
			PropertiesObject->SetObjectField(TEXT("ownerClassPath"), UnrealMcp::MakeStringProperty(TEXT("Class that owns the override event, for example /Script/Engine.Actor. Use custom to create a custom event."), TEXT("/Script/Engine.Actor")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_event_node"),
				TEXT("Blueprint Add Event Node"),
				TEXT("Adds an override event node or custom event node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("functionClassPath"), UnrealMcp::MakeStringProperty(TEXT("Class path that owns the function, for example /Script/Engine.KismetSystemLibrary.")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("Function name to call.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 200.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_call_function_node"),
				TEXT("Blueprint Add Call Function Node"),
				TEXT("Adds a K2 call-function node for an existing UFunction."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 400.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_branch_node"),
				TEXT("Blueprint Add Branch Node"),
				TEXT("Adds a Branch node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("macroName"), UnrealMcp::MakeStringProperty(TEXT("StandardMacros macro graph name, usually ForEachLoop or ForEachLoopWithBreak."), TEXT("ForEachLoop")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 400.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 180.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_for_each_node"),
				TEXT("Blueprint Add ForEach Node"),
				TEXT("Adds a StandardMacros ForEach macro instance node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("fromNodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Source node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("fromPin"), UnrealMcp::MakeStringProperty(TEXT("Source pin name or display name.")));
			PropertiesObject->SetObjectField(TEXT("toNodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Target node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("toPin"), UnrealMcp::MakeStringProperty(TEXT("Target pin name or display name.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_connect_pins"),
				TEXT("Blueprint Connect Pins"),
				TEXT("Connects two pins in a Blueprint graph using the K2 schema validation rules."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("nodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("pinName"), UnrealMcp::MakeStringProperty(TEXT("Pin name or display name to update.")));
			PropertiesObject->SetObjectField(TEXT("value"), UnrealMcp::MakeStringProperty(TEXT("New default value text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_set_pin_default"),
				TEXT("Blueprint Set Pin Default"),
				TEXT("Sets a pin default value using the K2 schema."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("originX"), UnrealMcp::MakeNumberProperty(TEXT("Layout origin X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("originY"), UnrealMcp::MakeNumberProperty(TEXT("Layout origin Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("columnSpacing"), UnrealMcp::MakeNumberProperty(TEXT("Horizontal graph spacing."), 320.0));
			PropertiesObject->SetObjectField(TEXT("rowSpacing"), UnrealMcp::MakeNumberProperty(TEXT("Vertical graph spacing."), 180.0));
			PropertiesObject->SetObjectField(TEXT("columns"), UnrealMcp::MakeNumberProperty(TEXT("Number of columns before wrapping."), 4.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_arrange_graph"),
				TEXT("Blueprint Arrange Graph"),
				TEXT("Arranges top-level nodes in a Blueprint graph into a simple grid."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to compile and optionally save.")));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save the Blueprint package after compiling."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.bp_compile_save"),
			TEXT("Blueprint Compile Save"),
			TEXT("Compiles a Blueprint and optionally saves its package."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("parentWidgetName"), UnrealMcp::MakeStringProperty(TEXT("Parent panel widget name. Empty uses the root widget; if there is no root, the new widget becomes root.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Name for the new widget. If omitted, a unique name is generated from the widget class.")));
			PropertiesObject->SetObjectField(TEXT("widgetClass"), UnrealMcp::MakeStringProperty(TEXT("Widget class simple name such as CanvasPanel, VerticalBox, Button, TextBlock, or a class path."), TEXT("TextBlock")));
			PropertiesObject->SetObjectField(TEXT("index"), UnrealMcp::MakeNumberProperty(TEXT("Optional child index for panel insertion. Use -1 to append."), -1.0));
			PropertiesObject->SetObjectField(TEXT("isVariable"), UnrealMcp::MakeBoolProperty(TEXT("Whether the widget should be exposed as a Blueprint variable."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_add"),
				TEXT("Widget Add"),
				TEXT("Adds a widget to a Widget Blueprint tree, optionally as root or as a child of a panel widget."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget name to remove. Use Root or RootWidget to remove the root.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_remove"),
				TEXT("Widget Remove"),
				TEXT("Removes a widget from a Widget Blueprint tree."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Target widget name. Use Root or RootWidget for the root widget.")));
			PropertiesObject->SetObjectField(TEXT("propertyName"), UnrealMcp::MakeStringProperty(TEXT("Property path to set on the widget, for example Text, RenderOpacity, Visibility, or RenderTransform.Translation.X.")));
			PropertiesObject->SetObjectField(TEXT("value"), UnrealMcp::MakeStringProperty(TEXT("Value text imported into the target property. FText properties accept plain text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_set_property"),
				TEXT("Widget Set Property"),
				TEXT("Sets a widget property by reflection using a string value, with plain-text handling for FText fields."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Target widget name whose parent slot layout should be edited.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot X position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot Y position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("width"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot width."), 100.0));
			PropertiesObject->SetObjectField(TEXT("height"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot height."), 40.0));
			PropertiesObject->SetObjectField(TEXT("autoSize"), UnrealMcp::MakeBoolProperty(TEXT("Canvas slot auto-size flag."), false));
			PropertiesObject->SetObjectField(TEXT("zOrder"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot z-order."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignmentX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot alignment X 0..1."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignmentY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot alignment Y 0..1."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMinX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot minimum anchor X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMinY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot minimum anchor Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMaxX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot maximum anchor X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMaxY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot maximum anchor Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingLeft"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot left padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingTop"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot top padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingRight"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot right padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingBottom"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot bottom padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("hAlign"), UnrealMcp::MakeStringProperty(TEXT("Horizontal alignment for box/overlay slots: left, center, right, fill.")));
			PropertiesObject->SetObjectField(TEXT("vAlign"), UnrealMcp::MakeStringProperty(TEXT("Vertical alignment for box/overlay slots: top, center, bottom, fill.")));
			PropertiesObject->SetObjectField(TEXT("sizeRule"), UnrealMcp::MakeStringProperty(TEXT("Box slot size rule: fill or automatic."), TEXT("fill")));
			PropertiesObject->SetObjectField(TEXT("sizeValue"), UnrealMcp::MakeNumberProperty(TEXT("Box slot fill size value."), 1.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_set_slot_layout"),
				TEXT("Widget Set Slot Layout"),
				TEXT("Updates common slot layout fields for CanvasPanel, VerticalBox, HorizontalBox, and Overlay parent slots."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget variable name to bind the event on, for example RefreshButton.")));
			PropertiesObject->SetObjectField(TEXT("eventName"), UnrealMcp::MakeStringProperty(TEXT("Multicast delegate property name, for example OnClicked on Button."), TEXT("OnClicked")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("Optional generated event function name override. Usually leave blank.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Optional event node X position after creation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Optional event node Y position after creation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile first so exposed widget variables exist in the skeleton class."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_bind_event"),
				TEXT("Widget Bind Event"),
				TEXT("Creates or returns a Widget Blueprint component-bound event node for a widget multicast delegate such as Button.OnClicked."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget name to expose or hide as a Blueprint variable.")));
			PropertiesObject->SetObjectField(TEXT("variableName"), UnrealMcp::MakeStringProperty(TEXT("Optional new variable/widget name. Leave blank to keep the current widget name.")));
			PropertiesObject->SetObjectField(TEXT("expose"), UnrealMcp::MakeBoolProperty(TEXT("Whether the widget should be exposed as a Blueprint variable."), true));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile after changing variable exposure."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_bind_blueprint_variable"),
				TEXT("Widget Bind Blueprint Variable"),
				TEXT("Exposes a designer widget as a Blueprint variable, optionally renaming it first."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to create or rebuild.")));
			PropertiesObject->SetObjectField(TEXT("templateName"), UnrealMcp::MakeStringProperty(TEXT("Template preset name. Currently supports mcp_demo_hud."), TEXT("mcp_demo_hud")));
			PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Title text for the generated template."), TEXT("MCP Demo")));
			PropertiesObject->SetObjectField(TEXT("replaceRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to replace the existing root widget tree."), true));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile after building the template. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save the Widget Blueprint package after building. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_build_template"),
				TEXT("Widget Build Template"),
				TEXT("Creates or rebuilds a Widget Blueprint with a practical demo HUD hierarchy."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_round_system"),
				TEXT("Scaffold Round System"),
				TEXT("Creates or updates high-level round-flow Blueprint scaffolding: GameMode, GameState, PlayerState, PlayerController, and RoundManagerComponent."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));
			PropertiesObject->SetObjectField(TEXT("replaceWidgetRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to rebuild the generated shop Widget Blueprint hierarchy."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_shop_system"),
				TEXT("Scaffold Shop System"),
				TEXT("Creates or updates shop manager Blueprint scaffolding plus a basic shop Widget Blueprint template."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_economy_system"),
				TEXT("Scaffold Economy System"),
				TEXT("Creates or updates food/gold/level economy Blueprint component scaffolding and matching PlayerState variables."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_autobattler_ai"),
				TEXT("Scaffold Autobattler AI"),
				TEXT("Creates or updates placeholder unit, AI controller, and combat manager Blueprint scaffolding for auto-battler combat."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));
			PropertiesObject->SetObjectField(TEXT("replaceWidgetRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to rebuild the generated result Widget Blueprint hierarchy."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_result_ui"),
				TEXT("Scaffold Result UI"),
				TEXT("Creates or updates combat result presenter Blueprint scaffolding and a basic result Widget Blueprint template."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("New MCP tool name. Must start with unreal., for example unreal.my_custom_tool.")));
			PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Human-readable tool title.")));
			PropertiesObject->SetObjectField(TEXT("description"), UnrealMcp::MakeStringProperty(TEXT("Short tool description for tools/list and AI tool selection.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative output directory for generated scaffold files."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("argumentSchemaJson"), UnrealMcp::MakeStringProperty(TEXT("Reference JSON schema for the intended arguments. Keep it fixed-schema for AI compatibility.")));
			PropertiesObject->SetObjectField(TEXT("exampleArgumentsJson"), UnrealMcp::MakeStringProperty(TEXT("Example arguments object JSON for the generated test request."), TEXT("{\"message\":\"hello\"}")));
			PropertiesObject->SetObjectField(TEXT("implementationNotes"), UnrealMcp::MakeStringProperty(TEXT("Optional implementation notes to include in the generated README.")));
			PropertiesObject->SetObjectField(TEXT("category"), UnrealMcp::MakeStringProperty(TEXT("Tool category/dispatcher owner: actors, blueprint, editor, memory, scaffold, self-extension, skills, or widget."), TEXT("self-extension")));
			PropertiesObject->SetObjectField(TEXT("riskLevel"), UnrealMcp::MakeStringProperty(TEXT("Tool risk level: read_only, low, medium, high, or critical."), TEXT("low")));
			PropertiesObject->SetObjectField(TEXT("requiresWrite"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool mutates project/editor state."), false));
			PropertiesObject->SetObjectField(TEXT("requiresBuild"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool requires a build step."), false));
			PropertiesObject->SetObjectField(TEXT("requiresExternalProcess"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool starts or depends on an external process."), false));
			PropertiesObject->SetObjectField(TEXT("requiresRestart"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool requires an editor restart to fully verify."), false));
			PropertiesObject->SetObjectField(TEXT("requiresProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool should read/write project memory as part of normal operation."), false));
			PropertiesObject->SetObjectField(TEXT("requiresLock"), UnrealMcp::MakeBoolProperty(TEXT("Whether the tool must acquire the self-extension session lock."), false));
			PropertiesObject->SetObjectField(TEXT("dryRunSupport"), UnrealMcp::MakeBoolProperty(TEXT("Whether the generated tool should expose a dryRun argument."), false));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Whether to overwrite an existing scaffold folder."), false));
			PropertiesObject->SetObjectField(TEXT("includeChatCommandSnippet"), UnrealMcp::MakeBoolProperty(TEXT("Whether to generate an optional direct slash-command patch fragment."), true));
			PropertiesObject->SetObjectField(TEXT("includeLegacyCompatibility"), UnrealMcp::MakeBoolProperty(TEXT("Also generate legacy ToolDefinition/ExecuteToolHandler fragments. Disabled by default; new tools should use descriptor-first patches."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_mcp_tool"),
				TEXT("Scaffold MCP Tool"),
				TEXT("Generates descriptor-first C++ patch files, ToolRegistry patch metadata, docs, an extension report, and a test request for adding a new Unreal MCP tool after review and rebuild."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root to scan."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("includeSavedTestScaffolds"), UnrealMcp::MakeBoolProperty(TEXT("Whether to also scan Saved/UnrealMcp/TestScaffolds."), true));
			PropertiesObject->SetObjectField(TEXT("toolNameFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive filter applied to tool names and scaffold paths.")));
			PropertiesObject->SetObjectField(TEXT("readyOnly"), UnrealMcp::MakeBoolProperty(TEXT("Only return scaffolds with all required files and a valid TestRequest.json."), false));
			PropertiesObject->SetObjectField(TEXT("includeFileText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full file text for each scaffold file."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum per-file preview characters."), 1200.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_list_scaffolds"),
				TEXT("List MCP Scaffolds"),
				TEXT("Lists generated MCP tool scaffold directories, readiness, tool names, schema status, and test request status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name whose scaffold should be inspected. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-local scaffold directory to inspect. To inspect shared repo recipes, pass toolName and leave scaffoldDir empty.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("includeFileText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full file text for scaffold files."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum per-file preview characters."), 2000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_inspect_scaffold"),
				TEXT("Inspect MCP Scaffold"),
				TEXT("Inspects one generated MCP scaffold for required files, schema compatibility, test request validity, patch fragments, and whether the tool is already loaded."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("patchText"), UnrealMcp::MakeStringProperty(TEXT("Raw C++ patch/legacy fragment text to validate. If empty, reads scaffoldDir/toolName + patchName.")));
			PropertiesObject->SetObjectField(TEXT("patchName"), UnrealMcp::MakeStringProperty(TEXT("Patch file or alias: ToolRegistrar.patch.cpp, ToolRegistrarCall.patch.cpp, CategoryHandlerFunction.patch.cpp, CategoryDispatcherBranch.patch.cpp, ChatCommand.patch.cpp, or legacy fragments LegacyToolDefinition.legacy.cpp / LegacyExecuteToolHandler.legacy.cpp."), TEXT("ToolRegistrar.patch.cpp")));
			PropertiesObject->SetObjectField(TEXT("snippetText"), UnrealMcp::MakeStringProperty(TEXT("Legacy alias for patchText.")));
			PropertiesObject->SetObjectField(TEXT("snippetName"), UnrealMcp::MakeStringProperty(TEXT("Legacy alias for patchName."), TEXT("ToolRegistrar.patch.cpp")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Expected MCP tool name for tool-name literal checks.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-local scaffold directory to read when snippetText is empty. To validate shared repo recipes, pass toolName and leave scaffoldDir empty.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_validate_cpp_patch"),
				TEXT("Validate C++ Patch"),
				TEXT("Runs static safety checks against MCP scaffold C++ patch fragments before applying them to the plugin source."),
				InputSchema);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_validate_cpp_snippet"),
				TEXT("Validate C++ Snippet Legacy"),
				TEXT("Legacy alias for unreal.mcp_validate_cpp_patch. New workflows should use patch fragments."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name whose scaffold patch fragment should be patched. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-local scaffold directory containing the patch fragment. To patch shared repo recipes, pass toolName and leave scaffoldDir empty only after copying to a project-local draft.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("patchName"), UnrealMcp::MakeStringProperty(TEXT("Patch file or alias: ToolRegistrar.patch.cpp, ToolRegistrarCall.patch.cpp, CategoryHandlerFunction.patch.cpp, CategoryDispatcherBranch.patch.cpp, ChatCommand.patch.cpp, or legacy fragments LegacyToolDefinition.legacy.cpp / LegacyExecuteToolHandler.legacy.cpp.")));
			PropertiesObject->SetObjectField(TEXT("snippetName"), UnrealMcp::MakeStringProperty(TEXT("Legacy alias for patchName.")));
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Patch mode: replace_all, replace_text, append, or prepend. Auto-selected when empty.")));
			PropertiesObject->SetObjectField(TEXT("newText"), UnrealMcp::MakeStringProperty(TEXT("Replacement text for replace_all mode.")));
			PropertiesObject->SetObjectField(TEXT("findText"), UnrealMcp::MakeStringProperty(TEXT("Exact text to find for replace_text mode.")));
			PropertiesObject->SetObjectField(TEXT("replaceText"), UnrealMcp::MakeStringProperty(TEXT("Replacement text for replace_text mode.")));
			PropertiesObject->SetObjectField(TEXT("appendText"), UnrealMcp::MakeStringProperty(TEXT("Text to append when mode=append.")));
			PropertiesObject->SetObjectField(TEXT("prependText"), UnrealMcp::MakeStringProperty(TEXT("Text to prepend when mode=prepend.")));
			PropertiesObject->SetObjectField(TEXT("replaceAll"), UnrealMcp::MakeBoolProperty(TEXT("Replace all findText occurrences instead of just the first."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview patch changes without writing the file."), true));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Create a timestamped patch backup before writing."), true));
			PropertiesObject->SetObjectField(TEXT("allowUnsafe"), UnrealMcp::MakeBoolProperty(TEXT("Allow writing patches that fail static validation. Use only after manual review."), false));
			PropertiesObject->SetObjectField(TEXT("diffPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum patch diff preview lines."), 120.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_patch_scaffold_patch"),
				TEXT("Patch MCP Scaffold Patch"),
				TEXT("Safely patches a generated MCP scaffold patch fragment with dry-run diff, static validation, idempotence checks, and backups."),
				InputSchema);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_patch_scaffold_snippet"),
				TEXT("Patch MCP Scaffold Snippet Legacy"),
				TEXT("Legacy alias for unreal.mcp_patch_scaffold_patch. New workflows should use patch fragments."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Existing MCP tool name to validate. Used when schemaJson is empty.")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Raw JSON object schema to validate. If set, this takes precedence over toolName.")));
			PropertiesObject->SetObjectField(TEXT("returnNormalizedSchema"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the normalized schema in structured output."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_validate_tool_schema"),
				TEXT("Validate MCP Tool Schema"),
				TEXT("Checks whether a tool input schema is compatible with OpenAI function calling, including additionalProperties risks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_tool_audit"),
				TEXT("Audit MCP Tools"),
				TEXT("Read-only audit of registered MCP tools, handlers, README documentation, and AI schema compatibility."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key to highlight in pipeline/workbench status."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogTail"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the latest build log tail."), false));
			PropertiesObject->SetObjectField(TEXT("buildLogTailLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum latest build log tail lines."), 80.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_workbench_status"),
				TEXT("MCP Workbench Status"),
				TEXT("Read-only dashboard summary for self-extension health: tools, audit, memory, manifests, build/test artifacts, and supervisor status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key."), TEXT("current")));
			PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Short human-readable memory summary.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Current status, for example pending, in_progress, blocked, or done.")));
			PropertiesObject->SetObjectField(TEXT("nextStep"), UnrealMcp::MakeStringProperty(TEXT("Next action to resume after restart.")));
			PropertiesObject->SetObjectField(TEXT("contentJson"), UnrealMcp::MakeStringProperty(TEXT("Optional JSON object payload with detailed state.")));
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional tags for this memory entry.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_write"),
				TEXT("Project Memory Write"),
				TEXT("Writes a resumable project memory entry under Saved/UnrealMcp for editor restart handoff."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Optional memory entry key. Empty returns all entries.")));
			PropertiesObject->SetObjectField(TEXT("includeContent"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include detailed content payloads."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_read"),
				TEXT("Project Memory Read"),
				TEXT("Reads resumable project memory entries from Saved/UnrealMcp."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("keyFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive substring filter for memory keys.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Optional exact status filter.")));
			PropertiesObject->SetObjectField(TEXT("tag"), UnrealMcp::MakeStringProperty(TEXT("Optional tag filter.")));
			PropertiesObject->SetObjectField(TEXT("includeContent"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include detailed content payloads."), false));
			PropertiesObject->SetObjectField(TEXT("maxEntries"), UnrealMcp::MakeNumberProperty(TEXT("Maximum entries to return."), 50.0));
			PropertiesObject->SetObjectField(TEXT("sortDescending"), UnrealMcp::MakeBoolProperty(TEXT("Sort newest updatedAtUtc entries first."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_view"),
				TEXT("Project Memory View"),
				TEXT("Views persistent project memory with key/status/tag filters and optional content payloads."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key to edit.")));
			PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Optional new summary. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Optional new status. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("nextStep"), UnrealMcp::MakeStringProperty(TEXT("Optional new next step. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("contentJson"), UnrealMcp::MakeStringProperty(TEXT("Optional JSON object to merge or replace into content.")));
			PropertiesObject->SetObjectField(TEXT("contentMode"), UnrealMcp::MakeStringProperty(TEXT("Content update mode: merge or replace."), TEXT("merge")));
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional tags to replace, append, or remove.")));
			PropertiesObject->SetObjectField(TEXT("tagsMode"), UnrealMcp::MakeStringProperty(TEXT("Tags update mode: replace, append, or remove."), TEXT("replace")));
			PropertiesObject->SetObjectField(TEXT("createIfMissing"), UnrealMcp::MakeBoolProperty(TEXT("Create the memory entry if it does not exist."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview the edit without writing ProjectMemory.json."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_edit"),
				TEXT("Project Memory Edit"),
				TEXT("Edits one persistent project memory entry with field-level updates, content merge/replace, tags modes, and dry run."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key to delete.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview deletion without writing ProjectMemory.json."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_delete"),
				TEXT("Project Memory Delete"),
				TEXT("Deletes one persistent project memory entry. Defaults to dryRun=true for safety."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to scan. Defaults to Tools/UnrealMcpSkills.")));
			PropertiesObject->SetObjectField(TEXT("nameFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive skill name filter.")));
			PropertiesObject->SetObjectField(TEXT("includeText"), UnrealMcp::MakeBoolProperty(TEXT("Include full skill text in results."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum preview characters per skill."), 1200.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_list"),
				TEXT("Skill List"),
				TEXT("Lists project-local SKILL.md or *.skill files that Chat can read and apply as reusable task instructions."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Project skill name to read. Used when skillPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("skillPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute path to SKILL.md or *.skill.")));
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to search by skillName.")));
			PropertiesObject->SetObjectField(TEXT("includeText"), UnrealMcp::MakeBoolProperty(TEXT("Include full skill text."), true));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum preview characters."), 4000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_read"),
				TEXT("Skill Read"),
				TEXT("Reads one project-local skill and returns title, description, preview, and optionally full text."),
				InputSchema);
		}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Project skill name to apply. Used when skillPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("skillPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute path to SKILL.md or *.skill.")));
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to search by skillName.")));
			PropertiesObject->SetObjectField(TEXT("task"), UnrealMcp::MakeStringProperty(TEXT("Current task/context this skill should be applied to.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Optional project memory key for recording skill application.")));
			PropertiesObject->SetObjectField(TEXT("writeMemory"), UnrealMcp::MakeBoolProperty(TEXT("Record skill application into project memory."), true));
			PropertiesObject->SetObjectField(TEXT("includeFullText"), UnrealMcp::MakeBoolProperty(TEXT("Return full skill text instead of a shorter preview."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_apply"),
				TEXT("Skill Apply"),
					TEXT("Applies a project-local skill by returning its instructions and optionally recording the application in project memory."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Human-readable goal for this activity recording session.")));
				PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Optional explicit session id. Defaults to timestamp-guid.")));
				PropertiesObject->SetObjectField(TEXT("recordIntervalSeconds"), UnrealMcp::MakeNumberProperty(TEXT("Heartbeat interval in seconds. Clamped to 10..3600; default 60."), 60.0));
				PropertiesObject->SetObjectField(TEXT("reset"), UnrealMcp::MakeBoolProperty(TEXT("Start a new session instead of resuming current state."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_recording_start"),
					TEXT("Skill Recording Start"),
					TEXT("Starts local high-level activity recording for later skill distillation. Writes JSONL under Saved/UnrealMcp/ActivityLog."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Optional stop reason or session summary.")));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_recording_stop"),
					TEXT("Skill Recording Stop"),
					TEXT("Stops local activity recording after writing a final recording_stopped event."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("includeRecentEvents"), UnrealMcp::MakeBoolProperty(TEXT("Include recent JSONL events from the active session."), false));
				PropertiesObject->SetObjectField(TEXT("maxEvents"), UnrealMcp::MakeNumberProperty(TEXT("Maximum recent events to include."), 20.0));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_activity_status"),
					TEXT("Skill Activity Status"),
					TEXT("Reports the active activity recording session, log paths, draft paths, counters, and optionally recent events."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Activity session id to distill. Defaults to current session.")));
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Output skill folder name. Defaults to sanitized title/goal.")));
				PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Draft SKILL.md title.")));
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Override learned goal text.")));
				PropertiesObject->SetObjectField(TEXT("writeDraft"), UnrealMcp::MakeBoolProperty(TEXT("Write draft SKILL.md under Saved/UnrealMcp/SkillDrafts."), true));
				PropertiesObject->SetObjectField(TEXT("includeEvents"), UnrealMcp::MakeBoolProperty(TEXT("Append event summaries to the draft for review."), false));
				PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite an existing draft when writeDraft=true."), true));
				PropertiesObject->SetObjectField(TEXT("maxEvents"), UnrealMcp::MakeNumberProperty(TEXT("Maximum JSONL events to distill."), 200.0));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_distill_from_activity"),
					TEXT("Skill Distill From Activity"),
					TEXT("Summarizes one activity session into a reusable SKILL.md draft and optionally writes it under Saved/UnrealMcp/SkillDrafts."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Draft skill folder name.")));
				PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Draft title used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Draft goal used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Draft summary used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("draftText"), UnrealMcp::MakeStringProperty(TEXT("Complete SKILL.md text to save as a draft.")));
				PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite an existing draft."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_save_draft"),
					TEXT("Skill Save Draft"),
					TEXT("Writes or replaces a local skill draft under Saved/UnrealMcp/SkillDrafts without promoting it into project skills."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Skill name to promote under Tools/UnrealMcpSkills.")));
					PropertiesObject->SetObjectField(TEXT("draftPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local draft path. Defaults to Saved/UnrealMcp/SkillDrafts/<skillName>/SKILL.md.")));
					PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite existing promoted skill."), false));
					PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview promotion without writing Tools/UnrealMcpSkills."), true));
					PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Back up existing promoted SKILL.md and write a manifest before overwriting."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_promote_draft"),
					TEXT("Skill Promote Draft"),
					TEXT("Promotes a reviewed draft into Tools/UnrealMcpSkills/<skillName>/SKILL.md for future Chat skill discovery."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Tool name whose scaffold should be applied. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-local scaffold directory containing generated descriptor-first patch files. To apply shared repo recipes, pass toolName and leave scaffoldDir empty.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview changes without modifying source."), true));
			PropertiesObject->SetObjectField(TEXT("applyChatCommand"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply optional ChatCommand.patch.cpp."), false));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Whether to create rollback backup and manifest when dryRun=false."), true));
			PropertiesObject->SetObjectField(TEXT("validatePatches"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run C++ patch-fragment safety validation before applying."), true));
			PropertiesObject->SetObjectField(TEXT("allowUnsafePatches"), UnrealMcp::MakeBoolProperty(TEXT("Allow applying patch fragments that fail static validation. Use only after manual review."), false));
			PropertiesObject->SetObjectField(TEXT("targetDiffPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum target source diff preview lines returned during dry run/apply."), 120.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_apply_scaffold"),
				TEXT("Apply MCP Scaffold"),
				TEXT("Safely previews or applies descriptor-first MCP scaffold patches: ToolRegistryPatch.json, registrar descriptor, category handler, and dispatcher branch with idempotence checks and backups."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local manifest path. Defaults to Saved/UnrealMcp/LastExtensionApply.json.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview rollback without restoring source."), false));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Restore even if current source hash differs from the apply manifest."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_rollback_last_extension"),
				TEXT("Rollback Last MCP Extension"),
				TEXT("Restores files changed by the last mcp_apply_scaffold backup, with hash safety checks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Lock mode: status, acquire, release, or refresh."), TEXT("status")));
			PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Session id for release/refresh.")));
			PropertiesObject->SetObjectField(TEXT("owner"), UnrealMcp::MakeStringProperty(TEXT("Human-readable lock owner."), TEXT("Unreal MCP Chat")));
			PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Why this extension session is locked.")));
			PropertiesObject->SetObjectField(TEXT("ttlSeconds"), UnrealMcp::MakeNumberProperty(TEXT("Lock TTL in seconds, clamped to 30..86400."), 900.0));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Override a stale or foreign lock. Use carefully."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_lock_extension_session"),
				TEXT("Lock MCP Extension Session"),
				TEXT("Acquires, refreshes, releases, or inspects the MCP extension lock used to avoid simultaneous source edits/builds/tests."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("Short label for the snapshot directory."), TEXT("manual")));
			PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Why this project state backup is being created.")));
			PropertiesObject->SetObjectField(TEXT("includeSource"), UnrealMcp::MakeBoolProperty(TEXT("Include Unreal MCP source/header files."), true));
			PropertiesObject->SetObjectField(TEXT("includeReadmes"), UnrealMcp::MakeBoolProperty(TEXT("Include root and plugin README files."), true));
			PropertiesObject->SetObjectField(TEXT("includeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ProjectMemory.json."), true));
			PropertiesObject->SetObjectField(TEXT("includeManifests"), UnrealMcp::MakeBoolProperty(TEXT("Include extension apply manifests."), true));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogs"), UnrealMcp::MakeBoolProperty(TEXT("Include the latest few build logs."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview snapshot contents without writing backup files."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_backup_project_state"),
				TEXT("Backup MCP Project State"),
				TEXT("Snapshots Unreal MCP source, README, project memory, manifests, and optional build logs before high-risk extension changes."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Specific project-local/absolute manifest to restore. If empty, selects from ExtensionBackups.")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional toolName filter when manifestPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("selector"), UnrealMcp::MakeStringProperty(TEXT("Manifest selector when manifestPath is empty: latest or oldest."), TEXT("latest")));
			PropertiesObject->SetObjectField(TEXT("manifestIndex"), UnrealMcp::MakeNumberProperty(TEXT("Optional zero-based candidate index after filtering/sorting; -1 uses selector."), -1.0));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview rollback without restoring source."), false));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Restore even if current source hash differs from the apply manifest."), false));
			PropertiesObject->SetObjectField(TEXT("createPreRollbackBackup"), UnrealMcp::MakeBoolProperty(TEXT("Snapshot current project state before a real rollback."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_rollback_to_manifest"),
				TEXT("Rollback MCP To Manifest"),
				TEXT("Restores an MCP extension from a selected historical apply manifest, not only the latest one, with optional pre-rollback snapshot."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("buildLogPath"), UnrealMcp::MakeStringProperty(TEXT("Project-local/absolute build log path. Defaults to newest Saved/UnrealMcp/BuildLogs/*.log.")));
			PropertiesObject->SetObjectField(TEXT("maxErrors"), UnrealMcp::MakeNumberProperty(TEXT("Maximum compiler errors to analyze."), 8.0));
			PropertiesObject->SetObjectField(TEXT("contextLines"), UnrealMcp::MakeNumberProperty(TEXT("Source context lines before/after each error."), 4.0));
			PropertiesObject->SetObjectField(TEXT("includeSourceContext"), UnrealMcp::MakeBoolProperty(TEXT("Include nearby source lines for each parsed error."), true));
			PropertiesObject->SetObjectField(TEXT("autoPatch"), UnrealMcp::MakeBoolProperty(TEXT("Attempt only deterministic safe patches when available."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview any autoPatch changes without writing files."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_compile_error_fix_plan"),
				TEXT("Compile Error Fix Plan"),
				TEXT("Parses build logs into error file/line/source context, probable cause, suggested fixes, and safe auto-patch status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("platform"), UnrealMcp::MakeStringProperty(TEXT("Launcher platform to generate: all, macos, or windows."), TEXT("all")));
			PropertiesObject->SetObjectField(TEXT("outputDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative output directory for generated supervisor launchers."), TEXT("Tools/UnrealMcpSupervisor")));
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("macOS LaunchAgent label.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Pipeline memory key embedded in generated commands."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("argsJson"), UnrealMcp::MakeStringProperty(TEXT("Pipeline args JSON embedded in generated commands. Defaults to {\"memoryKey\": memoryKey}.")));
			PropertiesObject->SetObjectField(TEXT("endpointUrl"), UnrealMcp::MakeStringProperty(TEXT("MCP endpoint URL used by generated supervisor commands."), TEXT("http://127.0.0.1:8765/mcp")));
			PropertiesObject->SetObjectField(TEXT("supervisorLogDir"), UnrealMcp::MakeStringProperty(TEXT("Directory where generated supervisor commands should write logs."), TEXT("Saved/UnrealMcp/SupervisorLogs")));
			PropertiesObject->SetObjectField(TEXT("editorCmd"), UnrealMcp::MakeStringProperty(TEXT("Optional UnrealEditor executable path for generated commands.")));
			PropertiesObject->SetObjectField(TEXT("installLaunchAgent"), UnrealMcp::MakeBoolProperty(TEXT("Also copy the generated macOS plist to ~/Library/LaunchAgents."), false));
			PropertiesObject->SetObjectField(TEXT("launchAtLoad"), UnrealMcp::MakeBoolProperty(TEXT("Set RunAtLoad=true in the generated macOS LaunchAgent."), false));
			PropertiesObject->SetObjectField(TEXT("autoRestart"), UnrealMcp::MakeBoolProperty(TEXT("Generate commands with supervisor pipeline --auto-restart."), true));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite existing generated launcher files."), true));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview the install generator command without writing launcher files."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_supervisor_install"),
				TEXT("Install MCP Supervisor Launchers"),
				TEXT("Generates macOS LaunchAgent/command and Windows PowerShell launchers for the external Unreal MCP supervisor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Tool name whose schema/scaffold should drive generated MCP tests.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory. Tests are written under scaffoldDir/Tests by default.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute test output directory. Defaults to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Optional raw input schema JSON. If omitted, the loaded tool schema, scaffold README schema, or TestRequest.json is used.")));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Whether to overwrite generated test files when content changes."), true));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview generated test files without writing them."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_generate_tests"),
				TEXT("Generate MCP Tests"),
				TEXT("Generates a Tests/*.json suite for an MCP tool from its schema, including happy path, missing required, boundary value, and wrong type cases."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("target"), UnrealMcp::MakeStringProperty(TEXT("UBT target to build."), FString::Printf(TEXT("%sEditor"), FApp::GetProjectName())));
			PropertiesObject->SetObjectField(TEXT("platform"), UnrealMcp::MakeStringProperty(TEXT("UBT platform. Empty/default uses the host editor platform."), UnrealMcp::GetHostBuildPlatformName()));
			PropertiesObject->SetObjectField(TEXT("configuration"), UnrealMcp::MakeStringProperty(TEXT("UBT configuration."), TEXT("Development")));
			PropertiesObject->SetObjectField(TEXT("extraArgs"), UnrealMcp::MakeStringProperty(TEXT("Optional additional UBT arguments appended to the build command.")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional newly integrated tool name to persist into project memory for post-restart testing.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local TestRequest.json path to persist for post-restart testing.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local Tests directory to persist for post-restart suite testing.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Optional scaffold directory. If testRequestPath is empty, scaffoldDir/TestRequest.json is remembered.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used for restart handoff."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write restart handoff state before and after the build."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_build_editor"),
				TEXT("Build Editor"),
				TEXT("Runs Unreal Build Tool for the editor target, captures build logs, parses errors, and writes restart handoff state for MCP extension testing."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional tool name. If empty, read from TestRequest.json or project memory.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute TestRequest.json path.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute Tests directory. Used when runSuite=true.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName if no testRequestPath/scaffoldDir is provided."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used to resume after editor restart."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("readProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to read test path/tool name from project memory when arguments are omitted."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write test result back to project memory."), true));
			PropertiesObject->SetObjectField(TEXT("expectToolListed"), UnrealMcp::MakeBoolProperty(TEXT("Whether missing tools/list entry should fail the test."), true));
			PropertiesObject->SetObjectField(TEXT("executeTool"), UnrealMcp::MakeBoolProperty(TEXT("Whether to execute the tools/call request from TestRequest.json after tools/list check."), true));
			PropertiesObject->SetObjectField(TEXT("runSuite"), UnrealMcp::MakeBoolProperty(TEXT("Delegate to unreal.mcp_run_test_suite instead of running one test request."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_run_tool_test"),
				TEXT("Run MCP Tool Test"),
				TEXT("Reads a TestRequest.json or wrapped test case, checks whether the tool is listed, executes the tool call through in-editor MCP handlers, compares expected error state, and records the result."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional tool name. If empty, read from project memory or scaffold TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute Tests directory containing *.json test cases.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory. Defaults testsDir to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used to resume after editor restart."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("readProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to read tool/scaffold/tests paths from project memory."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write suite result back to project memory."), true));
			PropertiesObject->SetObjectField(TEXT("executeTool"), UnrealMcp::MakeBoolProperty(TEXT("Whether each test should execute the tools/call request."), true));
			PropertiesObject->SetObjectField(TEXT("stopOnFailure"), UnrealMcp::MakeBoolProperty(TEXT("Stop after the first failed test case."), false));
			PropertiesObject->SetObjectField(TEXT("fallbackToSingleTest"), UnrealMcp::MakeBoolProperty(TEXT("If no Tests/*.json files exist, fall back to scaffoldDir/TestRequest.json."), true));
			PropertiesObject->SetObjectField(TEXT("includePassedStructuredContent"), UnrealMcp::MakeBoolProperty(TEXT("Include structuredContent for passed cases, not only failed cases."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_run_test_suite"),
				TEXT("Run MCP Test Suite"),
				TEXT("Runs all JSON test cases under a scaffold Tests directory and reports pass rate, failed cases, failure text, and structuredContent."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Pipeline mode: auto, apply_build, dry_run, resume_test, test_only."), TEXT("auto")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name to integrate/test.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing descriptor-first patches and TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Optional schema JSON to validate before applying patches. If omitted, the scaffold README schema is used when present.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional TestRequest.json path. Defaults to scaffoldDir/TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Optional Tests directory. Defaults to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key for restart handoff."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("task"), UnrealMcp::MakeStringProperty(TEXT("Natural-language task goal used by preview_change_plan and verify_task_outcome gates.")));
			PropertiesObject->SetObjectField(TEXT("apply"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply scaffold patches after dry run."), true));
			PropertiesObject->SetObjectField(TEXT("build"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run Unreal Build Tool after applying patches."), true));
			PropertiesObject->SetObjectField(TEXT("runTest"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the generated tool test when safe in the current editor session."), true));
			PropertiesObject->SetObjectField(TEXT("runTestSuite"), UnrealMcp::MakeBoolProperty(TEXT("Run Tests/*.json suite instead of only TestRequest.json."), true));
			PropertiesObject->SetObjectField(TEXT("generateTests"), UnrealMcp::MakeBoolProperty(TEXT("Generate or refresh Tests/*.json before apply/build/test."), true));
			PropertiesObject->SetObjectField(TEXT("overwriteTests"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite generated test files when content changes."), true));
			PropertiesObject->SetObjectField(TEXT("dryRunOnly"), UnrealMcp::MakeBoolProperty(TEXT("Only run validate and apply dry run; skip apply/build/test."), false));
			PropertiesObject->SetObjectField(TEXT("applyChatCommand"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply optional ChatCommand.patch.cpp."), false));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Whether to create rollback backup during real apply."), true));
			PropertiesObject->SetObjectField(TEXT("backupProjectState"), UnrealMcp::MakeBoolProperty(TEXT("Create a broad project-state snapshot before real apply/build/test changes."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write pipeline state into project memory."), true));
			PropertiesObject->SetObjectField(TEXT("enforceGate"), UnrealMcp::MakeBoolProperty(TEXT("Require preview_change_plan before schema validation and dry-run/apply."), true));
			PropertiesObject->SetObjectField(TEXT("captureSnapshots"), UnrealMcp::MakeBoolProperty(TEXT("Capture before/after project snapshots around real pipeline work."), true));
			PropertiesObject->SetObjectField(TEXT("verifyOutcome"), UnrealMcp::MakeBoolProperty(TEXT("Run verify_task_outcome after tests when no restart deferral is required."), true));
			PropertiesObject->SetObjectField(TEXT("classifyFailures"), UnrealMcp::MakeBoolProperty(TEXT("Classify failed pipeline steps and attach fix/rollback guidance."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_extension_pipeline"),
				TEXT("MCP Extension Pipeline"),
				TEXT("High-level MCP extension workflow: validate schema, dry-run apply, apply descriptor-first patches, write memory, build editor, request restart, and resume tool test."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key to inspect."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("includeAllMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether memory summaries should include full content payloads."), false));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogTail"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the latest build log tail."), true));
			PropertiesObject->SetObjectField(TEXT("buildLogTailLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum latest build log tail lines."), 80.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_pipeline_status"),
				TEXT("MCP Pipeline Status"),
				TEXT("Summarizes project memory, last apply manifest, latest build log, test scaffolds, and recommended next extension step."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local manifest path. Defaults to Saved/UnrealMcp/LastExtensionApply.json.")));
			PropertiesObject->SetObjectField(TEXT("maxPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum changed lines to include in the diff preview."), 120.0));
			PropertiesObject->SetObjectField(TEXT("includeFullText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full before/after source snapshots in structured output."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_diff_last_apply"),
				TEXT("Diff Last MCP Apply"),
				TEXT("Reads the last mcp_apply_scaffold manifest and returns a safe before/after source diff preview from backup snapshots."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview cleanup candidates without deleting anything."), true));
			PropertiesObject->SetObjectField(TEXT("cleanTestScaffolds"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/TestScaffolds child directories."), true));
			PropertiesObject->SetObjectField(TEXT("cleanTestRequests"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/TestRequests child directories."), false));
			PropertiesObject->SetObjectField(TEXT("cleanBuildLogs"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/BuildLogs/*.log files."), false));
			PropertiesObject->SetObjectField(TEXT("cleanExtensionBackups"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ExtensionBackups child directories."), false));
			PropertiesObject->SetObjectField(TEXT("cleanProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ProjectMemory.json. Use carefully."), false));
			PropertiesObject->SetObjectField(TEXT("maxAgeDays"), UnrealMcp::MakeNumberProperty(TEXT("Only include artifacts at least this many days old. 0 disables age filtering."), 0.0));
			PropertiesObject->SetObjectField(TEXT("nameContains"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive path substring filter for targeted cleanup.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_clean_test_artifacts"),
				TEXT("Clean MCP Test Artifacts"),
				TEXT("Safely previews or deletes generated MCP test scaffolds, test requests, build logs, extension backups, and memory artifacts under Saved/UnrealMcp."),
				InputSchema);
		}

				{
					TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to compile.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.compile_blueprint"),
				TEXT("Compile Blueprint"),
				TEXT("Compiles a Blueprint asset."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Content Browser path to scan for Blueprints."), TEXT("/Game")));
			PropertiesObject->SetObjectField(TEXT("recursive"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include child paths."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of Blueprints to compile in one call."), 100.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.compile_blueprints_in_path"),
				TEXT("Compile Blueprints In Path"),
				TEXT("Finds Blueprint assets under a path and compiles them in batch."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("assetPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to create, for example /Game/Blueprints/BP_NewActor.")));
		PropertiesObject->SetObjectField(TEXT("parentClass"), UnrealMcp::MakeStringProperty(TEXT("Native or Blueprint parent class path."), TEXT("/Script/Engine.Actor")));
		PropertiesObject->SetObjectField(TEXT("openAfterCreate"), UnrealMcp::MakeBoolProperty(TEXT("Whether to open the asset editor after creation."), true));
		PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile the new Blueprint immediately."), true));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.create_blueprint_class"),
			TEXT("Create Blueprint Class"),
			TEXT("Creates a Blueprint asset from a parent class."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> SaveMapsProperty = UnrealMcp::MakeBoolProperty(TEXT("Whether to save dirty maps."), true);
		TSharedPtr<FJsonObject> SaveAssetsProperty = UnrealMcp::MakeBoolProperty(TEXT("Whether to save dirty content assets."), true);

		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("saveMaps"), SaveMapsProperty);
		PropertiesObject->SetObjectField(TEXT("saveAssets"), SaveAssetsProperty);

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.save_dirty_packages"),
			TEXT("Save Dirty Packages"),
			TEXT("Saves dirty map packages and/or dirty content packages without prompting."),
			InputSchema);
	}
}
