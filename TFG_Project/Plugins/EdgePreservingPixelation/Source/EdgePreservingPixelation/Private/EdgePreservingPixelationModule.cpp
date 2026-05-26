// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del modulo.

#include "EdgePreservingPixelationModule.h"
#include "EdgePreservingPixelationViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FEdgePreservingPixelationModule"

void FEdgePreservingPixelationModule::StartupModule()
{
    // 1. Registrar el directorio virtual de shaders del plugin.
    //    Obligatorio para que el compilador de shaders resuelva las rutas
    //    /Plugin/EdgePreservingPixelation/... en los archivos .usf.
    FString PluginShaderDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("EdgePreservingPixelation"))->GetBaseDir(),
        TEXT("Shaders")
    );
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/EdgePreservingPixelation"), PluginShaderDir);

    // 2. Instanciar la extensión de vista que inyecta el pase de compute en el
    //    pipeline de post-proceso. FSceneViewExtensions::NewExtension crea la instancia,
    //    la registra en el sistema global de extensiones de vista de UE5 y devuelve un
    //    TSharedRef que se almacena aquí para mantener la extensión viva.
    //
    //    La extensión solo se activa durante el render; no tiene coste en frames
    //    en los que no hay ninguna vista activa.
    ViewExtension = FSceneViewExtensions::NewExtension<FEdgePreservingPixelationViewExtension>();
}

void FEdgePreservingPixelationModule::ShutdownModule()
{
    // Liberar la extensión de vista. Al destruirse el TSharedPtr (reset a nullptr),
    // FSceneViewExtensionBase marca la extensión como inactiva en el registro global.
    // El renderer la elimina de su array interno en el siguiente ciclo de frame.
    ViewExtension.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEdgePreservingPixelationModule, EdgePreservingPixelation)
