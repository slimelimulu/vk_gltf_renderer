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

// shaders
const std::vector<uint32_t> mc_comp_shd{};
std::string g_inFilename;
std::shared_ptr<nvvkhl::SampleAppLog>     g_elemLogger;
namespace LBVH {
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

	private:
		nvvkhl::Application* m_app = nullptr;
		std::unique_ptr<nvvkhl::AllocVma> m_alloc;
		std::unique_ptr<nvvk::DescriptorSetContainer> m_dset;
		std::array<VkShaderEXT, 1> m_shaders = {};

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

int main(int argc, char** argv) {
	nvvkhl::ApplicationCreateInfo appInfo;

	nvh::CommandLineParser cli(PROJECT_NAME);
	cli.addArgument({ "--headless" }, &appInfo.headless, "Run in headless mode");
	cli.addArgument({ "--frames" }, &appInfo.headlessFrameCount, "Number of frames to render in headless mode");
	cli.parse(argc, argv);

	// Extension feature needed.
	VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjFeature{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT };
	// Setting up how Vulkan context must be created
	VkContextSettings vkSetup;
	if (!appInfo.headless)
	{
		nvvkhl::addSurfaceExtensions(vkSetup.instanceExtensions);  // WIN32, XLIB, ...
		vkSetup.deviceExtensions.push_back({ VK_KHR_SWAPCHAIN_EXTENSION_NAME });
	}
	vkSetup.instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	vkSetup.deviceExtensions.push_back({ VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME });
	vkSetup.deviceExtensions.push_back({ VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &shaderObjFeature });

	// Create the Vulkan context
	auto vkContext = std::make_unique<VulkanContext>(vkSetup);
	load_VK_EXTENSIONS(vkContext->getInstance(), vkGetInstanceProcAddr, vkContext->getDevice(), vkGetDeviceProcAddr);  // Loading the Vulkan extension pointers
	if (!vkContext->isValid())
		std::exit(0);

	// Setting up how the the application must be created
	appInfo.name = "LBVH_Construction";
	appInfo.useMenu = false;
	appInfo.instance = vkContext->getInstance();
	appInfo.device = vkContext->getDevice();
	appInfo.physicalDevice = vkContext->getPhysicalDevice();
	appInfo.queues = vkContext->getQueueInfos();

	auto app = std::make_unique<nvvkhl::Application>(appInfo);                    // Create the application
	auto test = std::make_shared<nvvkhl::ElementBenchmarkParameters>(argc, argv);  // Create the test framework
	app->addElement(test);                                                         // Add the test element (--test ...)
	app->addElement(std::make_shared<LBVH::ConstructBVH>());                       // Add our sample to the application
	app->run();  // Loop infinitely, and call IAppElement virtual functions at each frame

	app.reset();  // Clean up
	//vkContext.deinit();
	vkContext.reset();

	return test->errorCode();



}

namespace LBVH {
	void ConstructBVH::onAttach(nvvkhl::Application* app) {
		m_app = app;
		m_alloc = std::make_unique<nvvkhl::AllocVma>(VmaAllocatorCreateInfo{
			.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
			.physicalDevice = app->getPhysicalDevice(),
			.device = app->getDevice(),
			.instance = app->getInstance(),
			});  // Allocator
		m_dset = std::make_unique<nvvk::DescriptorSetContainer>(m_app->getDevice());

		createShaderObjectAndLayout();
	
	}

	nvh::Bbox generateElements(const tinygltf::Model& model, std::vector<Element>& element, nvh::Bbox* extent) {
		// fetch vertices from model
		// https://github.com/KhronosGroup/glTF-Tutorials/blob/main/gltfTutorial/README.md
		// 参考scene.cpp - 656
		glm::vec3 minVal = { -1., -1., -1. };
		glm::vec3 maxVal = { 1., 1., 1. };
		for (const tinygltf::Accessor& accessor : model.accessors) {
			if (!accessor.minValues.empty()) {
				glm::vec3 localMin = {
					accessor.minValues[0],
					accessor.minValues[1],
					accessor.minValues[2],
				};
				minVal = glm::min(minVal, localMin);
			}
			if (!accessor.maxValues.empty()) {
				glm::vec3 localmax = {
					accessor.maxValues[0],
					accessor.maxValues[1],
					accessor.maxValues[2],
				};
				maxVal = glm::max(maxVal, localmax);
			}
		}
		return nvh::Bbox{ minVal, maxVal };

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
}



int main(int argc, char** argv) {
	nvvkhl::ApplicationCreateInfo appInfo;
	appInfo.name = "construct bvh";
	appInfo.vSync = false;

	nvh::CommandLineParser cli(appInfo.name);
	cli.addFilename(".gltf", &g_inFilename, "Load GLTF | GLB files");

	// 使用logger
	g_elemLogger = std::make_shared<nvvkhl::SampleAppLog>();
	nvprintSetCallback([](int level, const char* fmt) { g_elemLogger->addLog(level, "%s", fmt); });
	g_elemLogger->setLogLevel(LOGBITS_INFO);

	// Vulkan Context creation information
	VkContextSettings vkSetup;
	nvvkhl::addSurfaceExtensions(vkSetup.instanceExtensions);
	vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	vkSetup.instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
	app->addElement(g_emeCamera);
	app->addElement(g_elemeDebugPrintf);
	app->addElement(std::make_unique<nvvkhl::ElementLogger>(g_elemLogger.get(), false));  // Add logger window
	app->addElement(std::make_unique<nvvkhl::ElementNvml>(false));                        // Add GPU monitor

	app->run();

	// clean up
	//
	app.reset();
	vkContext.reset();
	return 0;
}