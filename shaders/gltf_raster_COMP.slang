/*
 * Copyright (c) 2023-2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

// texture

[[vk::binding(0, 1)]] Sampler2D worldPosTex;
[[vk::binding(1, 1)]] Sampler2D normalMIDTex;
[[vk::binding(2, 1)]] Sampler2D texCoordTex;

struct PSin
{
  float2 uv : TEXCOORD;
};

// Output of the vertex shader
struct VSout
{
  PSin   stage;
  float4 sv_position : SV_Position;
};

// Output of the fragment shader
struct PSout
{
  float4 color : SV_Target;
};



// Vertex  Shader
[shader("vertex")]
VSout COMPvertexMain(uint vid: SV_VertexID)  // gl_VertexIndex
{
  VSout  output;
  float2 uv          = float2((vid << 1) & 2, vid & 2);
  output.stage.uv    = uv;
  output.sv_position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);

  return output;
}


// Fragment Shader
[shader("fragment")]
PSout COMPfragmentMain(PSin input)
{
  PSout  output;
  float2 uv = input.uv;

  output.color = float4(1.0f, 0.5f, 0.5f, 1.0f);

  return output;
}
