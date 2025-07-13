#include <nvapp/elem_dbgprintf.hpp>
#include <nvutils/camera_manipulator.hpp>
#include <nvutils/parameter_registry.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>
#include <nvvk/helpers.hpp>
#include <nvvk/commands.hpp>
#include "render_ddgiRaster.hpp"

// Pre-compiled shaders
#include "_autogen/gltf_raster.slang.h"
#include "_autogen/sky_physical.slang.h"

#include "nvvk/default_structs.hpp"
#include<shaderc/shaderc.hpp>
#include<fstream>
#include<sstream>

std::string readFile(const std::string& filename) {
	std::ifstream file(filename, std::ios::in | std::ios::binary);
	if (!file.is_open())
		throw std::runtime_error("Could not open file " + filename);
	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}


void DDGIRasterizer::onAttach(Resources& resources, nvvk::ProfilerGpuTimer* profiler)
{
	::BaseRenderer::onAttach(resources, profiler);
	m_device = resources.allocator.getDevice();
	m_commandPool = resources.commandPool;
	m_skyPhysical.init(&resources.allocator, std::span(sky_physical_slang));
	compileShader(resources, false);  // Compile the shader
	createRecordCommandBuffer();
	
	
	
}

void DDGIRasterizer::registerParameters(nvutils::ParameterRegistry* paramReg)
{
	// Rasterizer-specific command line parameters
	// paramReg->add({ "rasterWireframe", "Rasterizer: Enable wireframe mode" }, &m_enableWireframe);
	// paramReg->add({ "rasterUseRecordedCmd", "Rasterizer: Use recorded command buffers" }, &m_useRecordedCmd);
}

void DDGIRasterizer::onDetach(Resources& resources)
{
	vkDestroyPipelineLayout(m_device, m_MRTPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_device, m_COMPPipelineLayout, nullptr);
	vkDestroyShaderEXT(m_device, m_MRTvertexShader, nullptr);
	vkDestroyShaderEXT(m_device, m_MRTfragmentShader, nullptr);
	vkDestroyShaderEXT(m_device, m_COMPvertexShader, nullptr);
	vkDestroyShaderEXT(m_device, m_COMPfragmentShader, nullptr);
	vkDestroyShaderEXT(m_device, m_wireframeShader, nullptr);

	m_skyPhysical.deinit();
}

void DDGIRasterizer::onResize(VkCommandBuffer cmd, const VkExtent2D& size, Resources& resources)
{
	freeRecordCommandBuffer();
}

bool DDGIRasterizer::onUIRender(Resources& resources)
{
	namespace PE = nvgui::PropertyEditor;
	bool changed = false;
	if (PE::begin())
	{
		PE::Checkbox("Wireframe", &m_enableWireframe);
		PE::Checkbox("Use Recorded Cmd", &m_useRecordedCmd, "Use recorded command buffers for better performance");
		PE::end();
	}

	return false;
}


void DDGIRasterizer::onRender(VkCommandBuffer cmd, Resources& resources)
{
	NVVK_DBG_SCOPE(cmd);  // <-- Helps to debug in NSight

	// 打印各个ColorAttachement的地址
	//LOGI("gBuffer:%p\n", (void*)resources.gBuffers.getColorImage(0));
	//LOGI("gBufferDefer0:%p\n", (void*)resources.gBuffersDefer.getColorImage(0));
	//LOGI("gBufferDefer1:%p\n", (void*)resources.gBuffersDefer.getColorImage(1));
	//LOGI("gBufferDefer2:%p\n", (void*)resources.gBuffersDefer.getColorImage(2));

	// Rendering the environment, 在gbuffer上
	if (!resources.settings.useSolidBackground)
	{
		glm::mat4 viewMatrix = resources.cameraManip->getViewMatrix();
		glm::mat4 projMatrix = resources.cameraManip->getPerspectiveMatrix();

		// Rendering dome or sky in the background, it is covering the entire screen
		if (resources.settings.envSystem == shaderio::EnvSystem::eSky)
		{
			m_skyPhysical.runCompute(cmd, resources.gBuffers.getSize(), viewMatrix, projMatrix, resources.skyParams,
				resources.gBuffers.getDescriptorImageInfo(Resources::eImgRendered));
		}
		else if (resources.settings.envSystem == shaderio::EnvSystem::eHdr)
		{
			resources.hdrDome.draw(cmd, viewMatrix, projMatrix, resources.gBuffers.getSize(), glm::vec4(resources.settings.hdrEnvIntensity),
				resources.settings.hdrEnvRotation, resources.settings.hdrBlur);
		}
	}
	// mrt，在gbufferDefer上
	
	{
		VkRenderingAttachmentInfo renderingInfo_gbuffer = DEFAULT_VkRenderingAttachmentInfo;

		renderingInfo_gbuffer.clearValue = { {{0.0f, 0.0f, 0.0f, 0.0f}} };
		renderingInfo_gbuffer.loadOp = resources.settings.useSolidBackground ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		// 1 - Selection attachment

		// Two attachments, one for color and one for selection
		std::array<VkRenderingAttachmentInfo, 3> attachments = { {renderingInfo_gbuffer, renderingInfo_gbuffer, renderingInfo_gbuffer} };
		// 0 - Color attachment
		attachments[0].imageView = resources.gBuffersDefer.getColorImageView((uint32_t)Resources::EGbuffer::epos);
		attachments[1].imageView = resources.gBuffersDefer.getColorImageView((uint32_t)Resources::EGbuffer::enorm);
		attachments[2].imageView = resources.gBuffersDefer.getColorImageView((uint32_t)Resources::EGbuffer::euv);
		// 1 - Selection attachment
		// attachments[1].imageView = resources.gBuffers.getColorImageView(Resources::eImgSelection);
		// X - Depth
		VkRenderingAttachmentInfo depthAttachment = DEFAULT_VkRenderingAttachmentInfo;
		depthAttachment.imageView = resources.gBuffersDefer.getDepthImageView();
		depthAttachment.clearValue = { .depthStencil = DEFAULT_VkClearDepthStencilValue };


		// Setting up the push constant
		m_pushConst.frameInfo = (shaderio::SceneFrameInfo*)resources.bFrameInfo.address;
		m_pushConst.skyParams = (shaderio::SkyPhysicalParameters*)resources.bSkyParams.address;
		m_pushConst.gltfScene = (shaderio::GltfScene*)resources.sceneVk.sceneDesc().address;
		m_pushConst.mouseCoord = nvapp::ElementDbgPrintf::getMouseCoord();  // Use for debugging: printf in shader
		vkCmdPushConstants(cmd, m_MRTPipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(shaderio::RasterPushConstant), &m_pushConst);


		// Create the rendering info
		VkRenderingInfo renderingInfo = DEFAULT_VkRenderingInfo;
		renderingInfo.flags = m_useRecordedCmd ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0,
			renderingInfo.renderArea = DEFAULT_VkRect2D(resources.gBuffersDefer.getSize());
		renderingInfo.colorAttachmentCount = uint32_t(attachments.size());
		renderingInfo.pColorAttachments = attachments.data();
		renderingInfo.pDepthAttachment = &depthAttachment;

		// Scene is recorded to avoid CPU overhead
		if (m_recordedSceneCmd == VK_NULL_HANDLE && m_useRecordedCmd)
		{
			recordRasterScene(resources);
		}


		// ** BEGIN RENDERING **
		vkCmdBeginRendering(cmd, &renderingInfo);

		if (m_useRecordedCmd && m_recordedSceneCmd != VK_NULL_HANDLE)
		{
			vkCmdExecuteCommands(cmd, 1, &m_recordedSceneCmd);  // Execute the recorded command buffer
		}
		else
		{
			renderRasterScene(cmd, resources);  // Render the scene
		}
		

		vkCmdEndRendering(cmd);
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::epos), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::enorm), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::euv), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });


		
		
	}
	// Composition - 在gbuffer上
	{
		VkRenderingAttachmentInfo renderingInfo_gbuffer = DEFAULT_VkRenderingAttachmentInfo;

		renderingInfo_gbuffer.clearValue = { {{0.0f, 0.0f, 0.0f, 0.0f}} };
		renderingInfo_gbuffer.loadOp = resources.settings.useSolidBackground ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		// 1 - Selection attachment

		// Two attachments, one for color and one for selection
		std::array<VkRenderingAttachmentInfo, 1> attachments = { {renderingInfo_gbuffer} };
		// 0 - Color attachment
		attachments[0].imageView = resources.gBuffers.getColorImageView(Resources::eImgRendered);
		// 1 - Selection attachment
		// attachments[1].imageView = resources.gBuffers.getColorImageView(Resources::eImgSelection);
		// X - Depth
		VkRenderingAttachmentInfo depthAttachment = DEFAULT_VkRenderingAttachmentInfo;
		depthAttachment.imageView = resources.gBuffers.getDepthImageView();
		depthAttachment.clearValue = { .depthStencil = DEFAULT_VkClearDepthStencilValue };

		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffers.getColorImage(Resources::eImgRendered), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		// Setting up the push constant
		m_pushConst.frameInfo = (shaderio::SceneFrameInfo*)resources.bFrameInfo.address;
		m_pushConst.skyParams = (shaderio::SkyPhysicalParameters*)resources.bSkyParams.address;
		m_pushConst.gltfScene = (shaderio::GltfScene*)resources.sceneVk.sceneDesc().address;
		m_pushConst.mouseCoord = nvapp::ElementDbgPrintf::getMouseCoord();  // Use for debugging: printf in shader
		vkCmdPushConstants(cmd, m_COMPPipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(shaderio::RasterPushConstant), &m_pushConst);

		// Create the rendering info
		VkRenderingInfo renderingInfo = DEFAULT_VkRenderingInfo;
		renderingInfo.flags = m_useRecordedCmd ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0,
			renderingInfo.renderArea = DEFAULT_VkRect2D(resources.gBuffers.getSize());
		renderingInfo.colorAttachmentCount = uint32_t(attachments.size());
		renderingInfo.pColorAttachments = attachments.data();
		renderingInfo.pDepthAttachment = &depthAttachment;

		// ** BEGIN RENDERING **
		vkCmdBeginRendering(cmd, &renderingInfo);

		{
			
			// All dynamic states are set here
			m_COMPPipeline.cmdApplyAllStates(cmd);
			m_COMPPipeline.cmdSetViewportAndScissor(cmd, resources.gBuffers.getSize());
			m_COMPPipeline.cmdBindShaders(cmd, { .vertex = m_COMPvertexShader, .fragment = m_COMPfragmentShader });
			vkCmdSetDepthTestEnable(cmd, VK_TRUE);

			// Bind the descriptor set: textures (Set: 0)
			std::array<VkDescriptorSet, 2> descriptorSets{ resources.descriptorSet, resources.gbufferDescSet };
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_COMPPipelineLayout, 0, descriptorSets.size(), descriptorSets.data(), 0, nullptr);
			// Back-face culling with depth bias
			vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
			vkCmdSetDepthBias(cmd, -1.0f, 0.0f, 1.0f);  // Apply depth bias for solid objects
			// finally composition
			vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			
		}
		

		vkCmdEndRendering(cmd);
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::epos), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::enorm), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffersDefer.getColorImage((uint32_t)Resources::EGbuffer::euv), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

		nvvk::cmdImageMemoryBarrier(cmd, { resources.gBuffers.getColorImage(Resources::eImgRendered), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,  VK_IMAGE_LAYOUT_GENERAL });


		}

}

//--------------------------------------------------------------------------------------------------
// Render a list of GLTF nodes with their associated materials and geometry
// Handles:
// 1. Material and node-specific constant updates
// 2. Vertex and index buffer binding
// 3. Draw calls for each primitive
void DDGIRasterizer::renderNodes(VkCommandBuffer cmd, Resources& resources, const std::vector<uint32_t>& nodeIDs)
{
	NVVK_DBG_SCOPE(cmd);

	nvvkgltf::Scene& scene = resources.scene;
	nvvkgltf::SceneVk& sceneVk = resources.sceneVk;

	const std::array<VkDeviceSize, 3>                            offsets{ {0} };
	const std::vector<nvvkgltf::RenderNode>& renderNodes = scene.getRenderNodes();
	const std::vector<nvvkgltf::RenderPrimitive>& subMeshes = scene.getRenderPrimitives();

	// Structure to hold only the changing parts
	struct NodeSpecificConstants
	{
		int32_t materialID;
		int32_t renderNodeID;
		int32_t renderPrimID;
	};

	// Get the offset of materialID in the RasterPushConstant struct
	// This assumes materialID is the first field that changes in the struct
	uint32_t offset = static_cast<uint32_t>(offsetof(shaderio::RasterPushConstant, materialID));

	for (const uint32_t& nodeID : nodeIDs)
	{
		const nvvkgltf::RenderNode& renderNode = renderNodes[nodeID];
		const nvvkgltf::RenderPrimitive& subMesh = subMeshes[renderNode.renderPrimID];  // Mesh referred by the draw object

		if (!renderNode.visible)
			continue;

		// Update only the changing fields
		NodeSpecificConstants nodeConstants{ .materialID = renderNode.materialID,
											.renderNodeID = static_cast<int>(nodeID),
											.renderPrimID = renderNode.renderPrimID };

		// Push only the changing parts
		vkCmdPushConstants(cmd, m_MRTPipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, offset, sizeof(NodeSpecificConstants), &nodeConstants);

		// Bind vertex and index buffers and draw the mesh
		std::array<VkBuffer, 3> vertexBuffers{ 
			sceneVk.vertexBuffers()[renderNode.renderPrimID].position.buffer,
			sceneVk.vertexBuffers()[renderNode.renderPrimID].normal.buffer,
			sceneVk.vertexBuffers()[renderNode.renderPrimID].texCoord0.buffer 
		};
		
		
		vkCmdBindVertexBuffers(cmd, 0, vertexBuffers.size(), vertexBuffers.data(), offsets.data());
		vkCmdBindIndexBuffer(cmd, sceneVk.indices()[renderNode.renderPrimID].buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, 0, 0, 0);
	}
}

//--------------------------------------------------------------------------------------------------
// Push descriptor set updates for the rasterizer
// Currently a placeholder for future descriptor set management
void DDGIRasterizer::pushDescriptorSet(VkCommandBuffer cmd, Resources& resources)
{
	//// Use a compute shader that outputs to the render target for now
	//nvvk::WriteSetContainer write{};
	//write.append(resources.descriptorBinding[1].getWriteSet(0), resources.sceneRtx.tlas());
	//write.append(resources.descriptorBinding[1].getWriteSet(1),
	//             resources.gBuffers.getColorImageView(Resources::eImgRendered), VK_IMAGE_LAYOUT_GENERAL);
	//vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 1, write.size(), write.data());
}

//--------------------------------------------------------------------------------------------------
// Create the graphics pipeline for the rasterizer
// Sets up:
// 1. Pipeline layout with descriptor sets and push constants
// 2. Dynamic state configuration
// 3. Color blending settings for transparent objects
void DDGIRasterizer::createPipeline(Resources& resources)
{
	SCOPED_TIMER(__FUNCTION__);
	{
		// for MRT
		std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ resources.descriptorSetLayout[0],resources.gbufferDescSetlayout };

		// Push constant is used to pass data to the shader at each frame
		const VkPushConstantRange pushConstantRange{
			.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS, .offset = 0, .size = sizeof(shaderio::RasterPushConstant) };

		// The pipeline layout is used to pass data to the pipeline, anything with "layout" in the shader
		const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = uint32_t(descriptorSetLayouts.size()),
			.pSetLayouts = descriptorSetLayouts.data(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushConstantRange,
		};
		NVVK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_MRTPipelineLayout));
		NVVK_DBG_NAME(m_MRTPipelineLayout);
		// Override default
		//  m_dynamicPipeline.colorBlendEnables[0]                       = VK_TRUE;
		m_MRTPipeline.colorBlendEquations[0].alphaBlendOp = VK_BLEND_OP_ADD;
		m_MRTPipeline.colorBlendEquations[0].colorBlendOp = VK_BLEND_OP_ADD;
		m_MRTPipeline.colorBlendEquations[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		m_MRTPipeline.colorBlendEquations[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		m_MRTPipeline.colorBlendEquations[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		m_MRTPipeline.colorBlendEquations[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

		// Add depth bias settings for solid objects
		m_MRTPipeline.rasterizationState.depthBiasEnable = VK_TRUE;
		m_MRTPipeline.rasterizationState.depthBiasConstantFactor = -1.0f;
		m_MRTPipeline.rasterizationState.depthBiasSlopeFactor = 1.0f;

		// Attachment #1 - Selection
		m_MRTPipeline.colorBlendEnables.push_back(false);  // No blending for attachment #1
		m_MRTPipeline.colorBlendEnables.push_back(false);

		//m_MRTPipeline.colorWriteMasks.push_back(
		//	{ VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT });
		m_MRTPipeline.colorWriteMasks.push_back(
			{ VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT });
		m_MRTPipeline.colorWriteMasks.push_back(
			{ VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT });
		m_MRTPipeline.colorBlendEquations.push_back(VkColorBlendEquationEXT{});
		m_MRTPipeline.colorBlendEquations.push_back(VkColorBlendEquationEXT{});
	}
	{
		// for COMP
		std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ resources.descriptorSetLayout[0], resources.gbufferDescSetlayout };

		// Push constant is used to pass data to the shader at each frame
		const VkPushConstantRange pushConstantRange{
			.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS, .offset = 0, .size = sizeof(shaderio::RasterPushConstant) };

		// The pipeline layout is used to pass data to the pipeline, anything with "layout" in the shader
		const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = uint32_t(descriptorSetLayouts.size()),
			.pSetLayouts = descriptorSetLayouts.data(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushConstantRange,
		};
		NVVK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_COMPPipelineLayout));
		NVVK_DBG_NAME(m_COMPPipelineLayout);
		// Override default
		//  m_dynamicPipeline.colorBlendEnables[0]                       = VK_TRUE;
		m_COMPPipeline.colorBlendEnables[0] = VK_FALSE;
		m_COMPPipeline.colorBlendEquations[0].alphaBlendOp = VK_BLEND_OP_ADD;
		m_COMPPipeline.colorBlendEquations[0].colorBlendOp = VK_BLEND_OP_ADD;
		m_COMPPipeline.colorBlendEquations[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		m_COMPPipeline.colorBlendEquations[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		m_COMPPipeline.colorBlendEquations[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		m_COMPPipeline.colorBlendEquations[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

		// Add depth bias settings for solid objects
		m_COMPPipeline.rasterizationState.depthBiasEnable = VK_TRUE;
		m_COMPPipeline.rasterizationState.depthBiasConstantFactor = -1.0f;
		m_COMPPipeline.rasterizationState.depthBiasSlopeFactor = 1.0f;

		// Attachment #1 - Selection
		//m_COMPPipeline.colorBlendEnables.push_back(false);  // No blending for attachment #1
		//m_COMPPipeline.colorWriteMasks.push_back(
		//	{ VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT });
		//m_COMPPipeline.colorBlendEquations.push_back(VkColorBlendEquationEXT{});
	}
	
}

//--------------------------------------------------------------------------------------------------
// Compile the rasterizer's shaders
// Creates vertex, fragment, and wireframe shaders from the gltf_raster.slang source
void DDGIRasterizer::compileShader(Resources& resources, bool fromFile)
{
	SCOPED_TIMER(__FUNCTION__);

	// Push constant is used to pass data to the shader at each frame
	const VkPushConstantRange pushConstantRange{
		.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
		.offset = 0,
		.size = sizeof(shaderio::RasterPushConstant),
	};



	{
		std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ resources.descriptorSetLayout[0] };

		VkShaderCreateInfoEXT shaderInfo{
			.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
			.codeSize = gltf_raster_slang_sizeInBytes,
			.pCode = gltf_raster_slang,
			.pName = "MRTvertexMain",
			.setLayoutCount = uint32_t(descriptorSetLayouts.size()),
			.pSetLayouts = descriptorSetLayouts.data(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushConstantRange,
		};

		if (resources.slangCompiler.compileFile("MRT.slang"))
		{
			shaderInfo.codeSize = resources.slangCompiler.getSpirvSize();
			shaderInfo.pCode = resources.slangCompiler.getSpirv();
		}
		else
		{
			LOGE("Error compiling gltf_raster.slang\n");
		}


		VkDevice device = resources.allocator.getDevice();
		vkDestroyShaderEXT(device, m_MRTvertexShader, nullptr);
		vkDestroyShaderEXT(device, m_MRTfragmentShader, nullptr);
		vkDestroyShaderEXT(device, m_wireframeShader, nullptr);


		NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_MRTvertexShader));
		NVVK_DBG_NAME(m_MRTvertexShader);
		shaderInfo.pName = "MRTfragmentMain";
		shaderInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderInfo.nextStage = 0;
		NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_MRTfragmentShader));
		NVVK_DBG_NAME(m_MRTfragmentShader);
		//shaderInfo.pName = "fragmentWireframeMain";
		//shaderInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		//NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_wireframeShader));
		//NVVK_DBG_NAME(m_wireframeShader);
	}

	{
		std::vector<VkDescriptorSetLayout> descriptorSetLayouts{ resources.descriptorSetLayout[0], resources.gbufferDescSetlayout };

		VkShaderCreateInfoEXT shaderInfo{
			.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
			.codeSize = gltf_raster_slang_sizeInBytes,
			.pCode = gltf_raster_slang,
			.pName = "MRTvertexMain",
			.setLayoutCount = uint32_t(descriptorSetLayouts.size()),
			.pSetLayouts = descriptorSetLayouts.data(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushConstantRange,
		};

		if (resources.slangCompiler.compileFile("COMP.slang"))
		{
			shaderInfo.codeSize = resources.slangCompiler.getSpirvSize();
			shaderInfo.pCode = resources.slangCompiler.getSpirv();
		}
		else
		{
			LOGE("Error compiling gltf_raster.slang\n");
		}


		VkDevice device = resources.allocator.getDevice();
		vkDestroyShaderEXT(device, m_COMPvertexShader, nullptr);
		vkDestroyShaderEXT(device, m_COMPfragmentShader, nullptr);
		shaderInfo.pName = "COMPvertexMain";
		shaderInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		shaderInfo.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
		NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_COMPvertexShader));
		NVVK_DBG_NAME(m_COMPvertexShader);
		shaderInfo.pName = "COMPfragmentMain";
		shaderInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderInfo.nextStage = 0;
		NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_COMPfragmentShader));
		NVVK_DBG_NAME(m_COMPfragmentShader);
		
	}
}

//--------------------------------------------------------------------------------------------------
// Recording in a secondary command buffer, the raster rendering of the scene;这里改为到gbuffer中
//
void DDGIRasterizer::recordRasterScene(Resources& resources)
{
	SCOPED_TIMER(__FUNCTION__);

	createRecordCommandBuffer();

	std::vector<VkFormat> colorFormat = { resources.gBuffersDefer.getColorFormat((uint32_t)Resources::EGbuffer::epos),
										 resources.gBuffersDefer.getColorFormat((uint32_t)Resources::EGbuffer::enorm),
										resources.gBuffersDefer.getColorFormat((uint32_t)Resources::EGbuffer::euv), };

	VkCommandBufferInheritanceRenderingInfo inheritanceRenderingInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
		.colorAttachmentCount = uint32_t(colorFormat.size()),
		.pColorAttachmentFormats = colorFormat.data(),
		.depthAttachmentFormat = resources.gBuffersDefer.getDepthFormat(),
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkCommandBufferInheritanceInfo inheritInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
		.pNext = &inheritanceRenderingInfo,
	};

	VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
		.pInheritanceInfo = &inheritInfo,
	};

	NVVK_CHECK(vkBeginCommandBuffer(m_recordedSceneCmd, &beginInfo));
	renderRasterScene(m_recordedSceneCmd, resources);
	NVVK_CHECK(vkEndCommandBuffer(m_recordedSceneCmd));
}

//--------------------------------------------------------------------------------------------------
// Render the entire scene for raster. Splitting the solid and blend-able element and rendering
// on top, the wireframe if active.
// This is done in a recoded command buffer to be replay
void DDGIRasterizer::renderRasterScene(VkCommandBuffer cmd, Resources& resources)
{

	// Setting up the push constant
	m_pushConst.frameInfo = (shaderio::SceneFrameInfo*)resources.bFrameInfo.address;
	m_pushConst.skyParams = (shaderio::SkyPhysicalParameters*)resources.bSkyParams.address;
	m_pushConst.gltfScene = (shaderio::GltfScene*)resources.sceneVk.sceneDesc().address;
	m_pushConst.mouseCoord = nvapp::ElementDbgPrintf::getMouseCoord();  // Use for debugging: printf in shader
	vkCmdPushConstants(cmd, m_MRTPipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(shaderio::RasterPushConstant), &m_pushConst);

	// All dynamic states are set here
	m_MRTPipeline.cmdApplyAllStates(cmd);
	m_MRTPipeline.cmdSetViewportAndScissor(cmd, resources.gBuffersDefer.getSize());
	m_MRTPipeline.cmdBindShaders(cmd, { .vertex = m_MRTvertexShader, .fragment = m_MRTfragmentShader });
	vkCmdSetDepthTestEnable(cmd, VK_TRUE);

	// Mesh specific vertex input (can be different for each mesh)
	std::array<VkVertexInputBindingDescription2EXT, 3> bindingDescription =
	{
		VkVertexInputBindingDescription2EXT{
		 .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
		 .binding = 0,  // Position buffer binding
		 .stride = sizeof(glm::vec3),
		 .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		 .divisor = 1},
		 VkVertexInputBindingDescription2EXT{
		 .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
		 .binding = 1,  // normal buffer binding
		 .stride = sizeof(glm::vec3),
		 .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		 .divisor = 1},
		 VkVertexInputBindingDescription2EXT{
		 .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
		 .binding = 2,  // texcoord buffer binding
		 .stride = sizeof(glm::vec2),
		 .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		 .divisor = 1},
	};

	const auto& attributeDescriptions = std::to_array<VkVertexInputAttributeDescription2EXT>(
		{ {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
		{.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0} ,
		{.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = 2, .binding = 2, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0} });

	vkCmdSetVertexInputEXT(cmd, uint32_t(bindingDescription.size()), bindingDescription.data(),
		uint32_t(attributeDescriptions.size()), attributeDescriptions.data());

	// Bind the descriptor set: textures (Set: 0)
	std::array<VkDescriptorSet, 1> descriptorSets{ resources.descriptorSet};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MRTPipelineLayout, 0, descriptorSets.size(), descriptorSets.data(), 0, nullptr);
	VkBool32 blendEnable[] = { VK_TRUE,VK_TRUE,VK_TRUE };
	VkBool32 blendDisable[] = {VK_FALSE, VK_FALSE, VK_FALSE};

	// Draw the scene
	// Back-face culling with depth bias
	vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
	vkCmdSetDepthBias(cmd, -1.0f, 0.0f, 1.0f);  // Apply depth bias for solid objects
	vkCmdSetColorBlendEnableEXT(cmd, 0, 3, blendDisable);

	renderNodes(cmd, resources, resources.scene.getShadedNodes(nvvkgltf::Scene::eRasterSolid));

	// Double sided without depth bias
	vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
	vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);  // Disable depth bias for double-sided objects
	vkCmdSetColorBlendEnableEXT(cmd, 0, 3, blendDisable);
	renderNodes(cmd, resources, resources.scene.getShadedNodes(nvvkgltf::Scene::eRasterSolidDoubleSided));

	// Blendable objects without depth bias
	
	// vkCmdSetColorBlendEnableEXT(cmd, 0, 3, blendEnable);
	// renderNodes(cmd, resources, resources.scene.getShadedNodes(nvvkgltf::Scene::eRasterBlend));

	//if (m_enableWireframe)
	//{
	//	m_dynamicPipeline.cmdBindShaders(cmd, { .vertex = m_vertexShader, .fragment = m_wireframeShader });
	//	vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blendDisable);
	//	vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);  // Disable depth bias for wireframe
	//	vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_LINE);
	//	renderNodes(cmd, resources, resources.scene.getShadedNodes(nvvkgltf::Scene::eRasterAll));
	//}
}

//--------------------------------------------------------------------------------------------------
// Raster commands are recorded to be replayed, this allocates that command buffer
//
void DDGIRasterizer::createRecordCommandBuffer()
{
	VkCommandBufferAllocateInfo alloc_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = m_commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
		.commandBufferCount = 1,
	};
	vkAllocateCommandBuffers(m_device, &alloc_info, &m_recordedSceneCmd);
}

//--------------------------------------------------------------------------------------------------
// Freeing the raster recoded command buffer
//
void DDGIRasterizer::freeRecordCommandBuffer()
{
	vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_recordedSceneCmd);
	m_recordedSceneCmd = VK_NULL_HANDLE;
}

//void compileShader(Resources& resources, bool fromFile)
//{
//	SCOPED_TIMER(__FUNCTION__);
//
//	// Push constant is used to pass data to the shader at each frame
//	const VkPushConstantRange pushConstantRange{
//		.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
//		.offset = 0,
//		.size = sizeof(shaderio::RasterPushConstant),
//	};
//
//	std::vector<VkDescriptorSetLayout> descriptorSetLayoutsMRT{ resources.descriptorSetLayout[0] };
//	std::vector<VkDescriptorSetLayout> descriptorSetLayoutsCOMP{ resources.descriptorSetLayout[0], resources.gbufferDescSetlayout };
//	VkShaderCreateInfoEXT shaderInfo{
//		.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
//		.stage = VK_SHADER_STAGE_VERTEX_BIT,
//		.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
//		.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
//		.codeSize = gltf_raster_slang_sizeInBytes,
//		.pCode = gltf_raster_slang,
//		.pName = "vertexMain",
//		.setLayoutCount = uint32_t(descriptorSetLayoutsMRT.size()),
//		.pSetLayouts = descriptorSetLayoutsMRT.data(),
//		.pushConstantRangeCount = 1,
//		.pPushConstantRanges = &pushConstantRange,
//	};
//	fromFile = true;
//
//
//
//	if (fromFile)
//	{
//
//		//if (resources.slangCompiler.compileFile("gltf_raster.slang"))
//		//{
//		//	shaderInfo.codeSize = resources.slangCompiler.getSpirvSize();
//		//	shaderInfo.pCode = resources.slangCompiler.getSpirv();
//		//}
//		//else
//		//{
//		//	LOGE("Error compiling gltf_raster.slang\n");
//		//}
//		std::array<std::string, 4> shader_glsl_files = {
//			"m_MRT.vert.glsl",
//			"m_MRT.frag.glsl",
//			"m_Compositioin.vert.glsl",
//			"m_Composition.frag.glsl",
//		};
//		VkDevice device = resources.allocator.getDevice();
//		vkDestroyShaderEXT(device, m_MRTvertexShader, nullptr);
//		vkDestroyShaderEXT(device, m_MRTfragmentShader, nullptr);
//		vkDestroyShaderEXT(device, m_COMPvertexShader, nullptr);
//		vkDestroyShaderEXT(device, m_COMPfragmentShader, nullptr);
//		vkDestroyShaderEXT(device, m_wireframeShader, nullptr);
//		for (int i = 0; i < shader_glsl_files.size(); i++) {
//			shaderc_shader_kind shader_kind = shaderc_glsl_vertex_shader;
//			VkShaderStageFlagBits shader_stage = VK_SHADER_STAGE_VERTEX_BIT;
//			VkShaderStageFlags next_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
//			uint32_t setLayoutCount = uint32_t(descriptorSetLayoutsMRT.size());
//			VkDescriptorSetLayout* setLayouts = descriptorSetLayoutsMRT.data();
//			if (i % 2 == 1) {
//				shader_kind = shaderc_glsl_fragment_shader;
//				shader_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
//				next_stage = 0;
//
//			}
//			if (i >= 2) {
//				setLayoutCount = uint32_t(descriptorSetLayoutsCOMP.size());
//				VkDescriptorSetLayout* pSetLayouts = descriptorSetLayoutsCOMP.data();
//			}
//			shaderc::Compiler compiler;
//
//			std::string shaderfile = std::string("../shaders/") + shader_glsl_files[i];
//			std::string shaderCodes = readFile(shaderfile);
//
//
//			auto result = compiler.CompileGlslToSpv(shaderCodes, shader_kind, shaderfile.c_str());
//			auto errorInfo = result.GetErrorMessage();
//			std::span<const uint32_t> spv = { result.begin(), size_t(result.end() - result.begin()) * 4 };
//			if (errorInfo == "") {
//				shaderInfo.pCode = spv.data();;
//				shaderInfo.codeSize = spv.size();
//				shaderInfo.pName = shader_glsl_files[i].substr(0, shader_glsl_files[i].find_first_of(".")).c_str();
//			}
//			else {
//				LOGE("Error compiling shader_%s, error:%s\n", shader_glsl_files[i].c_str(), errorInfo.c_str());
//			}
//			switch (i) {
//			case 0:
//			{
//				NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_MRTvertexShader));
//				NVVK_DBG_NAME(m_MRTvertexShader);
//				break;
//			}
//			case 1:
//			{
//				NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_MRTfragmentShader));
//				NVVK_DBG_NAME(m_MRTfragmentShader);
//				break;
//			}
//			case 2:
//			{
//				NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_COMPvertexShader));
//				NVVK_DBG_NAME(m_COMPvertexShader);
//				break;
//			}
//			case 3:
//			{
//				NVVK_CHECK(vkCreateShadersEXT(device, 1U, &shaderInfo, nullptr, &m_COMPfragmentShader));
//				NVVK_DBG_NAME(m_COMPfragmentShader);
//				break;
//			}
//			}
//		}
//	}
//}
