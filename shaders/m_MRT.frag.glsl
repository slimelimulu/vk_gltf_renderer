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
#include "dh_bindings.h"
// #include "nvvkhl/shaders/dh_scn_desc.h"



// 应该直接采样返回color layout(location = 4) out vec2 uv; 
struct RenderNode
{
  mat4 objectToWorld;
  mat4 worldToObject;
  int  materialID;
  int  renderPrimID;
};
#include "nvvkhl/shaders/dh_lighting.h"
#define MAX_NB_LIGHTS 1
#define WORKGROUP_SIZE 16
struct SceneFrameInfo
{
  mat4  projMatrix;            // projection matrix
  mat4  projMatrixI;           // inverse projection matrix
  mat4  viewMatrix;            // view matrix (world to camera)
  mat4  viewMatrixI;           // inverse view matrix (camera to world)
  Light light[MAX_NB_LIGHTS];  // Light information
  vec4  envIntensity;          // Environment intensity
  vec3  camPos;                // Camera position
  int   flags;                 // Use flag bits instead of separate useSky and useSolidBackground
  int   nbLights;              // Number of lights
  float envRotation;           // Environment rotation (used for the HDR)
  int   frameCount;            // Current render frame [0, ... [
  float envBlur;               // Level of blur for the environment map (0.0: no blur, 1.0: full blur)
  int   useSolidBackground;    // Use solid background color (0==false, 1==true)
  vec3  backgroundColor;       // Background color when using solid background
};
struct SceneDescription
{
  uint64_t materialAddress;
  uint64_t renderNodeAddress;
  uint64_t renderPrimitiveAddress;
  uint64_t lightAddress;
  int      numLights;  // number of punctual lights
};

layout(buffer_reference, scalar) readonly buffer RenderNodeBuf      { RenderNode _[]; };
//layout(buffer_reference, scalar) readonly buffer RenderPrimitiveBuf { RenderPrimitive _[]; };
//layout(buffer_reference, scalar) readonly buffer GltfMaterialBuf    { GltfShadeMaterial m[]; };

layout(location = 0) in Interpolants {
	vec3 pos;
  vec3 normal;
  vec2 texCoord;
}IN;

layout(location = 0) out vec4 pos;
layout(location = 1) out vec4 normal_materialID;
layout(location = 2) out vec4 texCoord;

layout(set = 0, binding = eFrameInfo, scalar) uniform FrameInfo_ { SceneFrameInfo frameInfo; };
layout(set = 0, binding = eSceneDesc) readonly buffer SceneDesc_ { SceneDescription sceneDesc; } ;
layout(set = 0, binding = eTextures) uniform sampler2D[] texturesMap;
void main(){
  // 取材质id
  RenderNode renderNode = RenderNodeBuf(sceneDesc.renderNodeAddress)._[pc.renderNodeID];
	pos = vec4(IN.pos, 1.0f);
  normal_materialID.xyz = normalize(IN.normal.xyz);
  texCoord.xy = IN.texCoord.xy;
  normal_materialID.w = intBitsToFloat(renderNode.materialID); // 存materialID
  

}	
