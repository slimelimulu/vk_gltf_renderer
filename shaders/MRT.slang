#include "nvshaders/bsdf_functions.h.slang"
#include "nvshaders/bsdf_types.h.slang"
#include "nvshaders/gltf_scene_io.h.slang"
#include "nvshaders/gltf_vertex_access.h.slang"
#include "nvshaders/hdr_env_sampling.h.slang"
#include "nvshaders/light_contrib.h.slang"
#include "nvshaders/pbr_ggx_microfacet.h.slang"
#include "nvshaders/pbr_material_eval.h.slang"
#include "nvshaders/ray_utils.h.slang"
#include "nvshaders/sample_blur.h.slang"


#include "shaderio.h"
#include "get_hit.h.slang"
#include "common.h.slang"

[[vk::push_constant]] ConstantBuffer<RasterPushConstant> pushConst;

struct VertexInput {
    [[vk::location(0), vk::binding(0)]]
    float3 position : POSITION;
    [[vk::location(1), vk::binding(1)]]
    float3 normal : NORMAL;
    [[vk::location(2), vk::binding(2)]]
    float3 uv : TEXCOORD;
}

struct VertexOutput
{
    float4 position : SV_Position; // Clip space position (required)
    float3 worldPos;
    float3 normal;
    float2 uv;
};

struct PixelOutput
{
    float4 position : SV_TARGET0;     
    float4 normal_id : SV_TARGET1;    
    float4 uv : SV_TARGET2;
};

[shader("vertex")]
VertexOutput MRTvertexMain(VertexInput input) {
    GltfRenderNode renderNode = pushConst.gltfScene.renderNodes[pushConst.renderNodeID];

    float3 pos = mul(float4(input.position, 1.0), renderNode.objectToWorld).xyz;

    VertexOutput output;
    output.worldPos = pos;
    output.position = mul(float4(pos, 1.0), pushConst.frameInfo.viewProjMatrix);
    output.normal.xyz = input.normal.xyz;
    output.uv = input.uv.xy;
    return output;
}

[shader("fragment")]
PixelOutput MRTfragmentMain(VertexOutput input) {
    PixelOutput output;
    output.position = float4(1.0f, 0.0f, 0.0f, 1.0f);
    output.normal_id = float4(0.0f, 1.0f, 0.0f, 1.0f);
    output.uv = float4(0.0f, 0.0f, 1.0f, 1.0f);
    return output;
}