// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Modulo de construccion UBT.

using UnrealBuildTool;

public class EdgePreservingPixelation : ModuleRules
{
    public EdgePreservingPixelation(ReadOnlyTargetRules Target) : base(Target)
    {
        // Uso de PCH explicito para evitar contaminacion de includes entre modulos de render.
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
