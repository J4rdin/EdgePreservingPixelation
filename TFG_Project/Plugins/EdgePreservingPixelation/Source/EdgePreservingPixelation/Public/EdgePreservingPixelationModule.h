// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes.
//
// Interfaz publica del modulo IModuleInterface.
// El modulo se carga en fase PostConfigInit para garantizar que los tipos de shader
// queden registrados antes de que el renderer inicialice su pipeline de compilacion.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * FEdgePreservingPixelationModule
 *
 * Punto de entrada del plugin. Responsabilidades principales:
 *  - Registrar el directorio virtual de shaders (/Plugin/EdgePreservingPixelation)
 *    en el sistema de archivos virtual del compilador de shaders de UE5.
 *  - Liberar recursos al descargar el modulo.
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
