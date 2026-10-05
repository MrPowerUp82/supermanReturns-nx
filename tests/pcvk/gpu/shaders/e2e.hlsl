// Project-owned fullscreen triangle and a green pixel for the front-end -> Vulkan end-to-end test.
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    float2 p = vertex == 0 ? float2(-1,-1) : vertex == 1 ? float2(3,-1) : float2(-1,3);
    return float4(p,0.5,1);
}
float4 PSMain() : SV_Target0 { return float4(0,1,0,1); }
