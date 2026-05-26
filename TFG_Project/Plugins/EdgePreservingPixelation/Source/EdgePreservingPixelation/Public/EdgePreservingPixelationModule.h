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
 *  - Instanciar y mantener con vida FEdgePreservingPixelationViewExtension, que es
 *    el único propietario del punto de inyección en el pipeline de post-proceso.
 *    Al destruir el TSharedPtr en ShutdownModule, FSceneViewExtensionBase
 *    deregistra automáticamente la extensión del sistema global de vistas.
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    // Propietario único de la extensión de vista. El shared_ptr mantiene viva la extensión
    // durante todo el ciclo de vida del módulo; al reset(), el renderer la elimina del
    // array interno de extensiones activas en el siguiente frame.
    TSharedPtr<FEdgePreservingPixelationViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
