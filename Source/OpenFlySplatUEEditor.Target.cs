using UnrealBuildTool;

public class OpenFlySplatUEEditorTarget : TargetRules
{
    public OpenFlySplatUEEditorTarget(TargetInfo Target) : base(Target)
    {
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        Type = TargetType.Editor;
        ExtraModuleNames.Add("OpenFlySplatUE");
    }
}
