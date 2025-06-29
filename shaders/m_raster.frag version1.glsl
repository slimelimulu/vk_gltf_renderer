#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_fragment_shader_barycentric : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_debug_printf : enable

#include "device_host.h"
#include "dh_bindings.h"
#include "nvvkhl/shaders/dh_sky.h"
#include "nvvkhl/shaders/dh_scn_desc.h"
#include "nvvkhl/shaders/func.h"
#include "nvvkhl/shaders/pbr_mat_struct.h"
#include "nvvkhl/shaders/bsdf_structs.h"
#include "nvvkhl/shaders/bsdf_functions.h"
#include "nvvkhl/shaders/light_contrib.h"
#include "nvvkhl/shaders/vertex_accessor.h"

layout(location = 0) in Interpolants {
	vec3 pos;
}IN;

layout(location = 0) out vec4 outColor;

// Buffers
layout(buffer_reference, scalar) readonly buffer GltfMaterialBuf    { GltfShadeMaterial m[]; };

layout(set = 0, binding = eFrameInfo, scalar) uniform FrameInfo_ { SceneFrameInfo frameInfo; };
layout(set = 0, binding = eSceneDesc) readonly buffer SceneDesc_ { SceneDescription sceneDesc; } ;
layout(set = 0, binding = eTextures) uniform sampler2D[] texturesMap;

#include "nvvkhl/shaders/pbr_mat_eval.h"
#include "get_hit.h"

layout(push_constant) uniform RasterPushConstant_
{
  PushConstantRaster pc;
};


void main(){
	// Current Instance
	RenderNode renderNode = RenderNodeBuf(sceneDesc.renderNodeAddress)._[pc.renderNodeID];

	// Mesh used by instance
	RenderPrimitive renderPrim = RenderPrimitiveBuf(sceneDesc.renderPrimitiveAddress)._[pc.renderPrimID];

	// Using same hit code as for ray tracing
	const vec3 worldRayOrigin = vec3(frameInfo.viewMatrixI[3].x, frameInfo.viewMatrixI[3].y, frameInfo.viewMatrixI[3].z);
	HitState   hit      = getHitState(renderPrim, gl_BaryCoordEXT, gl_PrimitiveID, worldRayOrigin,
                                          mat4x3(renderNode.objectToWorld), mat4x3(renderNode.worldToObject));

	outColor = hit.color;




}	
