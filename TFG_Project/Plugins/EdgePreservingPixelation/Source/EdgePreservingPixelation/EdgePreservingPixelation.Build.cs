// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Modulo de construccion UBT.

using UnrealBuildTool;

public class EdgePreservingPixelation : ModuleRules
{
    public EdgePreservingPixelation(ReadOnlyTargetRules Target) : base(Target)
    {
        // Uso de PCH explicito para evitar contaminacion de includes entre modulos de render.
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Forzar el orden de includes de UE5.5 para este modulo.
        // El proyecto base usa EngineIncludeOrderVersion.Unreal5_3 (BuildSettingsVersion.V5),
        // que provoca que Engine/Engine.h (arrastrado por SceneViewExtensionContext.h) exponga
        // TOptional antes de que este completamente definido, causando un cascade de errores.
        // Restringir la correccion al modulo del plugin evita alterar el resto del proyecto.
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",

            // RenderCore: necesario para AddShaderSourceDirectoryMapping, FGlobalShader,
            // DECLARE_GLOBAL_SHADER, BEGIN_SHADER_PARAMETER_STRUCT y ShaderParameterStruct.
            "RenderCore",

            // RHI: necesario para FRHICommandListImmediate, ERHIFeatureLevel, tipos de textura.
            "RHI",

            // Renderer: necesario para acceder a scene textures (SceneDepth, Velocity)
            // a traves de FSceneTextureParameters y FSceneRenderTargets en fases posteriores.
            "Renderer",

            // Projects: necesario para IPluginManager::Get().FindPlugin() en StartupModule,
            // que resuelve la ruta fisica del directorio Shaders/ del plugin.
            "Projects",
        });
    }
}
