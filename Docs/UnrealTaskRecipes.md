# Unreal Task Recipes

This page gives UEvolve RAG compact, action-oriented recipes for common Unreal Editor tasks. Prefer these recipes before falling back to arbitrary Python or console commands.

## First-Person Ground Character

Goal: create or configure a playable first-person character that starts at a known point in the level.

Recommended flow:

1. Inspect the project with `unreal.editor_status`, `unreal.list_maps`, and `unreal.list_assets`.
2. Search existing assets before creating new ones.
3. Use `unreal.scaffold_recipe` with `recipeName=first_person_ground_character` for a bounded high-level plan.
4. If writing assets, capture a before snapshot with `unreal.capture_project_snapshot`.
5. Create or reuse a Character/Pawn Blueprint and GameMode Blueprint.
6. Set GameMode `DefaultPawnClass` to the playable character.
7. Set current World Settings GameMode override when the level should use it.
8. Create or move a PlayerStart to a clear spawn location.
9. Configure camera height, camera FOV, movement speed, acceleration, deceleration, capsule size, and controller yaw.
10. Compile and save touched Blueprints.
11. Run `unreal.map_check`, optionally start PIE, then verify with `unreal.verify_task_outcome`.

Important checks:

- Do not run edit tools while PIE is active.
- If the character does not spawn, check World Settings GameMode override and GameMode DefaultPawnClass.
- If movement does not work, check input mapping, PlayerController possession, CharacterMovement settings, and whether the current pawn is actually possessed.
- If the camera is wrong, inspect CameraComponent location, FOV, and whether Auto Activate is enabled.

## Widget HUD

Goal: create or update a Widget Blueprint HUD and verify the tree.

Recommended flow:

1. Use `unreal.knowledge_search` for Widget/UMG task context.
2. Use `unreal.widget_build_template` for common HUD layouts.
3. Use `unreal.widget_add`, `unreal.widget_set_property`, and `unreal.widget_set_slot_layout` for targeted edits.
4. Use `unreal.widget_bind_event` and `unreal.widget_bind_blueprint_variable` only when required.
5. Inspect the final tree with `unreal.widget_dump_tree`.
6. Compile/save the Widget Blueprint.
7. Use `unreal.verify_task_outcome` with widget tree evidence.

Important checks:

- Prefer named widgets and `isVariable=true` for elements the AI needs to reference later.
- Confirm parent slot type before applying layout properties.
- Keep visual hierarchy simple: root panel, named containers, named text/images/buttons.

## Blueprint Graph Edit

Goal: edit a Blueprint graph without losing track of nodes and pins.

Recommended flow:

1. Inspect first with `unreal.bp_list_graph_nodes`.
2. Add variables/functions/events/nodes using the `unreal.bp_*` graph tools.
3. Connect pins with explicit node identifiers and pin names.
4. Set literal pin defaults with `unreal.bp_set_pin_default`.
5. Arrange graph with `unreal.bp_arrange_graph`.
6. Compile/save with `unreal.bp_compile_save`.
7. Verify specific links with `unreal.bp_trace_pin_connections`.

Important checks:

- Do not assume node display names are unique.
- Always re-list graph nodes after adding nodes.
- A compile success is necessary but not sufficient; also trace key pin connections.

## Self-Extension Tool

Goal: add a new MCP tool without destabilizing the plugin.

Recommended flow:

1. Run `unreal.preview_change_plan` for the natural-language task.
2. Run `unreal.tool_gap_analyze` to decide whether to use existing tools, compose a workflow, or scaffold a new tool.
3. If a new tool is justified, run `unreal.scaffold_mcp_tool`.
4. Patch generated descriptor-first fragments with `unreal.mcp_patch_scaffold_patch`.
5. Validate patch fragments with `unreal.mcp_validate_cpp_patch`.
6. Run `unreal.mcp_apply_scaffold` with `dryRun=true`.
7. Apply with backups only after dry run is clean.
8. Close Unreal Editor, build with `unreal.mcp_build_editor`, reopen Editor, and run `unreal.mcp_run_tool_test` or `unreal.mcp_run_test_suite`.
9. Finish with `unreal.verify_task_outcome`.

Important checks:

- New C++ tools cannot hot-load into a running Editor.
- Generated patch files are independent during scaffold review, but applied tools currently merge into core plugin source.
- Keep tool schema OpenAI-compatible: object root, explicit properties, required fields when needed, and `additionalProperties=false`.
- Add at least one happy-path test and one invalid-argument test.

