// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes.
//
// Declaracion del Global Shader y su estructura de parametros RDG.
// Fase 3: Deteccion de bordes geometricos — SceneDepth real + cbuffer ampliado a 4 registros.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"                // FViewUniformShaderParameters — necesario para SHADER_PARAMETER_STRUCT_REF
#include "SceneTexturesConfig.h"      // FSceneTextureUniformParameters — UB con SceneDepth real (SHADER_PARAMETER_RDG_UNIFORM_BUFFER)

/**
 * FEdgePreservingPixelationCS
 *
 * Compute Shader global para pixelacion en tiempo real con preservacion de bordes.
 *
 * Hereda de FGlobalShader: shaders globales no estan vinculados a ningun material
 * ni FMeshBatch; son recursos estaticos del renderer compilados una sola vez por
 * combinacion de plataforma y permutacion.
 *
 * El macro SHADER_USE_PARAMETER_STRUCT vincula automaticamente FParameters con el
 * sistema de reflection de UE5, permitiendo al RDG rastrear dependencias de recursos.
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FEdgePreservingPixelationCS);
    SHADER_USE_PARAMETER_STRUCT(FEdgePreservingPixelationCS, FGlobalShader);

    /**
     * FParameters
     *
     * Estructura de parametros del shader compatible con el Render Dependency Graph (RDG).
     * El RDG utiliza esta estructura para:
     *   1. Inferir dependencias entre pases de render (lectura/escritura de recursos).
     *   2. Gestionar transiciones de estado de recursos (barriers) de forma automatica.
     *   3. Emitir los bindings a la API de RHI subyacente (DX12 / Vulkan).
     *
     * -------------------------------------------------------------------------
     * ALINEACION DE 16 BYTES EN CONSTANT BUFFERS (HLSL cbuffer):
     * -------------------------------------------------------------------------
     * Las GPU modernas (DX12, Vulkan) exigen que los constant buffers esten
     * alineados a 16 bytes (tamano de un registro de GPU: float4 = xyzw).
     * El estandar HLSL de empaquetado (D3D constant buffer packing rules) prohíbe
     * que una variable cruce un limite de 16 bytes. Por ejemplo:
     *
     *   float3 a;   // ocupa bytes [0..11] del registro 0
     *   float4 b;   // NO puede empezar en byte 12 (cruzaria registro); se mueve al registro 1
     *
     * UE5 respeta estas reglas internamente mediante su sistema de layout, pero el
     * programador debe ordenar los campos y anadir padding explicito para:
     *   a) Evitar padding implicito invisible que desincronizaria C++ y HLSL.
     *   b) Documentar de forma rigurosa el layout del buffer para revision academica.
     *
     * Layout resultante de FParameters (solo escalares del cbuffer):
     *   Registro 0 [bytes  0-15] : ViewRect         (float4, 16 bytes) — alineado.
     *   Registro 1 [bytes 16-23] : InverseViewSize  (float2,  8 bytes)
     *              [bytes 24-27] : PixelSize         (float,   4 bytes)
     *              [bytes 28-31] : Padding0          (float,   4 bytes) — relleno explicito.
     *   Registro 2 [bytes 32-35] : TemporalJitterX  (float,   4 bytes)
     *              [bytes 36-39] : TemporalJitterY   (float,   4 bytes)
     *              [bytes 40-43] : FrameIndex        (float,   4 bytes)
     *              [bytes 44-47] : Padding1          (float,   4 bytes) — relleno explicito.
     *   Registro 3 [bytes 48-51] : EdgeThreshold    (float,   4 bytes) — umbral sobre puntuacion combinada.
     *              [bytes 52-55] : DebugMode         (float,   4 bytes) — 0=normal, 1=debug bordes.
     *              [bytes 56-59] : DepthWeight       (float,   4 bytes) — peso Sobel profundidad en suma ponderada.
     *              [bytes 60-63] : NormalWeight      (float,   4 bytes) — peso Sobel normal en suma ponderada.
     * Total: 64 bytes = 4 registros de 16 bytes. Sin huecos implicitos.
     * Nota: SHADER_PARAMETER_STRUCT_REF(View) es un uniform buffer independiente del
     *       cbuffer $Globals; no afecta al layout de escalares descrito arriba.
     * -------------------------------------------------------------------------
     */
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, EDGEPRESERVINGPIXELATION_API)

        // --- Recursos de entrada (SRV — Shader Resource View) ---
        // Gestionados por RDG; no forman parte del constant buffer sino del descriptor heap.

        // Color de escena HDR antes del tonemapping (formato tipico: R11G11B10_FLOAT o RGBA16F).
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D,         SceneColor)

        // Uniform buffer de escena con acceso a SceneDepthTexture, GBuffers, etc.
        // Nombre del slot HLSL: SceneTexturesStruct (convención del engine, ver SceneTexturesCommon.ush).
        // Creado con CreateSceneTextureUniformBuffer(GraphBuilder, View, ESceneTextureSetupMode::SceneDepth).
        SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)

        // Buffer de velocidad por pixel en unidades de NDC/frame (formato: R16G16_FLOAT).
        // Empleado para estabilizacion temporal (reprojection) en Fase 3.
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, SceneVelocity)

        // Sampler compartido con filtrado bilineal y modo de direccionamiento Clamp.
        SHADER_PARAMETER_SAMPLER(SamplerState,               LinearClampSampler)

        // Uniform buffer de vista de escena (cbuffer View en HLSL, declarado via Common.ush).
        // Proporciona InvDeviceZToWorldZTransform y otros parametros de camara al shader.
        // El RDG lo vincula automaticamente al slot de uniform buffer correcto.
        // En C++: Parameters->View = View.ViewUniformBuffer.
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

        // --- Recurso de salida (UAV — Unordered Access View) ---
        // El compute shader escribe el resultado pixelado directamente en este buffer.
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)

        // --- Constant buffer: Registro 0 (16 bytes) ---
        // Region activa del viewport en pixeles de pantalla: [OffsetX, OffsetY, Width, Height].
        // Necesario para descartar hilos fuera del viewport en el compute shader.
        SHADER_PARAMETER(FVector4f, ViewRect)

        // --- Constant buffer: Registro 1 (16 bytes) ---
        // Inversa del tamano del render target completo: (1/RTWidth, 1/RTHeight).
        // Convierte coordenadas de pixel a UV normalizadas [0,1] sobre el render target.
        SHADER_PARAMETER(FVector2f, InverseViewSize)
        // Tamano del bloque de pixelacion en pixeles de pantalla (ej. 4.0 = bloques 4x4).
        SHADER_PARAMETER(float,     PixelSize)
        // Relleno explicito: completa el Registro 1 a 16 bytes. Sin semantica en HLSL.
        SHADER_PARAMETER(float,     Padding0)

        // --- Constant buffer: Registro 2 (16 bytes) ---
        // Activacion de la compensacion de jitter TAA en el detector Sobel.
        //   1.0 = activo: los muestreos de SceneDepth y GBufferA se desplazan por
        //         View.TemporalAAJitter para leer la misma posicion geometrica cada frame.
        //   0.0 = desactivado: comportamiento sin compensacion (ablacion A/B).
        // CVar: EPP.JitterCompensation.
        SHADER_PARAMETER(float,     JitterCompensation)
        // Reservado (antes TemporalJitterY). Sin semantica activa.
        SHADER_PARAMETER(float,     TemporalJitterY)
        // Reservado (antes FrameIndex). Sin semantica activa.
        SHADER_PARAMETER(float,     FrameIndex)
        // Relleno explicito: completa el Registro 2 a 16 bytes. Sin semantica en HLSL.
        SHADER_PARAMETER(float,     Padding1)

        // --- Constant buffer: Registro 3 (16 bytes) — introducido en Fase 3 ---
        // Umbral aplicado sobre la puntuacion de borde combinada:
        //   CombinedScore = DepthWeight * GradienteDepthRelativo + NormalWeight * GradienteNormal
        // Un borde en cualquiera de las dos senales puede superar el umbral; una superficie
        // plana oblicua (debil en ambas senales tras normalizar) se descarta.
        // CVar: EPP.EdgeThreshold.
        SHADER_PARAMETER(float,     EdgeThreshold)
        // Selector de modo de salida del shader:
        //   0.0 = pixelacion normal (sin debug).
        //   1.0 = debug: bordes detectados en blanco sobre negro.
        // CVar: EPP.DebugMode.
        SHADER_PARAMETER(float,     DebugMode)
        // Peso del gradiente Sobel de profundidad relativa en la puntuacion combinada.
        // Escala el termino adimensional SobelDepth/LinearDepth.
        // CVar: EPP.DepthWeight. Valor por defecto: 1.0.
        SHADER_PARAMETER(float,     DepthWeight)
        // Peso del gradiente Sobel de normal del GBuffer en la puntuacion combinada.
        // Escala el termino de magnitud de gradiente en espacio de normal codificada [0,1].
        // CVar: EPP.NormalWeight. Valor por defecto: 1.0.
        SHADER_PARAMETER(float,     NormalWeight)

    END_SHADER_PARAMETER_STRUCT()

    /**
     * ShouldCompilePermutation
     * Restringe la compilacion del shader a plataformas con Shader Model 5 o superior,
     * requisito minimo para compute shaders con acceso a UAV y texturas de profundidad.
     */
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    /**
     * ModifyCompilationEnvironment
     * Inyecta defines de preprocesador en el entorno de compilacion del shader.
     * El tamano del grupo de hilos (8x8=64 hilos) es optimo para acceso coalescente
     * a memoria en GPU con warp/wavefront de 32 o 64 hilos.
     */
    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
    }
};

/**
 * FEdgePreservingPixelationTemporalCS
 *
 * Compute Shader para la estabilizacion temporal mediante EMA (Fase 4 / Paso 1).
 *
 * Combina el frame actual pixelado con el historial acumulado del frame anterior
 * mediante una media exponencial movil:
 *   output = lerp(HistoryTexture, CurrentTexture, TemporalAlpha)
 *
 * El ping-pong de buffers de historia (dos TRefCountPtr<IPooledRenderTarget>)
 * es gestionado en FEdgePreservingPixelationViewExtension y persiste entre frames
 * mediante GraphBuilder.QueueTextureExtraction().
 *
 * Paso 1 (EMA ingenua): sin reprojeccion con SceneVelocity, sin neighborhood clamping.
 * El ghosting severo en camara en movimiento es esperado y diagnostico en este paso.
 * Los Pasos 2 y 3 anadiran reprojeccion y YCoCg clamping respectivamente.
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationTemporalCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FEdgePreservingPixelationTemporalCS);
    SHADER_USE_PARAMETER_STRUCT(FEdgePreservingPixelationTemporalCS, FGlobalShader);

    /**
     * FParameters
     *
     * Layout del constant buffer (solo escalares — recursos en descriptor heap aparte):
     *   Registro 0 [bytes  0-15]: ViewRect        (float4, 16 bytes)
     *   Registro 1 [bytes 16-23]: InverseViewSize (float2,  8 bytes)
     *              [bytes 24-27]: TemporalAlpha   (float,   4 bytes)
     *              [bytes 28-31]: Padding_T0      (float,   4 bytes) — relleno explicito
     * Total: 32 bytes = 2 registros de 16 bytes. Sin huecos implicitos.
     */
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, EDGEPRESERVINGPIXELATION_API)

        // Frame actual pixelado (salida del pase de pixelacion principal).
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D,          CurrentTexture)
        // Historial EMA del frame anterior (buffer ping-pong extraido con QueueTextureExtraction).
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D,          HistoryTexture)
        // SceneTextures UB — contiene GBufferVelocityTexture para la reprojeccion temporal (Paso 2).
        // Se solicita con ESceneTextureSetupMode::SceneDepth|GBufferA|SceneVelocity; el UB es compartido
        // con el pase de pixelacion principal (misma TRDGUniformBufferRef, sin doble creacion).
        SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
        // Sampler bilineal con clamp, compartido con el pase principal.
        SHADER_PARAMETER_SAMPLER(SamplerState,                LinearClampSampler)
        // Salida EMA: nueva imagen visible + nueva historia para el frame siguiente.
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
        // View UB — requerido porque Common.ush (incluido por SceneTexturesCommon.ush para
        // DecodeVelocityFromTexture) declara cbuffer View. Sin este binding el sistema de
        // parametros del engine no asigna correctamente el slot de $Globals, lo que hace
        // que TemporalAlpha sea ignorado por el shader (lee un valor fijo de memoria no inicializada).
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

        // --- Constant buffer: Registro 0 ---
        SHADER_PARAMETER(FVector4f, ViewRect)

        // --- Constant buffer: Registro 1 ---
        SHADER_PARAMETER(FVector2f, InverseViewSize)
        SHADER_PARAMETER(float,     TemporalAlpha)
        SHADER_PARAMETER(float,     Padding_T0)

    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), 8);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), 8);
    }
};
