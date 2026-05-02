using UnrealBuildTool;

public class UnrealMcp : ModuleRules
{
	public UnrealMcp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AIModule",
			"ApplicationCore",
			"AssetRegistry",
			"BlueprintGraph",
			"ContentBrowser",
			"CoreUObject",
			"DeveloperSettings",
			"Engine",
			"HTTP",
			"HTTPServer",
			"InputCore",
			"Json",
			"JsonUtilities",
			"Kismet",
			"KismetCompiler",
			"PythonScriptPlugin",
			"Settings",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UMG",
			"UMGEditor",
			"UnrealEd"
		});
	}
}
