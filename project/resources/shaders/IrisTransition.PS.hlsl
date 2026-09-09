// 役割: 画面中央の円形領域だけを残して表示するアイリストランジション。
#include "Fullscreen.hlsli"

Texture2D<float4> gTexture : register(t0);
Texture2D<float4> gMaskTexture : register(t2);
SamplerState gSampler : register(s0);

// radialBlurCenter と radialBlurWidth をアイリスの中心・半径として利用する。
struct PostEffectParameters
{
    float vignetteScale;
    float vignettePower;
    float vignetteIntensity;
    float blurStrength;
    uint blurRadius;
    float gaussianSigma;
    float2 padding;
    float outlineLuminanceWeight;
    float outlineDepthWeight;
    float outlineThreshold;
    float outlineSoftness;
    float outlineThickness;
    float cameraNear;
    float cameraFar;
    uint outlineFlags;
    float4 outlineColor;
    float2 radialBlurCenter;
    float radialBlurWidth;
    uint radialBlurSamples;
};

ConstantBuffer<PostEffectParameters> gParameters : register(b0);

float4 main(VertexShaderOutput input) : SV_TARGET0
{
    // 半径を縮小しながらマスクを中央へ寄せる。
    const float requestedScale = max(gParameters.radialBlurWidth * 4.0f, 0.0f);
    const float scale = max(requestedScale, 0.0001f);
    const float2 maskUv =
        (input.texcoord - gParameters.radialBlurCenter) / scale + 0.5f;
    const bool insideMask = all(maskUv >= 0.0f) && all(maskUv <= 1.0f);
    // 指定画像の透明部分だけを穴として、Scene画像を表示する。
    const float maskAlpha = insideMask
        ? gMaskTexture.Sample(gSampler, maskUv).a
        : 1.0f;
    const float hole = 1.0f - maskAlpha;
    // 不透明部分は演出開始フレームから完全に黒で覆う。
    const float4 sourceColor = gTexture.Sample(gSampler, input.texcoord);
    // 中央の1ピクセルが透明でも、縮小完了時には必ず完全な黒にする。
    const float visible = requestedScale <= 0.001f ? 0.0f : hole;
    return float4(sourceColor.rgb * visible, sourceColor.a);
}
