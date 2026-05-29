// Copyright (c) 2025 Manuel Rodríguez Sánchez. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del modulo.

#include "EdgePreservingPixelationModule.h"
#include "EdgePreservingPixelationViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "ShaderCore.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FEdgePreservingPixelationModule"

void FEdgePreservingPixelationModule::StartupModule()
{
    // 1. Registrar el directorio virtual de shaders del plugin.
    //    Debe ejecutarse en PostConfigInit (antes de que el shader compiler
    //    intente resolver las rutas /Plugin/EdgePreservingPixelation/...).
    FString PluginShaderDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("EdgePreservingPixelation"))->GetBaseDir(),
        TEXT("Shaders")
    );
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/EdgePreservingPixelation"), PluginShaderDir);

    // 2. Diferir la creación del view extension hasta que GEngine esté inicializado.
    //    FSceneViewExtensions::NewExtension requiere GEngine para registrar la extensión
    //    en el sistema global de vistas; GEngine no existe en la fase PostConfigInit.
    //    OnPostEngineInit dispara en FEngineLoop::Init(), tras GEngine->Init().
    FCoreDelegates::OnPostEngineInit.AddRaw(
        this, &FEdgePreservingPixelationModule::OnPostEngineInit);
}

void FEdgePreservingPixelationModule::OnPostEngineInit()
{
    // GEngine está completamente inicializado aquí.
    // FSceneViewExtensions::NewExtension registra la extensión en el registro global
    // de vistas de UE5, lo que garantiza que SubscribeToPostProcessingPass sea
    // invocado por el renderer en cada frame con una vista activa.
    ViewExtension = FSceneViewExtensions::NewExtension<FEdgePreservingPixelationViewExtension>();
}

void FEdgePreservingPixelationModule::ShutdownModule()
{
    // Desvincular el delegate por si ShutdownModule se ejecuta antes de OnPostEngineInit
    // (por ejemplo, si el módulo se descarga durante el arranque).
    FCoreDelegates::OnPostEngineInit.RemoveAll(this);

    // Liberar la extensión de vista. Al destruirse el TSharedPtr (reset a nullptr),
    // FSceneViewExtensionBase marca la extensión como inactiva en el registro global.
    // El renderer la elimina de su array interno en el siguiente ciclo de frame.
    ViewExtension.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FEdgePreservingPixelationModule, EdgePreservingPixelation)
