// Copyright (c) UEvolve contributors.

using UnrealBuildTool;
using System.Collections.Generic;

public class UEvolveTarget : TargetRules
{
	public UEvolveTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UEvolveHost");
	}
}
