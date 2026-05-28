// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del View Extension.

#include "EdgePreservingPixelationViewExtension.h"
#include "EdgePreservingPixelationShader.h"

#include "ScreenPass.h"                              // FScreenPassTexture — incluido aquí, no en el .h público
#include "PostProcess/PostProcessMaterialInputs.h"  // FPostProcessMaterialInputs, EPostProcessMaterialInput
#include "RenderGraphUtils.h"                        // FComputeShaderUtils::AddPass, AddClearUAVPass, AddCopyTexturePass
#include "RenderGraphBuilder.h"                      // FRDGBuilder
#include "SceneView.h"                               // FSceneView
#include "GlobalShader.h"                            // GetGlobalShaderMap
#include "SceneRenderTargetParameters.h"  // CreateSceneTextureUniformBuffer, ESceneTextureSetupMode

// Variable de consola para controlar el tamaño del bloque de pixelación en tiempo de ejecución.
// ECVF_RenderThreadSafe permite leer el valor de forma segura desde el render thread.
// Uso en consola: EPP.PixelSize 4  (o 8, 16, etc.)
static TAutoConsoleVariable<float> CVarEPPPixelSize(
    TEXT("EPP.PixelSize"),
    8.0f,
    TEXT("Tamanno del bloque de pixelacion en pixeles de pantalla. Minimo efectivo: 2."),
    ECVF_RenderThreadSafe
);

// Umbral de discontinuidad relativa de profundidad para el operador Sobel.
// Se compara contra (GradienteSobel / ProfundidadLinealCentral), valor adimensional
// e invariante a la distancia de camara. Rango util: [0.01, 0.5].
// Uso: EPP.EdgeThreshold 0.05
static TAutoConsoleVariable<float> CVarEPPEdgeThreshold(
    TEXT("EPP.EdgeThreshold"),
    0.05f,
    TEXT("Umbral aplicado sobre la puntuacion combinada de borde: "
         "DepthWeight*GradDepthRelativo + NormalWeight*GradNormal > EdgeThreshold => borde. "
         "Rango util [0.02, 0.5]: valores bajos detectan bordes suaves, altos solo geometria prominente."),
    ECVF_RenderThreadSafe
);

// Modo de debug del detector de bordes.
// 0 = salida normal (pixelacion); 1 = bordes en blanco sobre negro.
// Uso: EPP.DebugMode 1
static TAutoConsoleVariable<int32> CVarEPPDebugMode(
    TEXT("EPP.DebugMode"),
    0,
    TEXT("Modo de debug del detector de bordes. "
         "0 = pixelacion normal. "
         "1 = bordes detectados en blanco sobre negro (visualizacion Sobel aislada)."),
    ECVF_RenderThreadSafe
);

// Peso del gradiente Sobel de profundidad relativa en la puntuacion combinada de borde.
// La puntuacion combinada es: DepthWeight * GradDepth + NormalWeight * GradNormal.
// Un borde visible en cualquiera de las dos senales puede superar EdgeThreshold;
// una superficie plana oblicua (debil en ambas) se descarta.
// Uso: EPP.DepthWeight 1.0
static TAutoConsoleVariable<float> CVarEPPDepthWeight(
    TEXT("EPP.DepthWeight"),
    1.0f,
    TEXT("Peso del gradiente Sobel de profundidad relativa en la puntuacion combinada de borde. "
         "Valor 0 desactiva la contribucion de profundidad. Valor por defecto: 1.0."),
    ECVF_RenderThreadSafe
);

// Peso del gradiente Sobel de normal del GBuffer (GBufferA) en la puntuacion combinada.
// Permite detectar esquinas y cambios de superficie sin discontinuidad de profundidad.
// Uso: EPP.NormalWeight 1.0
static TAutoConsoleVariable<float> CVarEPPNormalWeight(
    TEXT("EPP.NormalWeight"),
    1.0f,
    TEXT("Peso del gradiente Sobel de normal del GBuffer en la puntuacion combinada de borde. "
         "Valor 0 desactiva la contribucion de normal. Valor por defecto: 1.0."),
    ECVF_RenderThreadSafe
);

// Activacion del pase de estabilizacion temporal EMA (Fase 4).
// 0 = desactivado (pixelacion pura, Fases 1-3b — util para comparar en ablacion).
// 1 = activo (Paso 1: EMA ingenua sin reprojeccion).
// Uso: EPP.EnableTemporal 0
static TAutoConsoleVariable<int32> CVarEPPEnableTemporal(
    TEXT("EPP.EnableTemporal"),
    1,
    TEXT("Activa la estabilizacion temporal EMA (Fase 4). "
         "0 = desactivado (pixelacion pura, para ablacion). "
         "1 = activo (EMA sin reprojeccion, Paso 1)."),
    ECVF_RenderThreadSafe
);

// Factor de mezcla de la EMA temporal.
// output = lerp(historia, actual, TemporalAlpha).
// Valores bajos: historia domina, mayor estabilidad pero ghosting mas largo.
// Valores altos: actual domina, ghosting breve pero menor reduccion de parpadeo.
// Uso: EPP.TemporalAlpha 0.1
static TAutoConsoleVariable<float> CVarEPPTemporalAlpha(
    TEXT("EPP.TemporalAlpha"),
    0.1f,
    TEXT("Factor de mezcla EMA temporal: output = lerp(historia, actual, alpha). "
         "1.0 = solo frame actual (sin historia). "
         "0.0 = historia pura (imagen congelada). "
         "Defecto: 0.1 (90% historia, 10% actual — constante de tiempo ~10 frames)."),
    ECVF_RenderThreadSafe
);

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

FEdgePreservingPixelationViewExtension::FEdgePreservingPixelationViewExtension(
    const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

// ---------------------------------------------------------------------------
// SubscribeToPostProcessingPass
// ---------------------------------------------------------------------------

void FEdgePreservingPixelationViewExtension::SubscribeToPostProcessingPass(
    EPostProcessingPass Pass,
    const FSceneView& InView,
    FAfterPassCallbackDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    if (Pass == EPostProcessingPass::Tonemap)
    {
        InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
            this,
            &FEdgePreservingPixelationViewExtension::PostProcessPass_RenderThread
        ));
    }
}

// ---------------------------------------------------------------------------
// PostProcessPass_RenderThread
// ---------------------------------------------------------------------------

FScreenPassTexture FEdgePreservingPixelationViewExtension::PostProcessPass_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& InOutInputs)
{
    check(IsInRenderingThread());

    // Obtener la textura de color producida por el pase Tonemap (LDR, formato sRGB o similar).
    const FScreenPassTexture InputSceneColor(InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor));
    if (!InputSceneColor.IsValid())
    {
        return InputSceneColor;
    }

    // Obtener el shader del mapa de shaders globales para el feature level de esta vista.
    // Sin permutaciones activas en Fase 2 — se usa el índice de permutación 0 implícito.
    TShaderMapRef<FEdgePreservingPixelationCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));

    // Guardia: si el shader no está compilado (p. ej. plataforma no soportada), pasar sin efecto.
    if (!ComputeShader.IsValid())
    {
        return InputSceneColor;
    }

    // -----------------------------------------------------------------------
    // Textura de salida intermedia
    //
    // El engine puede pre-asignar una textura de destino final (OverrideOutput)
    // a la que debemos escribir nuestro resultado. D3D12 CopyResource exige que
    // origen y destino tengan formato DXGI y dimensiones idénticos; si usáramos
    // el formato de InputSceneColor (que puede diferir, p. ej. R10G10B10A2 vs
    // B8G8R8A8), CopyResource fallaría con E_INVALIDARG.
    //
    // Solución: cuando OverrideOutput es válido, crear OutputTexture con su mismo
    // formato y extensión para que la copia posterior no tenga incompatibilidades.
    // -----------------------------------------------------------------------
    const FIntPoint RTExtent = InputSceneColor.Texture->Desc.Extent;

    const bool bHasOverride = InOutInputs.OverrideOutput.IsValid();
    const EPixelFormat OutputPixelFormat = bHasOverride
        ? InOutInputs.OverrideOutput.Texture->Desc.Format
        : InputSceneColor.Texture->Desc.Format;
    const FIntPoint OutputExtent = bHasOverride
        ? InOutInputs.OverrideOutput.Texture->Desc.Extent
        : RTExtent;

    const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
        OutputExtent,
        OutputPixelFormat,
        FClearValueBinding::Black,
        TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable
    );
    FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("EPP_PixelatedOutput"));

    // Velocity sigue como placeholder hasta Fase 4 (estabilización temporal).
    FRDGTextureRef PlaceholderVelocity = GraphBuilder.CreateTexture(
        FRDGTextureDesc::Create2D(
            FIntPoint(1, 1),
            PF_G16R16F,
            FClearValueBinding::Black,
            TexCreate_ShaderResource | TexCreate_UAV),
        TEXT("EPP_PlaceholderVelocity")
    );
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(PlaceholderVelocity), FLinearColor::Transparent);

    // -----------------------------------------------------------------------
    // Parámetros del shader
    // -----------------------------------------------------------------------
    // UnscaledViewRect: viewport en píxeles de pantalla antes de cualquier escala DPI.
    // FSceneView no expone ViewRect directamente en UE5.5; el miembro correcto es UnscaledViewRect.
    const FIntRect ViewRect     = View.UnscaledViewRect;
    const float    PixelSizeVal = FMath::Max(1.0f, CVarEPPPixelSize.GetValueOnRenderThread());

    FEdgePreservingPixelationCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FEdgePreservingPixelationCS::FParameters>();

    // View uniform buffer — proporciona InvDeviceZToWorldZTransform al HLSL (vía Common.ush)
    // para linealizar profundidad correctamente con reversed-Z y far plane infinito de UE5.
    Parameters->View = View.ViewUniformBuffer;

    // SceneTextures UB — contiene SceneDepthTexture, GBufferATexture y GBufferVelocityTexture.
    // Se almacena en una variable local para reutilizarlo tanto en el pase de pixelacion
    // principal (SceneDepth + GBufferA para el Sobel) como en el pase temporal (Velocity
    // para la reprojeccion del Paso 2). Crear el UB dos veces no seria incorrecto pero
    // generaria trabajo redundante; compartir la misma TRDGUniformBufferRef es lo correcto.
    // El RDG gestiona las barreras (DepthWrite→SRV, GBufferWrite→SRV) automaticamente.
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexUniformBuffer =
        CreateSceneTextureUniformBuffer(
            GraphBuilder, View,
            ESceneTextureSetupMode::SceneDepth | ESceneTextureSetupMode::GBufferA | ESceneTextureSetupMode::SceneVelocity);
    Parameters->SceneTexturesStruct = SceneTexUniformBuffer;

    // SRVs de entrada
    Parameters->SceneColor    = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(InputSceneColor.Texture));
    Parameters->SceneVelocity = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(PlaceholderVelocity));

    // Sampler bilineal con clamp en todos los ejes, compartido por todos los accesos del shader.
    Parameters->LinearClampSampler =
        TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

    // UAV de salida
    Parameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);

    // Constant buffer — Registro 0
    Parameters->ViewRect = FVector4f(
        static_cast<float>(ViewRect.Min.X),
        static_cast<float>(ViewRect.Min.Y),
        static_cast<float>(ViewRect.Width()),
        static_cast<float>(ViewRect.Height())
    );

    // Constant buffer — Registro 1
    // InverseViewSize normaliza sobre el render target completo (no solo el viewport).
    Parameters->InverseViewSize = FVector2f(
        1.0f / FMath::Max(1, RTExtent.X),
        1.0f / FMath::Max(1, RTExtent.Y)
    );
    Parameters->PixelSize = PixelSizeVal;
    Parameters->Padding0  = 0.0f;

    // Constant buffer — Registro 2 (reservado para Fase 4)
    Parameters->TemporalJitterX = 0.0f;
    Parameters->TemporalJitterY = 0.0f;
    Parameters->FrameIndex      = 0.0f;
    Parameters->Padding1        = 0.0f;

    // Constant buffer — Registro 3 (Fase 3: deteccion de bordes Sobel combinado)
    // EdgeThreshold se aplica sobre la puntuacion combinada:
    //   CombinedScore = DepthWeight * GradDepthRelativo + NormalWeight * GradNormal
    // Un borde fuerte en cualquiera de las dos senales puede superar el umbral.
    Parameters->EdgeThreshold  = FMath::Max(0.0f, CVarEPPEdgeThreshold.GetValueOnRenderThread());
    Parameters->DebugMode      = static_cast<float>(CVarEPPDebugMode.GetValueOnRenderThread());
    Parameters->DepthWeight    = FMath::Max(0.0f, CVarEPPDepthWeight.GetValueOnRenderThread());
    Parameters->NormalWeight   = FMath::Max(0.0f, CVarEPPNormalWeight.GetValueOnRenderThread());

    // -----------------------------------------------------------------------
    // Despacho del compute shader vía RDG
    //
    // GetGroupCount calcula el número de grupos en X e Y redondeando hacia arriba
    // al múltiplo del tamaño de grupo (8×8), garantizando cobertura completa del
    // viewport aunque su resolución no sea múltiplo de 8.
    //
    // Los hilos fuera del viewport son descartados en el shader mediante el test
    // sobre ViewRect (ver MainCS en EdgePreservingPixelation.usf).
    // -----------------------------------------------------------------------
    const FIntPoint ViewportSize(ViewRect.Width(), ViewRect.Height());

    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("EdgePreservingPixelation_Pixelate (%.0fpx)", PixelSizeVal),
        ERDGPassFlags::Compute,
        ComputeShader,
        Parameters,
        FComputeShaderUtils::GetGroupCount(ViewportSize, FIntPoint(8, 8))
    );

    // -----------------------------------------------------------------------
    // FASE 4 — Estabilizacion temporal EMA (Paso 1: sin reprojeccion)
    //
    // FinalTexture es la textura que se presentara al engine (y se copiara a
    // OverrideOutput si bHasOverride). Por defecto es OutputTexture (sin temporal);
    // si el pase temporal se ejecuta, pasa a ser TemporalOutput.
    //
    // Ping-pong de historia:
    //   HistoryReadIndex     → buffer del frame anterior (SRV de lectura).
    //   1 - HistoryReadIndex → buffer del frame actual (extraido con QueueTextureExtraction).
    //
    // HistoryRT[WriteIndex] se puebla en este frame y sera el HistoryReadIndex del siguiente.
    // QueueTextureExtraction ejecuta la copia fuera del RDG, cuando el frame termina,
    // garantizando que la historia sobrevive entre frames sin depender del pool transitorio.
    // -----------------------------------------------------------------------
    FRDGTextureRef FinalTexture = OutputTexture;

    const bool  bTemporalEnabled = (CVarEPPEnableTemporal.GetValueOnRenderThread() != 0);
    const float TemporalAlphaVal = FMath::Clamp(CVarEPPTemporalAlpha.GetValueOnRenderThread(), 0.0f, 1.0f);

    if (bTemporalEnabled)
    {
        // Verificar compatibilidad del historial: tamano y formato deben coincidir con
        // el frame actual. Si no coinciden (primer frame, cambio de resolucion o de
        // formato por cambio en bHasOverride), se descarta el historial y se reinicia.
        const bool bHistoryCompatible = bHistoryValid
            && (HistorySize   == OutputExtent)
            && (HistoryFormat == OutputPixelFormat);

        if (!bHistoryCompatible)
        {
            HistoryRT[0].SafeRelease();
            HistoryRT[1].SafeRelease();
            bHistoryValid = false;
        }

        const int32 WriteIndex = 1 - HistoryReadIndex;

        // Textura de salida del pase temporal: misma especificacion que OutputTexture
        // para que AddCopyTexturePass a OverrideOutput (al final) sea valido (E_INVALIDARG).
        const FRDGTextureDesc TemporalDesc = FRDGTextureDesc::Create2D(
            OutputExtent, OutputPixelFormat, FClearValueBinding::Black,
            TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable);
        FRDGTextureRef TemporalOutput = GraphBuilder.CreateTexture(
            TemporalDesc, TEXT("EPP_TemporalOutput"));

        if (bHistoryValid)
        {
            // Historia valida: ejecutar el pase EMA.
            FRDGTextureRef HistoryReadRDG = GraphBuilder.RegisterExternalTexture(
                HistoryRT[HistoryReadIndex], TEXT("EPP_HistoryRead"));

            TShaderMapRef<FEdgePreservingPixelationTemporalCS> TemporalShader(
                GetGlobalShaderMap(View.GetFeatureLevel()));

            FEdgePreservingPixelationTemporalCS::FParameters* TemporalParams =
                GraphBuilder.AllocParameters<FEdgePreservingPixelationTemporalCS::FParameters>();

            TemporalParams->CurrentTexture     = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(OutputTexture));
            TemporalParams->HistoryTexture     = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(HistoryReadRDG));
            // SceneTexturesStruct reutilizado del pase principal — contiene GBufferVelocityTexture
            // para la reprojeccion temporal. El UB fue creado con ESceneTextureSetupMode::SceneVelocity.
            TemporalParams->SceneTexturesStruct = SceneTexUniformBuffer;
            // View UB — necesario para que el sistema de parametros asigne $Globals correctamente;
            // Common.ush declara cbuffer View y sin este binding el slot de TemporalAlpha se pierde.
            TemporalParams->View               = View.ViewUniformBuffer;
            TemporalParams->LinearClampSampler =
                TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            TemporalParams->OutputTexture      = GraphBuilder.CreateUAV(TemporalOutput);

            TemporalParams->ViewRect = FVector4f(
                static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y),
                static_cast<float>(ViewRect.Width()), static_cast<float>(ViewRect.Height()));
            // InverseViewSize normaliza sobre el render target completo (mismo convenio que el pase principal).
            TemporalParams->InverseViewSize = FVector2f(
                1.0f / FMath::Max(1, RTExtent.X),
                1.0f / FMath::Max(1, RTExtent.Y));
            TemporalParams->TemporalAlpha = TemporalAlphaVal;
            TemporalParams->Padding_T0    = 0.0f;

            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("EdgePreservingPixelation_TemporalEMA (alpha=%.2f)", TemporalAlphaVal),
                ERDGPassFlags::Compute,
                TemporalShader,
                TemporalParams,
                FComputeShaderUtils::GetGroupCount(ViewportSize, FIntPoint(8, 8)));

            FinalTexture = TemporalOutput;
        }
        else
        {
            // Primer frame o historial invalido: inicializar la historia con el frame actual
            // sin aplicar mezcla EMA (para evitar que el primer frame aparezca atenuado
            // o distorsionado por un historial no inicializado).
            // El frame visible es OutputTexture sin cambios; TemporalOutput se usa solo
            // como portador de la historia inicial (se copia OutputTexture en el).
            AddCopyTexturePass(GraphBuilder, OutputTexture, TemporalOutput);
            // FinalTexture permanece como OutputTexture (sin mezcla este frame).
        }

        // Persistir la salida del pase temporal (o la copia de inicializacion) como historia
        // para el frame siguiente. QueueTextureExtraction ejecuta el pase de extraccion
        // cuando el RDG se ejecuta al final del frame, antes de que el siguiente frame comience.
        GraphBuilder.QueueTextureExtraction(TemporalOutput, &HistoryRT[WriteIndex]);

        // Actualizar el estado de ping-pong DESPUES de llamar a QueueTextureExtraction:
        // el proximo frame leera de WriteIndex (que ahora contiene la historia actual).
        HistoryReadIndex = WriteIndex;
        HistorySize      = OutputExtent;
        HistoryFormat    = OutputPixelFormat;
        bHistoryValid    = true;
    }

    // -----------------------------------------------------------------------
    // Copia a OverrideOutput (cuando el engine pre-asigna el destino final).
    //
    // Se aplica sobre FinalTexture (OutputTexture sin temporal, o TemporalOutput
    // si el pase EMA se ejecuto). Ambas tienen el mismo formato y extension que
    // OverrideOutput porque OutputTexture se creo con OutputPixelFormat/OutputExtent
    // y TemporalOutput hereda la misma especificacion, por lo que D3D12 CopyResource
    // no produce E_INVALIDARG.
    // -----------------------------------------------------------------------
    if (bHasOverride)
    {
        AddCopyTexturePass(GraphBuilder, FinalTexture, InOutInputs.OverrideOutput.Texture);
        return FScreenPassTexture(InOutInputs.OverrideOutput.Texture, InputSceneColor.ViewRect);
    }

    return FScreenPassTexture(FinalTexture, InputSceneColor.ViewRect);
}
