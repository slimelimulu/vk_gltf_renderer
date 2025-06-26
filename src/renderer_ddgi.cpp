#include <glm/glm.hpp>

 // Purpose: Raster renderer implementation
#include "nvvkhl/shaders/dh_lighting.h"
namespace DH {
#include "shaders/device_host.h"  // Include the device/host structures
}  // namespace DH

// Pre-compiled shaders
#include "_autogen/raster.frag.glsl.h"
#include "_autogen/raster.vert.glsl.h"
#include "_autogen/raster_overlay.frag.glsl.h"


#include "imgui.h"

#include "nvh/cameramanipulator.hpp"
#include "nvh/timesampler.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/dynamicrendering_vk.hpp"
#include "nvvk/error_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"
#include "nvvk/sbtwrapper_vk.hpp"
#include "nvvkhl/shaders/dh_tonemap.h"
#include "shaders/dh_bindings.h"

#include "renderer.hpp"
#include "silhouette.hpp"
#include "nvvk/shaders_vk.hpp"
#include "collapsing_header_manager.h"



constexpr auto RASTER_SS_SIZE = 2;  // Change this for the default Super-Sampling resolution multiplier for raster;

namespace PE = ImGuiH::PropertyEditor;

namespace gltfr {

    struct RasterSettings1
    {
        bool            ssao{ true };
        //bool             showWireframe{ false };
        //bool             useSuperSample{ true };
        DH::EDebugMethod dbgMethod{ DH::eDbgMethod_none };
    } g_rasterSettings1;

    // This shows path tracing using ray tracing
    class RendererDDGIRaster : public Renderer
    {


    public:
        RendererDDGIRaster() = default;
        ~RendererDDGIRaster() { deinit(); };

        bool init(Resources& res, Scene& scene) override;
        void deinit(Resources& /*res*/) override { deinit(); }
        void render(VkCommandBuffer cmd, Resources& res, Scene& scene, Settings& settings, nvvk::ProfilerVK& profiler) override;


        bool onUI() override;
        void handleChange(Resources& res, Scene& scene) override;

        VkDescriptorImageInfo getOutputImage() const override { return m_gSimpleBuffers->getDescriptorImageInfo(); }

        bool reloadShaders(Resources& res, Scene& scene) override;

    private:
        void createRasterPipeline(Resources& res, Scene& scene);
        void createRecordCommandBuffer();
        void freeRecordCommandBuffer();
        void recordRasterScene(Scene& scene);
        void renderNodes(VkCommandBuffer cmd, Scene& scene, const std::vector<uint32_t>& nodeIDs);
        void renderRasterScene(VkCommandBuffer cmd, Scene& scene);
        bool initShaders(Resources& res, bool reload);
        void deinit();
        void createGBuffer(Resources& res, Scene& scene);

        enum PipelineType
        {
            eRasterSolid,
            eRasterSolidDoubleSided,
            //eRasterBlend,
            //eRasterWireframe
        };

        enum GBufferType
        {
            //eSuperSample,
            //eSilhouette
        };

        DH::PushConstantRaster m_pushConst{};

        std::unique_ptr<nvvkhl::PipelineContainer> m_rasterPipepline{};      // Raster scene pipeline
        //std::unique_ptr<nvvkhl::GBuffer>           m_gSuperSampleBuffers{};  // G-Buffers: RGBA32F, R8, Depth32F
        std::unique_ptr<nvvkhl::GBuffer>           m_gSimpleBuffers{};       // G-Buffers: RGBA32F, R8, Depth32F
        std::unique_ptr<nvvk::DebugUtil>           m_dbgUtil{};
        //std::unique_ptr<Silhouette>                m_silhouette{};

        enum ShaderStages
        {
            eVertex,
            eFragment,
            eFragmentOverlay,
            // Last entry is the number of shaders
            eShaderGroupCount
        };
        std::vector<shaderc::SpvCompilationResult>    m_spvShader;
        std::array<VkShaderModule, eShaderGroupCount> m_shaderModules{};

        VkCommandBuffer m_recordedSceneCmd{ VK_NULL_HANDLE };
        VkDevice        m_device{ VK_NULL_HANDLE };
        VkCommandPool   m_commandPool{ VK_NULL_HANDLE };
    };


//--------------------------------------------------------------------------------------------------
// vertex shader和原来的一样
// fragment shader只进行ssao，然后

bool RendererDDGIRaster::initShaders(Resources& res, bool reload)
{
    nvh::ScopedTimer st(__FUNCTION__);

    if (res.hasGlslCompiler() && (reload || g_forceExternalShaders))
    {
        // Loading the shaders
        m_spvShader.resize(eShaderGroupCount);
        m_spvShader[eVertex] = res.compileGlslShader("raster.vert.glsl", shaderc_shader_kind::shaderc_vertex_shader);
        m_spvShader[eFragment] = res.compileGlslShader("raster.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);
        m_spvShader[eFragmentOverlay] = res.compileGlslShader("raster_overlay.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);

        for (size_t i = 0; i < m_spvShader.size(); i++)
        {
            auto& s = m_spvShader[i];
            if (s.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                LOGE("Error when loading shaders\n");
                LOGE("Error %s\n", s.GetErrorMessage().c_str());
                return false;
            }
            m_shaderModules[i] = res.createShaderModule(s);
        }
    }
    else
    {
        const auto& vert_shd = std::vector<uint32_t>{ std::begin(raster_vert_glsl), std::end(raster_vert_glsl) };
        const auto& frag_shd = std::vector<uint32_t>{ std::begin(raster_frag_glsl), std::end(raster_frag_glsl) };
        const auto& overlay_shd = std::vector<uint32_t>{ std::begin(raster_overlay_frag_glsl), std::end(raster_overlay_frag_glsl) };

        m_shaderModules[eVertex] = nvvk::createShaderModule(m_device, vert_shd);
        m_shaderModules[eFragment] = nvvk::createShaderModule(m_device, frag_shd);
        m_shaderModules[eFragmentOverlay] = nvvk::createShaderModule(m_device, overlay_shd);
    }

    m_dbgUtil->DBG_NAME(m_shaderModules[eVertex]);
    m_dbgUtil->DBG_NAME(m_shaderModules[eFragment]);
    m_dbgUtil->DBG_NAME(m_shaderModules[eFragmentOverlay]);

    return true;
}

}
