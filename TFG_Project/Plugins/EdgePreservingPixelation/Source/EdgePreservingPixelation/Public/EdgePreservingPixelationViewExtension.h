// Copyright (c) 2025 Manuel Rodríguez Sánchez. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes.
//
// FSceneViewExtensionBase derivada que inyecta el pase de compute de pixelacion
// en el pipeline de post-proceso de UE5 vía SubscribeToPostProcessingPass.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderTargetPool.h"    // IPooledRenderTarget, TRefCountPtr<IPooledRenderTarget>

// Declaraciones anticipadas: evita incluir ScreenPass.h (Renderer) en un header
// público, lo que arrastraría CanvasTypes.h y corrompería el orden del PCH.
// Las definiciones completas se incluyen únicamente en el .cpp.
struct FScreenPassTexture;
struct FPostProcessMaterialInputs;

/// <summary>
/// Class: FEdgePreservingPixelationViewExtension
/// Description: Extensión de vista que inyecta el pase de compute de pixelación tras el Tonemap del pipeline de post-procesado.
/// Author: Manuel Rodríguez Sánchez
/// Project: TFG - Plugin de pixelación con preservación de bordes
/// </summary>
/**
 * FEdgePreservingPixelationViewExtension
 *
 * Extensión de vista que inyecta un pase de compute shader en el pipeline de
 * post-procesado de UE5 5.5 mediante el mecanismo de callback de FSceneViewExtensionBase.
 *
 * PUNTO DE INYECCION: EPostProcessingPass::Tonemap (posterior al tone-mapping).
 *
 * Justificación del punto de inyección (relevante para el TFG):
 *   1. LDR post-tonemap: el color está en [0,1] tras la curva de respuesta del tono.
 *      Operar en LDR evita que valores HDR extremos produzcan artefactos de color
 *      en los bordes de bloque de la cuantización UV.
 *   2. Posterior a TAA/TSR: el super-sampling temporal ya ha aportado estabilidad
 *      sub-pixel a la imagen base, reduciendo el parpadeo del bloque sin coste adicional.
 *   3. Anterior a FXAA/post-sharpening: FXAA suavizaría los bordes duros de los
 *      bloques de pixelación, destruyendo la estética del efecto. Inyectando antes,
 *      nos aseguramos de que nuestra lógica de preservación de bordes sea la última
 *      etapa de anti-aliasing que actúa sobre la imagen.
 *   4. SceneDepth y SceneVelocity permanecen válidos en este punto del frame,
 *      lo que habilitará la detección de bordes y la estabilización temporal en
 *      las fases posteriores sin necesidad de cambiar el punto de inyección.
 *
 * Ciclo de vida: la instancia es mantenida por un TSharedPtr en FEdgePreservingPixelationModule.
 * FSceneViewExtensionBase la registra automáticamente en su constructor y la elimina
 * del sistema de extensiones cuando el TSharedPtr es destruido en ShutdownModule().
 */
class EDGEPRESERVINGPIXELATION_API FEdgePreservingPixelationViewExtension
    : public FSceneViewExtensionBase
{
public:
    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Inicializa la extensión y la registra en el sistema de vistas de UE5 mediante FAutoRegister.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /** @brief Inicializa la extensión y la registra automáticamente en el sistema de vistas de UE5 mediante FAutoRegister. */
    FEdgePreservingPixelationViewExtension(const FAutoRegister& AutoRegister);

    // ISceneViewExtension — implementaciones vacías requeridas por la interfaz.
    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Sin uso; implementación vacía requerida por ISceneViewExtension.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /** @brief Sin uso; implementación vacía requerida por ISceneViewExtension. */
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Sin uso; implementación vacía requerida por ISceneViewExtension.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /** @brief Sin uso; implementación vacía requerida por ISceneViewExtension. */
    virtual void SetupView(FSceneViewFamily& ViewFamily, FSceneView& View) override {}
    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Sin uso; implementación vacía requerida por ISceneViewExtension.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /** @brief Sin uso; implementación vacía requerida por ISceneViewExtension. */
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Registra el callback de pixelación para el pase Tonemap del post-procesado de UE5.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /**
     * SubscribeToPostProcessingPass
     *
     * Llamado por el renderer en el render thread una vez por frame, una vez por
     * pase de post-proceso existente en el grafo. Registramos el callback únicamente
     * cuando PassId == EPostProcessingPass::Tonemap.
     *
     * El array InOutPassCallbacks acumula todos los callbacks de todas las extensiones
     * para ese pase; el renderer los ejecuta secuencialmente encadenando la salida de
     * uno como entrada del siguiente.
     */
    // Firma actualizada a UE5.5: se añade const FSceneView& InView como segundo parámetro.
    // La firma anterior (sin InView) está marcada como UE_DEPRECATED(5.5) en SceneViewExtension.h.
    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
        const FSceneView& InView,
        FAfterPassCallbackDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

private:
    /// <summary>
    /// Class: FEdgePreservingPixelationViewExtension
    /// Description: Callback del render thread que despacha el compute shader de pixelación y el pase EMA temporal vía RDG.
    /// Author: Manuel Rodríguez Sánchez
    /// Project: TFG - Plugin de pixelación con preservación de bordes
    /// </summary>
    /**
     * PostProcessPass_RenderThread
     *
     * Callback ejecutado en el render thread tras el pase Tonemap.
     * Recibe la textura de color post-tonemapeada, despacha el compute shader de
     * pixelación vía RDG y devuelve la nueva textura de color como FScreenPassTexture.
     *
     * El RDG infiere la dependencia OutputTexture(Write) → SceneColor(Read) y emite
     * la barrera de recurso correspondiente antes del dispatch.
     */
    FScreenPassTexture PostProcessPass_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessMaterialInputs& InOutInputs);

    // -----------------------------------------------------------------------
    // Ping-pong de buffers de historia — Fase 4 (estabilizacion temporal EMA)
    //
    // HistoryRT[HistoryReadIndex]   : historia del frame anterior (lectura SRV).
    // HistoryRT[1-HistoryReadIndex] : nueva historia del frame actual (escritura UAV).
    //
    // La extraccion via GraphBuilder.QueueTextureExtraction() persiste los buffers
    // fuera del grafo de render, garantizando que sobreviven entre frames.
    // El intercambio de indices (swap del ping-pong) ocurre en PostProcessPass_RenderThread
    // despues de llamar a QueueTextureExtraction, para que el frame siguiente sepa
    // de que buffer leer.
    //
    // bHistoryValid:  false en el primer frame y tras cambios de resolucion/formato.
    // HistorySize:    resolucion de los buffers actuales — invalida si cambia.
    // HistoryFormat:  formato de pixel de los buffers — invalida si cambia (puede ocurrir
    //                 si bHasOverride cambia entre frames, p.ej. al entrar/salir de PIE).
    //
    // NOTA: la implementacion actual asume un solo viewport activo. Con multiples vistas
    // (split-screen) el historial se compartiria incorrectamente; se puede extender
    // indexando por FSceneViewState si fuera necesario en el futuro.
    // -----------------------------------------------------------------------
    TRefCountPtr<IPooledRenderTarget> HistoryRT[2];      ///< Par de buffers de historia ping-pong; HistoryRT[HistoryReadIndex] contiene el frame anterior.
    int32        HistoryReadIndex = 0;                   ///< Índice del buffer de historia de lectura (0 ó 1); alterna en cada frame tras QueueTextureExtraction.
    bool         bHistoryValid    = false;               ///< Indica si el historial contiene datos válidos del frame anterior; false en el primer frame.
    FIntPoint    HistorySize      = FIntPoint::ZeroValue; ///< Resolución de los buffers de historia; un cambio invalida el historial.
    EPixelFormat HistoryFormat    = PF_Unknown;          ///< Formato de píxel de los buffers de historia; un cambio invalida el historial.
};
