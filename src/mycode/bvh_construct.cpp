
#include <thread>

#if defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define VMA_IMPLEMENTATION
#include "nvvk/descriptorsets_vk.hpp"               // Descriptor set helper
#include "nvvkhl/alloc_vma.hpp"                     // Our allocator
#include "nvvkhl/application.hpp"                   // For Application and IAppElememt
#include "nvvkhl/gbuffer.hpp"                       // G-Buffer helper
#include "nvvkhl/shaders/dh_comp.h"                 // Workgroup size and count
#include "nvvkhl/element_benchmark_parameters.hpp"  // For testing
#include "nvvkhl/element_logger.hpp"
#include "../vk_context.hpp"
#include "nvvk/extensions_vk.hpp"
#include "LBVH.h"
#include "tiny_gltf.h"
#include "nvvkhl/element_camera.hpp"
// ImGui headers
#include "imgui/imgui_axis.hpp"
#include "imgui/imgui_camera_widget.h"
#include "imgui/imgui_helper.h"

// Application specific headers
#include "../busy_window.hpp"
#include "../renderer.hpp"
#include "../scene.hpp"
#include "../settings.hpp"
#include "../utilities.hpp"
#include "stb_image.h"
// #include "doc/app_icon_png.h"
#include "../collapsing_header_manager.h"
#include "perproject_globals.hpp"
#include "nvvk/nsight_aftermath_vk.hpp"
#include "../imgui_mouse_state.hpp"

std::shared_ptr<nvvkhl::ElementCamera> g_elemCamera;
std::shared_ptr<nvvkhl::SampleAppLog>     g_elemLogger;
namespace LBVH {
	// shaders
	const std::vector<uint32_t> mc_comp_shd{};
	std::string g_inFilename;
	//std::shared_ptr<nvvkhl::ElementCamera>    g_elemCamera;    // The camera element (UI and movement)
	
	using namespace gltfr;
	class ConstructBVH : public nvvkhl::IAppElement {
		
	public:
		ConstructBVH() = default;
		~ConstructBVH() override = default;

		void onAttach(nvvkhl::Application* app) override;

		void onDetach() override;

		void onUIRender() override;

		void onRender(VkCommandBuffer cmd);

		void onUIMenu() override;

		void onResize(uint32_t width, uint32_t height) override;

		void createShaderObjectAndLayout();

		void onLastHeadlessFrame() override;

		void initPushConstants();

		void  generateElements(nvh::Bbox& extent);

	private:
		nvvkhl::Application* m_app = nullptr;
		std::unique_ptr<nvvkhl::AllocVma> m_alloc;
		std::unique_ptr<nvvk::DescriptorSetContainer> m_dset;
		std::array<VkShaderEXT, 1> m_shaders = {};
		std::vector<Element> m_elements;

		Resources   m_resources;
		Scene       m_scene;

	};
	// push constants
	struct PCMortonCodes {
		uint32_t g_num_elements;
		float g_min_x;
		float g_min_y;
		float g_min_z;
		float g_max_x;
		float g_max_y;
		float g_max_z;
	};

	PCMortonCodes pcMortonCodes{};

	struct PCRadixSort {
		uint32_t g_num_elements;
	};

}


namespace LBVH {
	void ConstructBVH::onAttach(nvvkhl::Application* app) {
		m_app = app;
		// Getting all required resources
		gltfr::VulkanInfo ctx;
		ctx.device = app->getDevice();
		ctx.physicalDevice = app->getPhysicalDevice();
		ctx.GCT0 = { app->getQueue(0).queue, app->getQueue(0).familyIndex };  // See creation of queues in main()
		ctx.compute = { app->getQueue(1).queue, app->getQueue(1).familyIndex };
		ctx.transfer = { app->getQueue(2).queue, app->getQueue(2).familyIndex };

		m_resources.init(ctx);
		m_scene.init(m_resources);

		nvvk::ResourceAllocator* alloc = m_resources.m_allocator.get();
		uint32_t                 c_queue_index = ctx.compute.familyIndex;
		if (g_inFilename.empty()) {
			g_inFilename = "E:\\gltf_models\\glTF-Sample-Models-main\\2.0\\SimpleMeshes\\glTF\\SimpleMeshes.gltf";
		}

		if (!g_inFilename.empty()) {
			m_scene.load(m_resources, g_inFilename);
		}

		// createShaderObjectAndLayout();

		// push constant
		initPushConstants();
		
	
	}


	// --------------------------------------------------
	// 将所有三角形的AABB添加到m_elements中，并计算出总的AABB
	void ConstructBVH::generateElements(nvh::Bbox& extent) {
		// fetch vertices from model
		// 参考scene.cpp - 656
		// 后续需要看gltfscene.cpp
		extent = m_scene.m_gltfScene->getSceneBounds();
		

	}

	//--------------------------------------------------
	// 初始化push constants
	void ConstructBVH::initPushConstants() {
		nvh::Bbox scene_bbox;
		m_elements.clear();
		generateElements(scene_bbox);
		exit(0);
	}


	// --------------------------------------------------
	// 创建：descriptor set layout, push_constant_range, 
	// shader, 
	void ConstructBVH::createShaderObjectAndLayout() {
		// for MortonCodes
		VkPushConstantRange push_constant_ranges = { 
			.stageFlags = VK_SHADER_STAGE_ALL, 
			.offset = 0,
			.size = sizeof(PCMortonCodes),	 
		};
		// layout
		m_dset->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL);
		m_dset->addBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL);
		m_dset->initLayout(VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
		m_dset->initPipeLayout(1, &push_constant_ranges);

		// Computer shader description
		std::vector<VkShaderCreateInfoEXT> shaderCreateInfos;
		shaderCreateInfos.push_back(VkShaderCreateInfoEXT{
			 .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
			.pNext = NULL,
			.flags = VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.nextStage = 0,
			.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
			.codeSize = mc_comp_shd.size(),
			.pCode = mc_comp_shd.data(),
			.pName = "morton code",
			.setLayoutCount = 1,
			.pSetLayouts = &m_dset->getLayout(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_ranges,
			.pSpecializationInfo = NULL,
		});
		NVVK_CHECK(vkCreateShadersEXT(m_app->getDevice(), 1, shaderCreateInfos.data(), NULL, m_shaders.data()));

	}

	void ConstructBVH::onDetach() {

	}
	void ConstructBVH::onUIRender() {

	}
	void ConstructBVH::onUIMenu() {

	}
	void ConstructBVH::onResize(uint32_t width, uint32_t height) {

	}
	void ConstructBVH::onLastHeadlessFrame() {

	}
	void ConstructBVH::onRender(VkCommandBuffer cmd) {
	}
}



int main_(int argc, char** argv) {
	nvvkhl::ApplicationCreateInfo appInfo;
	appInfo.name = "construct bvh";
	appInfo.vSync = false;

	nvh::CommandLineParser cli(appInfo.name);
	cli.addFilename(".gltf", &LBVH::g_inFilename, "Load GLTF | GLB files");

	// 使用logger
	g_elemLogger = std::make_shared<nvvkhl::SampleAppLog>();
	nvprintSetCallback([](int level, const char* fmt) { g_elemLogger->addLog(level, "%s", fmt); });
	g_elemLogger->setLogLevel(LOGBITS_INFO);

	// Vulkan Context creation information
	VkContextSettings vkSetup;
	nvvkhl::addSurfaceExtensions(vkSetup.instanceExtensions);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	vkSetup.instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// All Vulkan extensions required by the sample
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accel_feature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline_feature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
	VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
	VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT };
	VkPhysicalDeviceNestedCommandBufferFeaturesEXT nestedCmdFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT };
	VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV reorderFeature{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV };
	vkSetup.deviceExtensions.emplace_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, &accel_feature);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, &rt_pipeline_feature);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_QUERY_EXTENSION_NAME, &ray_query_features);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, &baryFeatures, false);
	vkSetup.deviceExtensions.emplace_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &shaderObjFeature);
	vkSetup.deviceExtensions.emplace_back(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME, &nestedCmdFeature);
	vkSetup.deviceExtensions.emplace_back(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, &reorderFeature, false);

	// Request the creation of all needed queues
	vkSetup.queues = { VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT,  // GTC for rendering
					  VK_QUEUE_COMPUTE_BIT,                                                  // Compute
					  VK_QUEUE_TRANSFER_BIT };
	ValidationSettings vvlInfo{};
	// use debug print
	vvlInfo.validate_gpu_based = { "GPU_BASED_DEBUG_PRINTF" };  // Adding the debug printf extension
	vvlInfo.printf_verbose = VK_FALSE;
	vvlInfo.printf_to_stdout = VK_FALSE;
	vvlInfo.printf_buffer_size = 1024;
	vvlInfo.gpuav_reserve_binding_slot = false;
	vvlInfo.message_id_filter = { 0x76589099 };  // Truncate the message when too long

	ValidationSettings vvl(std::move(vvlInfo));
	vkSetup.instanceCreateInfoExt = vvl.buildPNextChain();  // Adding the validation layer settings

	// Creating the Vulkan context
	auto vkContext = std::make_unique<VulkanContext>(vkSetup);
	if (!vkContext->isValid())
	{
		LOGE("Error in Vulkan context creation\n");
		std::exit(0);
	}

	// Loading Vulkan extension pointers
	load_VK_EXTENSIONS(vkContext->getInstance(), vkGetInstanceProcAddr, vkContext->getDevice(), vkGetDeviceProcAddr);

	// Setup the application information
	appInfo.instance = vkContext->getInstance();
	appInfo.device = vkContext->getDevice();
	appInfo.physicalDevice = vkContext->getPhysicalDevice();
	appInfo.queues = vkContext->getQueueInfos();
	appInfo.imguiConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	appInfo.vSync = true;

	auto app = std::make_unique < nvvkhl::Application >(appInfo);

	g_elemCamera = std::make_shared<nvvkhl::ElementCamera>();

	auto bvhConstructor = std::make_shared<LBVH::ConstructBVH>();

	app->addElement(bvhConstructor);
	app->addElement(g_elemCamera);
	// app->addElement(g_elemeDebugPrintf);
	app->addElement(std::make_unique<nvvkhl::ElementLogger>(g_elemLogger.get(), false));  // Add logger window
	// app->addElement(std::make_unique<nvvkhl::ElementNvml>(false));                        // Add GPU monitor

	app->run();

	// clean up
	//
	app.reset();
	vkContext.reset();
	return 0;
}