#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_fragment_shader_barycentric : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_debug_printf : enable

struct PushConstantRaster
{
  int materialID;          // Material used by the rendering instance
  int renderNodeID;        // Node used by the rendering instance
  int renderPrimID;        // Primitive used by the rendering instance
  int dbgMethod;           // Debugging method
  int selectedRenderNode;  // The node that is selected, used to create silhouette
};

layout(push_constant) uniform RasterPushConstant_
{
  PushConstantRaster pc;
};

#include "nvvkhl/shaders/dh_scn_desc.h"


layout(location = 0) in Interpolants {
	vec3 pos;
  vec4 normal;
  vec4 color;
  vec4 tangent;
  vec2 texCoord;
}IN;

layout(location = 0) out vec4 pos;
layout(location = 1) out vec4 normal;
layout(location = 2) out vec4 albedo;
layout(location = 3) out vec4 material;
// 应该直接采样返回color layout(location = 4) out vec2 uv; 

layout(buffer_reference, scalar) readonly buffer RenderNodeBuf      { RenderNode _[]; };
layout(buffer_reference, scalar) readonly buffer RenderPrimitiveBuf { RenderPrimitive _[]; };
layout(buffer_reference, scalar) readonly buffer GltfMaterialBuf    { GltfShadeMaterial m[]; };

layout(set = 0, binding = eFrameInfo, scalar) uniform FrameInfo_ { SceneFrameInfo frameInfo; };
layout(set = 0, binding = eSceneDesc) readonly buffer SceneDesc_ { SceneDescription sceneDesc; } ;
layout(set = 0, binding = eTextures) uniform sampler2D[] texturesMap;
void main(){
  // 取材质
  RenderNode renderNode = RenderNodeBuf(sceneDesc.renderNodeAddress)._[pc.renderNodeID];
  GltfShadeMaterial gltfMat = GltfMaterialBuf(sceneDesc.materialAddress).m[renderNode.materialID];

	pos = vec4(IN.pos, 1.0f);
  normal = normalize(IN.normal) * 2.0f - 1.0f;
  tangent = normalize(IN.tangent) * 2.0f - 1.0f;
  albedo = IN.color * gltfMat.pbrBaseColorFactor; // 它还真用了texturemap，注意（pbr_mat_eval.h）

  

}	
