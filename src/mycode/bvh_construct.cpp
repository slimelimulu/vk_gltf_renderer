#define VMA_IMPLEMENTATION
#include "nvvk/descriptorsets_vk.hpp"               // Descriptor set helper
#include "nvvkhl/alloc_vma.hpp"                     // Our allocator
#include "nvvkhl/application.hpp"                   // For Application and IAppElememt
#include "nvvkhl/gbuffer.hpp"                       // G-Buffer helper
#include "nvvkhl/shaders/dh_comp.h"                 // Workgroup size and count
#include "nvvkhl/element_benchmark_parameters.hpp"  // For testing

#include "../vk_context.hpp"
#include "nvvk/extensions_vk.hpp"
#include "LBVH.h"

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

	void generateElements(std::vector<Element>& element, AABB* extent) {
		// 从vertices中获取AABB
	}

	void ConstructBVH::createShaderObjectAndLayout() {
		// for MortonCodes

	}
}