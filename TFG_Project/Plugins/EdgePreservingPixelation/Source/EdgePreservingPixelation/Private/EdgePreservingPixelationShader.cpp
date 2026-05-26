// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Registro del Global Shader.

#include "EdgePreservingPixelationShader.h"

// Registra FEdgePreservingPixelationCS en el sistema de tipos de shader de UE5.
//
// IMPLEMENT_GLOBAL_SHADER genera una variable estatica de tipo FShaderType que se
// inicializa al cargar el modulo (antes de main). Esta instancia estatica vincula:
//   - La clase C++ (FEdgePreservingPixelationCS)
//   - La ruta virtual del archivo .usf (/Plugin/EdgePreservingPixelation/Private/...)
//   - El punto de entrada HLSL (MainCS)
//   - El tipo de shader de hardware (SF_Compute)
//
// La ruta virtual /Plugin/EdgePreservingPixelation/Private/ es valida porque en
// StartupModule() se mapeo el directorio fisico Shaders/ del plugin a esa ruta.
IMPLEMENT_GLOBAL_SHADER(
    FEdgePreservingPixelationCS,
    "/Plugin/EdgePreservingPixelation/Private/EdgePreservingPixelation.usf",
    "MainCS",
    SF_Compute
);
