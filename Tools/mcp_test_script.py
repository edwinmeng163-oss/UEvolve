import sys
import unreal

editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = editor_subsystem.get_editor_world() if editor_subsystem else None
world_name = world.get_name() if world else "None"
unreal.log(f"[MCPTestScript] world={world_name} args={sys.argv[1:]}")
print(f"MCP_TEST_SCRIPT_OK world={world_name} args={sys.argv[1:]}")
