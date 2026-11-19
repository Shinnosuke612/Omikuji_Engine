// 役割: RGBA8 sRGBの直前mipをlinear lightでBox平均し、次段へsRGBで格納する。
// 一threadは出力1texelを専有。段間同期と前段SRV/次段UAVの切替はDIRECT側で行う。
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Sizes : register(b0) { uint2 SourceSize; uint2 TargetSize; };
float3 Decode(float3 value) {
    return float3(value.x <= 0.04045 ? value.x / 12.92 : pow((value.x + 0.055) / 1.055, 2.4),
                  value.y <= 0.04045 ? value.y / 12.92 : pow((value.y + 0.055) / 1.055, 2.4),
                  value.z <= 0.04045 ? value.z / 12.92 : pow((value.z + 0.055) / 1.055, 2.4));
}
float3 Encode(float3 value) {
    value = saturate(value);
    return float3(value.x <= 0.0031308 ? value.x * 12.92 : 1.055 * pow(value.x, 1.0 / 2.4) - 0.055,
                  value.y <= 0.0031308 ? value.y * 12.92 : 1.055 * pow(value.y, 1.0 / 2.4) - 0.055,
                  value.z <= 0.0031308 ? value.z * 12.92 : 1.055 * pow(value.z, 1.0 / 2.4) - 0.055);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= TargetSize)) return;
    uint2 p = min(id.xy * 2, SourceSize - 2);
    float4 a = Source.Load(int3(p,0));
    float4 b = Source.Load(int3(p+uint2(1,0),0));
    float4 c = Source.Load(int3(p+uint2(0,1),0));
    float4 d = Source.Load(int3(p+uint2(1,1),0));
    // UNORM viewでencoded値を読み、RGBだけgamma変換。alphaはstraight平均のまま。
    Target[id.xy] = float4(Encode((Decode(a.rgb)+Decode(c.rgb)+Decode(b.rgb)+Decode(d.rgb))*0.25),
                          (a.a+c.a+b.a+d.a)*0.25);
}
