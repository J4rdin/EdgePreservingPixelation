// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del modulo.

#include "EdgePreservingPixelationModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FEdgePreservingPixelationModule"

void FEdgePreservingPixelationModule::StartupModule()
{
    // Registra el directorio fisico Shaders/ del plugin bajo la ruta virtual
    // /Plugin/EdgePreservingPixelation en el sistema de archivos virtual del
    // compilador de shaders de UE5 (FShaderSourceDirectoryMappings).
    //
    // Sin este mapeo, la directiva #include "/Plugin/EdgePreservingPixelation/..."
    // en los archivos .usf fallaria con error de archivo no encontrado en tiempo
    // de compilacion de shaders.
    //
    // Este registro debe realizarse en StartupModule (fase PostConfigInit) antes de
    // que el ShaderCompilerManager emita trabajos de compilacion al iniciar el renderer.
    FString PluginShaderDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("EdgePreservingPixelation"))->GetBaseDir(),
        TEXT("Shaders")
    );

    AddShaderSourceDirectoryMapping(TEXT("/Plugin/EdgePreservingPixelation"), PluginShaderDir);
}

void FEdgePreservingPixelationModule::ShutdownModule()
{
    // No se requiere limpieza explicita en Fase 1.
    // En fases posteriores: liberar SceneViewExtension y desregistrar delegates del renderer.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEdgePreservingPixelationModule, EdgePreservingPixelation)
