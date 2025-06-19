#version 460
#extension GL_GOOGL_include_directive : enable

#include "common_struct.h.glsl"

layout(local_size_x = 256)in;

layout(push_constant, std430) uniform PushConstants {
    uint g_num_elements;
    vec3 g_min;
    vec3 g_max;
};

layout(std430, set=0, binding=0) writeonly buffer morton_codes{
    MortonCodeElement g_morton_codes[];
};

layout(std430, set=0, binding=0) readonly buffer elements{
    Element g_elements[];
};

// 将一个10位整数转到30位，每个数后面填两个0
uint expandBits(uint v){
    // 9876543210 -> 0000 0098 0000 0000 0000 7654 3210
    v = (v * 0b0000 0000 0000 0001 0000 0000 0000 0001u) & 0b1111 1111 0000 0000 0000 0000 1111 1111;
    // -> 0000 0098 0000 7654 0000 0000 3210 
    v = (v * 0b0000 0000 0000 0000 0000 0001 0000 0001u) & 0b0000 1111 0000 0000 1111 0000 0000 1111;

    v = (v * 0b0000 0000 0000 0000 0000 0000 0001 0001u) & 0b1010 0011 0000 1010 0011 0000 1010 0011;
    // -> 
    v = (v * 0b0000 0000 0000 0000 0000 0000 0000 0101u) & 0b0100 1001 0010 0100 1001 0010 0100 1001;
    return v;
}

// 输入浮点数，输出莫顿码：
uint morton3D(float x, float y, float z){
    // 归到10位，即1024内
    x = clamp(x * 1024., 0.0f, 1024.0f);
    y = clamp(y * 1024., 0.0f, 1024.0f);
    z = clamp(z * 1024., 0.0f, 1024.0f);

    // 转到uint，然后expand到30位
    uint xbit = expandBits((uint)x);
    uint ybit = expandBits((uint)y);
    uint zbit = expandBits((uint)z);

    // 转到32位
    return 4 * xbit + 2 * ybit + zbit;

}

void main(){
    uint tx = gl_GlobalInvocationID.x;
    if (tx >= g_num_elements){
        return;
    }
    // 归一化
    Element local_element = g_elements[tx];
    // 使用aabb的中心进行morton code
    vec3 aabbmin = vec3(local_element.aabbMinX, local_element.aabbMinY, local_element.aabbMinZ);
    vec3 aabbmax = vec3(local_element.aabbMaxX, local_element.aabbMaxY, local_element.aabbMaxZ);
    // middle

    vec3 center = (aabbmin + aabbmax) * 0.5;
    center = (center - g_min) * (1.0f / (g_max - g_min));
    uint morton = morton3D(center.x, center.y, center.z);

    // 写入：
    morton_codes[tx] = MortonCodeElement(morton, tx);

}