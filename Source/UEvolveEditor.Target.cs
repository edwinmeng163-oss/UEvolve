// Copyright (c) UEvolve contributors.

using UnrealBuildTool;
using System.Collections.Generic;

public class UEvolveEditorTarget : TargetRules
{
	public UEvolveEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UEvolveHost");
	}
}
