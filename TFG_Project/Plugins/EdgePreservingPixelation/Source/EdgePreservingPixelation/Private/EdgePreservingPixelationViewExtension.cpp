// Copyright (c) 2025 Manuel. Todos los derechos reservados.
// TFG: Plugin de pixelacion con preservacion de bordes — Implementacion del View Extension.

#include "EdgePreservingPixelationViewExtension.h"
#include "EdgePreservingPixelationShader.h"

#include "ScreenPass.h"                              // FScreenPassTexture — incluido aquí, no en el .h público
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
    // UnscaledViewRect: viewport en píxeles de pantalla antes de cualquier escala DPI.
    // FSceneView no expone ViewRect directamente en UE5.5; el miembro correcto es UnscaledViewRect.
    const FIntRect ViewRect     = View.UnscaledViewRect;
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

    // -----------------------------------------------------------------------
    // Copia a OverrideOutput (cuando el engine pre-asigna el destino final).
    //
    // El engine presenta OverrideOutput directamente como salida de pantalla
    // independientemente del FScreenPassTexture que devolvamos; si no escribimos
    // en él, la pantalla queda negra/sin inicializar.
    //
    // OutputTexture se creó con el mismo formato y extensión que OverrideOutput
    // (ver bloque anterior), por lo que D3D12 CopyResource no produce E_INVALIDARG.
    // -----------------------------------------------------------------------
    FRDGTextureRef FinalTexture = OutputTexture;
    if (bHasOverride)
    {
        FinalTexture = InOutInputs.OverrideOutput.Texture;
        AddCopyTexturePass(GraphBuilder, OutputTexture, FinalTexture);
    }

    return FScreenPassTexture(FinalTexture, InputSceneColor.ViewRect);
}
