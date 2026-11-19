// 役割: 断片と同じmesh/depth/元UVで一閃の先端と残光をlinear加算する。
#include "SwordShatter.hlsli"
float4 main(ShatterOutput input, bool frontFace : SV_IsFrontFace) : SV_TARGET0
{
    if (!frontFace) return float4(0,0,0,0);
    float2 p = HeightSpace(input.sourceUV);
    float3 light = 0;
    uint count = min((uint)gStyleCutCount.z,kMaxCuts);
    for (uint i=0; i<count; ++i)
    {
        CutData cut = gCuts[i];
        float reveal = cut.revealGlowWidths.x;
        // V2の進行中cutは時刻0でもglow=1を持つ。未表示線の点発光をV1と分けて抑える。
        if (gFlashIntensity.y > 0 && reveal <= 0) continue;
        float2 a = HeightSpace(cut.segmentUV.xy);
        float2 end = HeightSpace(cut.segmentUV.zw);
        float2 head = lerp(a,end,reveal);
        float headDistance = length(p-head);
        float radius = cut.revealGlowWidths.w*.5f;
        float aa = max(fwidth(headDistance),gStyleCutCount.y);
        float headGlow = 1-smoothstep(0,radius+aa,headDistance);
        float trailDistance = SegmentDistance(p,a,head);
        float trailAA = max(fwidth(trailDistance),gStyleCutCount.y);
        float trail = 1-smoothstep(0,radius+trailAA,trailDistance);
        float2 delta = head-a;
        float progress = saturate(dot(p-a,delta)/max(dot(delta,delta),1.0e-20f));
        float shape = max(headGlow,trail*progress);
        light += cut.glowColor.rgb * (shape*cut.revealGlowWidths.y*cut.intensityPadding.x*cut.glowColor.a);
    }
    return float4(light*gStyleCutCount.w,0);
}
