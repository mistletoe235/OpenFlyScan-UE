using UnrealBuildTool;

public class OpenFlySplatUETarget : TargetRules
{
    public OpenFlySplatUETarget(TargetInfo Target) : base(Target)
    {
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        Type = TargetType.Game;
        ExtraModuleNames.Add("OpenFlySplatUE");
    }
}
