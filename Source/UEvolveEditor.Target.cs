// Copyright (c) UEvolve contributors.

using UnrealBuildTool;
using System.Collections.Generic;

public class UEvolveEditorTarget : TargetRules
{
	public UEvolveEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("UEvolveHost");
	}
}
