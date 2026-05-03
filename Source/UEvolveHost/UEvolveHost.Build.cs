// Copyright (c) UEvolve contributors.

using UnrealBuildTool;

public class UEvolveHost : ModuleRules
{
	public UEvolveHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});
	}
}
