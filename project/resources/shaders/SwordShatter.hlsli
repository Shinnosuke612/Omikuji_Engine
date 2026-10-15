// 役割: 断片RendererのGPU layoutと元画面UVに付随する傷・発光の距離評価を共有する。
// CPUのstride/field順と一致させる。時間評価はAsset側だけが所有する。
static const uint kMaxCuts = 16;
struct FragmentData
{
    row_major float4x4 localToClip;
    float2 centroidUV;
    uint firstEdge;
    uint edgeCount;
};
struct EdgeData { float2 from; float2 to; };
struct CutData
{
    float4 segmentUV;
    float4 lineColor;
    float4 glowColor;
    float4 revealGlowWidths;
    float4 intensityPadding;
};
Texture2D<float4> gCapture : register(t0);
StructuredBuffer<FragmentData> gFragments : register(t1);
StructuredBuffer<CutData> gCuts : register(t2);
StructuredBuffer<EdgeData> gEdges : register(t3);
SamplerState gLinearClamp : register(s0);
cbuffer Globals : register(b0)
{
    float4 gViewportAspectPhase; // width/height/aspect/Collapse
    float4 gBackColor;
    float4 gEdgeColor;
    float4 gStyleCutCount; // edgeWidth/AA下限/cutCount/unused
    float4 gFlashColor;
    float4 gFlashIntensity;
};
struct ShatterOutput
{
    float4 position : SV_POSITION;
    float2 sourceUV : TEXCOORD0;
    nointerpolation uint fragmentIndex : TEXCOORD1;
};
float2 HeightSpace(float2 uv) { return uv * float2(gViewportAspectPhase.z, 1); }
float SegmentDistance(float2 p, float2 a, float2 b)
{
    float2 delta = b-a;
    float t = saturate(dot(p-a,delta) / max(dot(delta,delta),1.0e-20f));
    return length(p-lerp(a,b,t));
}
float StrokeCoverage(float distance, float width)
{
    // widthが小さくてもsmoothstepの両端を分け、静止点での0除算を防ぐ。
    float aa = max(fwidth(distance),gStyleCutCount.y);
    float halfWidth = width*.5f;
    return 1-smoothstep(max(halfWidth-aa,0),halfWidth+aa,distance);
}
