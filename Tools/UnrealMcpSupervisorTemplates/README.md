# Unreal MCP Supervisor Templates

These are versioned, path-neutral templates for the external supervisor launchers. The real launcher files are generated locally by:

```bash
python3 Tools/unreal_mcp_supervisor.py install --platform all --output-dir Tools/UnrealMcpSupervisor --overwrite
```

Generated files are ignored by Git because they contain machine-specific absolute paths. Use these templates for review, documentation, packaging, and future installer improvements.

## Placeholders

- `{{PROJECT_ROOT}}`: absolute project checkout path.
- `{{UPROJECT}}`: absolute `.uproject` path.
- `{{SUPERVISOR}}`: absolute `Tools/unreal_mcp_supervisor.py` path.
- `{{MCP_URL}}`: MCP endpoint URL.
- `{{LOG_DIR}}`: supervisor log directory.
- `{{ARGS_JSON}}`: JSON object passed to `unreal.mcp_extension_pipeline`.
- `{{LABEL}}`: macOS LaunchAgent label.
- `{{EDITOR_CMD}}`: optional explicit UnrealEditor executable path.
