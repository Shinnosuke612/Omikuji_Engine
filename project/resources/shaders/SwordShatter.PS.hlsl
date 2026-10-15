// 役割: 前面のCapture/裏面の板へ元UVの傷とCollapse限定の縁を重ね、opaque面を描く。
#include "SwordShatter.hlsli"
float4 main(ShatterOutput input, bool frontFace : SV_IsFrontFace) : SV_TARGET0
{
    float3 color = frontFace ? gCapture.Sample(gLinearClamp,input.sourceUV).rgb : gBackColor.rgb;
    float2 p = HeightSpace(input.sourceUV);
    uint count = min((uint)gStyleCutCount.z,kMaxCuts);
    for (uint i=0; i<count; ++i)
    {
        CutData cut = gCuts[i];
        float reveal = cut.revealGlowWidths.x;
        // reveal=0を点の傷として描かず、開始前の分割線を見せない。
        if (reveal > 0)
        {
            float2 a = HeightSpace(cut.segmentUV.xy);
            float2 b = HeightSpace(lerp(cut.segmentUV.xy,cut.segmentUV.zw,reveal));
            float coverage = StrokeCoverage(SegmentDistance(p,a,b),cut.revealGlowWidths.z);
            color = lerp(color,cut.lineColor.rgb,coverage*cut.lineColor.a);
        }
    }
    if (gViewportAspectPhase.w > 0)
    {
        FragmentData fragment = gFragments[input.fragmentIndex];
        float distance = 1.0e20f;
        for (uint j=0; j<fragment.edgeCount; ++j)
        {
            EdgeData edge = gEdges[fragment.firstEdge+j];
            distance = min(distance,SegmentDistance(p,HeightSpace(edge.from),HeightSpace(edge.to)));
        }
        // 境界の内側だけで所定幅を取る。厚みmeshや隣接片への透過は作らない。
        float coverage = StrokeCoverage(distance,gStyleCutCount.x*2);
        color = lerp(color,gEdgeColor.rgb,coverage*gEdgeColor.a);
    }
    return float4(color,1);
}
