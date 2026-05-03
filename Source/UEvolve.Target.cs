// Copyright (c) UEvolve contributors.

using UnrealBuildTool;
using System.Collections.Generic;

public class UEvolveTarget : TargetRules
{
	public UEvolveTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("UEvolveHost");
	}
}
