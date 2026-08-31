// A stand-in for "the game's own dispatch", used only by the WARP harness.
//
// It reads from every root-argument kind our restore has to replay — a root constant, a root
// CBV, and a descriptor table SRV — and writes them out. If the restore puts any of them back
// wrongly, the output changes, which is the smallest honest analogue of "the image went wrong".

cbuffer RootConstants : register(b0) { uint4 Marker; };
cbuffer RootCbv       : register(b1) { uint4 CbvValue; };

Texture2D<float4>  Source : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const float4 s = Source.Load(int3(0, 0, 0));
    Out[0] = uint4(Marker.x, CbvValue.x, asuint(s.x), 0xC0FFEE);
}
