"""Read-only Unreal MCP Python smoke tool for editor Python runtime details."""

import platform
import sys

import unreal  # type: ignore[reportMissingImports]


TOOL_NAME = "unreal.editor.python_runtime_info"


def _version_info() -> dict:
    return {
        "major": sys.version_info.major,
        "minor": sys.version_info.minor,
        "micro": sys.version_info.micro,
        "releaselevel": sys.version_info.releaselevel,
        "serial": sys.version_info.serial,
    }


def _implementation_info() -> dict:
    implementation = getattr(sys, "implementation", None)
    if implementation is None:
        return {}

    version = getattr(implementation, "version", None)
    version_fields = {}
    if version is not None:
        version_fields = {
            "major": getattr(version, "major", None),
            "minor": getattr(version, "minor", None),
            "micro": getattr(version, "micro", None),
            "releaselevel": getattr(version, "releaselevel", None),
            "serial": getattr(version, "serial", None),
        }

    return {
        "name": str(getattr(implementation, "name", "")),
        "cacheTag": str(getattr(implementation, "cache_tag", "")),
        "version": version_fields,
    }


def _engine_version_info() -> dict:
    system_library = getattr(unreal, "SystemLibrary", None)
    get_engine_version = getattr(system_library, "get_engine_version", None)
    if not callable(get_engine_version):
        return {
            "available": False,
            "value": "",
            "error": "unreal.SystemLibrary.get_engine_version is unavailable",
        }

    try:
        return {
            "available": True,
            "value": str(get_engine_version()),
            "error": "",
        }
    except Exception as exc:  # Read-only smoke tool: report, do not mutate or retry.
        return {
            "available": False,
            "value": "",
            "error": f"{type(exc).__name__}: {exc}",
        }


def execute(args: dict) -> dict:
    """Return JSON-serializable Python/Unreal runtime details without mutation."""
    if not isinstance(args, dict):
        args = {}

    return {
        "isError": False,
        "toolName": TOOL_NAME,
        "summary": "Unreal Python runtime is available and the python-track bridge executed this handler.",
        "python": {
            "version": sys.version,
            "versionInfo": _version_info(),
            "implementation": _implementation_info(),
            "compiler": platform.python_compiler(),
            "build": list(platform.python_build()),
            "executable": sys.executable,
            "prefix": sys.prefix,
            "basePrefix": getattr(sys, "base_prefix", ""),
            "execPrefix": sys.exec_prefix,
            "baseExecPrefix": getattr(sys, "base_exec_prefix", ""),
            "byteOrder": sys.byteorder,
            "filesystemEncoding": sys.getfilesystemencoding(),
            "defaultEncoding": sys.getdefaultencoding(),
            "moduleCount": len(sys.modules),
            "pathCount": len(sys.path),
            "platform": platform.platform(),
        },
        "unreal": {
            "moduleImported": True,
            "moduleName": str(getattr(unreal, "__name__", "unreal")),
            "moduleFile": str(getattr(unreal, "__file__", "")),
            "apiObjectCount": len(dir(unreal)),
            "systemLibraryAvailable": hasattr(unreal, "SystemLibrary"),
            "engineVersion": _engine_version_info(),
        },
        "request": {
            "argumentKeys": sorted(str(key) for key in args.keys()),
        },
    }
