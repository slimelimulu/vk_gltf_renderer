#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_fragment_shader_barycentric : enable
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_debug_printf : enable


layout(location = 0) in Interpolants {
	vec3 pos;
  vec4 normal;
  vec4 color;
  vec4 tangent;
  vec2 texCoord;
}IN;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec4 outColor;
layout(location = 3) out vec4 outColor;
// 应该直接采样返回color layout(location = 4) out vec2 uv; 

void main(){
	

}	
