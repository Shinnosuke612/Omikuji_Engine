// 役割: sample済みFlashをDisplay上へalpha合成する。独自の時間進行は持たない。
#include "Fullscreen.hlsli"
#include "SwordShatter.hlsli"
float4 main(VertexShaderOutput input) : SV_TARGET0
{
    return float4(gFlashColor.rgb,saturate(gFlashColor.a*gFlashIntensity.x));
}
