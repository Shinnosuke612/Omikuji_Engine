// 役割: 左上原点の元UVを重心局所座標へ変換し、評価済みTRSからclip空間へ出力する。
#include "SwordShatter.hlsli"
struct Input { float2 sourceUV : TEXCOORD0; uint fragmentIndex : FRAGMENT0; };
ShatterOutput main(Input input)
{
    FragmentData fragment = gFragments[input.fragmentIndex];
    float2 offset = input.sourceUV-fragment.centroidUV;
    float3 local = float3(offset.x*gViewportAspectPhase.z,-offset.y,0);
    ShatterOutput output;
    output.position = mul(float4(local,1),fragment.localToClip);
    output.sourceUV = input.sourceUV;
    output.fragmentIndex = input.fragmentIndex;
    return output;
}
