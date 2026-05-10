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
			"Projects",
			"PythonScriptPlugin",
			"Settings",
			"Slate",
			"SlateCore",
			"Sockets",
			"ToolMenus",
			"UMG",
			"UMGEditor",
			"UnrealEd",
			"WebSockets"
		});
	}
}
