#version 450

layout(location = 0) in vec2 inUV;

layout(set = 1, binding = 0) uniform sampler2D tex_pos;
layout(set = 1, binding = 1) uniform sampler2D tex_normal_materialID;
layout(set = 1, binding = 2) uniform sampler2D tex_texCoord;

layout(location = 0) out vec4 outColor;
void main(){
    vec4 worldPos = texture(tex_pos, inUV);
    vec4 N_MID = texture(tex_normal_materialID, inUV);
    int materialID = floatBitsToInt(N_MID.w);
    vec3 N = N_MID.xyz;
    vec2 texUV = texture(tex_texCoord, inUV).xy;
    outColor = vec4(0.5f, 0.5f, 0.5f, 1.0f);

}