// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes.
//
// Interfaz publica del modulo IModuleInterface.
// El modulo se carga en fase PostConfigInit para garantizar que los tipos de shader
// queden registrados antes de que el renderer inicialice su pipeline de compilacion.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FEdgePreservingPixelationViewExtension;

/**
 * FEdgePreservingPixelationModule
 *
 * Punto de entrada del plugin. Responsabilidades principales:
 *  - Registrar el directorio virtual de shaders (/Plugin/EdgePreservingPixelation).
 *  - Instanciar y mantener con vida FEdgePreservingPixelationViewExtension.
 *
 * Ciclo de vida del view extension:
 *   StartupModule   → AddShaderSourceDirectoryMapping (PostConfigInit, pre-shader-compiler).
 *   OnPostEngineInit→ FSceneViewExtensions::NewExtension (requiere GEngine inicializado).
 *   ShutdownModule  → ViewExtension.Reset() desregistra la extensión del renderer.
 *
 * La separación en dos fases es necesaria porque PostConfigInit se ejecuta antes de
 * que GEngine exista; FSceneViewExtensions::NewExtension necesita el registro de vistas
 * de GEngine para que SubscribeToPostProcessingPass sea invocado por el renderer.
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    // Callback vinculado a FCoreDelegates::OnPostEngineInit.
    // Se llama cuando GEngine está completamente inicializado y el sistema de
    // view extensions es operativo.
    void OnPostEngineInit();

    // Propietario único de la extensión de vista. El shared_ptr mantiene viva la extensión
    // durante todo el ciclo de vida del módulo; al reset(), FSceneViewExtensionBase
    // deregistra automáticamente la extensión del sistema global de vistas.
    TSharedPtr<FEdgePreservingPixelationViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
