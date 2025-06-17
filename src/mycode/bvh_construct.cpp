#define VMA_IMPLEMENTATION
#include "nvvk/descriptorsets_vk.hpp"               // Descriptor set helper
#include "nvvkhl/alloc_vma.hpp"                     // Our allocator
#include "nvvkhl/application.hpp"                   // For Application and IAppElememt
#include "nvvkhl/gbuffer.hpp"                       // G-Buffer helper
#include "nvvkhl/shaders/dh_comp.h"                 // Workgroup size and count
#include "nvvkhl/element_benchmark_parameters.hpp"  // For testing

#include "../vk_context.hpp"
#include "nvvk/extensions_vk.hpp"

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