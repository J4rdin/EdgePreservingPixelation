// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del View Extension.

#include "EdgePreservingPixelationViewExtension.h"
#include "EdgePreservingPixelationShader.h"

#include "PostProcess/PostProcessMaterialInputs.h"  // FPostProcessMaterialInputs, EPostProcessMaterialInput
#include "RenderGraphUtils.h"                        // FComputeShaderUtils::AddPass, AddClearUAVPass
#include "RenderGraphBuilder.h"                      // FRDGBuilder
#include "SceneView.h"                               // FSceneView
#include "GlobalShader.h"                            // GetGlobalShaderMap

// Variable de consola para controlar el tamaño del bloque de pixelación en tiempo de ejecución.
// ECVF_RenderThreadSafe permite leer el valor de forma segura desde el render thread.
// Uso en consola: EPP.PixelSize 4  (o 8, 16, etc.)
static TAutoConsoleVariable<float> CVarEPPPixelSize(
    TEXT("EPP.PixelSize"),
    8.0f,
    TEXT("Tamanno del bloque de pixelacion en pixeles de pantalla. Minimo efectivo: 2."),
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
    EPostProcessingPass PassId,
    FAfterPassCallbackDelegateArray& InOutPassCallbacks,
    bool bIsPassEnabled)
{
    // Registrar el callback únicamente para el pase Tonemap.
    // El renderer llamará a esta función una vez por pase activo en el pipeline;
    // ignoramos todos los PassId que no sean Tonemap.
    if (PassId == EPostProcessingPass::Tonemap)
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
    // Textura de salida
    // Mismas dimensiones y formato que la entrada; con TexCreate_UAV para permitir
    // la escritura RWTexture2D desde el compute shader.
    // TexCreate_ShaderResource: permite usar la salida como SRV en pases posteriores.
    // -----------------------------------------------------------------------
    const FIntPoint RTExtent = InputSceneColor.Texture->Desc.Extent;

    const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
        RTExtent,
        InputSceneColor.Texture->Desc.Format,
        FClearValueBinding::Black,
        TexCreate_UAV | TexCreate_ShaderResource
    );
    FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("EPP_PixelatedOutput"));

    // -----------------------------------------------------------------------
    // Texturas placeholder para SceneDepth y SceneVelocity (Fase 2).
    // No se muestrean en el shader de esta fase; se proporcionan para satisfacer
    // la validación RDG que exige que todos los SRV declarados en FParameters estén
    // enlazados a recursos válidos y previamente escritos.
    //
    // AddClearUAVPass escribe en el recurso dentro del grafo, marcándolo como
    // inicializado para la validación de dependencias del RDG.
    //
    // En Fase 3 estos placeholders serán reemplazados por los render targets reales
    // (SceneTextures.Depth.Target y SceneTextures.Velocity).
    // -----------------------------------------------------------------------
    FRDGTextureRef PlaceholderDepth = GraphBuilder.CreateTexture(
        FRDGTextureDesc::Create2D(
            FIntPoint(1, 1),
            PF_R32_FLOAT,
            FClearValueBinding(0.0f),
            TexCreate_ShaderResource | TexCreate_UAV),
        TEXT("EPP_PlaceholderDepth")
    );
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(PlaceholderDepth), FLinearColor::Transparent);

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
    const FIntRect ViewRect     = View.ViewRect;
    const float    PixelSizeVal = FMath::Max(1.0f, CVarEPPPixelSize.GetValueOnRenderThread());

    FEdgePreservingPixelationCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FEdgePreservingPixelationCS::FParameters>();

    // SRVs de entrada
    Parameters->SceneColor    = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(InputSceneColor.Texture));
    Parameters->SceneDepth    = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(PlaceholderDepth));
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

    // Constant buffer — Registro 2 (reservado para Fase 3)
    Parameters->TemporalJitterX = 0.0f;
    Parameters->TemporalJitterY = 0.0f;
    Parameters->FrameIndex      = 0.0f;
    Parameters->Padding1        = 0.0f;

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

    // Devolver la textura de salida con el ViewRect original preservado.
    // El pipeline de post-proceso encadenará esta textura como entrada del siguiente pase.
    return FScreenPassTexture(OutputTexture, InputSceneColor.ViewRect);
}
