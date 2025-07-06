#pragma once
#include <nvapp/application.hpp>
#include <nvvk/graphics_pipeline.hpp>
#include <nvshaders_host/sky.hpp>

// Shader Input/Output
namespace shaderio {
	using namespace glm;
#include "shaders/shaderio.h"  // Shared between host and device
}  // namespace shaderio


#include "resources.hpp"
#include "renderer_base.hpp"


class DDGIRasterizer : public BaseRenderer
{
public:
	DDGIRasterizer() = default;
	virtual ~DDGIRasterizer() = default;

	void onAttach(Resources& resources, nvvk::ProfilerGpuTimer* profiler) override;
	void onDetach(Resources& resources) override;
	void onResize(VkCommandBuffer cmd, const VkExtent2D& size, Resources& resources) override;
	bool onUIRender(Resources& resources) override;
	void onRender(VkCommandBuffer cmd, Resources& resources) override;

	void pushDescriptorSet(VkCommandBuffer cmd, Resources& resources);
	void compileShader(Resources& resources, bool fromFile = true) override;
	void createPipeline(Resources& resources) override;
	void freeRecordCommandBuffer();

	// Register command line parameters
	void registerParameters(nvutils::ParameterRegistry* paramReg);

private:
	void renderNodes(VkCommandBuffer cmd, Resources& resources, const std::vector<uint32_t>& nodeIDs);
	void recordRasterScene(Resources& resources);
	void renderRasterScene(VkCommandBuffer cmd, Resources& resources);
	void createRecordCommandBuffer();


	VkDevice         m_device{};                 // Vulkan device
	VkCommandBuffer  m_recordedSceneCmd{};       // Command buffer for recording the scene
	VkCommandPool    m_commandPool{};            // Command pool for recording the scene
	VkPipelineLayout m_graphicPipelineLayout{};  // The pipeline layout use with graphics pipeline

	nvvk::GraphicsPipelineState m_dynamicPipeline;  // Graphics pipeline state
	nvvk::DescriptorBindings    m_descBind;         // Descriptor bindings

	shaderio::RasterPushConstant m_pushConst{};  // Reusing the same push constant structure for now


	VkShaderEXT m_MRTvertexShader{};     // Vertex shader
	VkShaderEXT m_MRTfragmentShader{};   // Fragment shader
	VkShaderEXT m_COMPvertexShader{};     // Vertex shader
	VkShaderEXT m_COMPfragmentShader{};   // Fragment shader
	VkShaderEXT m_wireframeShader{};  // Wireframe shader
	//VkShaderEXT m_vertexShader{};     // Vertex shader
	//VkShaderEXT m_fragmentShader{};   // Fragment shader
	nvshaders::SkyPhysical m_skyPhysical;  // Sky physical


	// UI
	bool m_enableWireframe = false;
	bool m_useRecordedCmd = true;  // Use recorded command buffer for rendering
};