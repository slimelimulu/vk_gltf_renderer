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
//#include "silhouette.hpp"
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
            //eRasterSolid,
            //eRasterSolidDoubleSided,
            //eRasterBlend,
            
            mDeferredSolid,
            mDeferredDoubleSided,
            
            //eRasterWireframe
        };

        enum GBufferType
        {
            //eSuperSample,
            //eSilhouette
        };

        DH::PushConstantRaster m_pushConst{};

        std::unique_ptr<nvvkhl::PipelineContainer> m_rasterPipeplineMRT{};      // Raster scene pipeline
        std::unique_ptr<nvvkhl::PipelineContainer> m_rasterPipeplineCOMP{};      // Raster scene pipeline
        //std::unique_ptr<nvvkhl::GBuffer>           m_gSuperSampleBuffers{};  // G-Buffers: RGBA32F, R8, Depth32F
        std::unique_ptr<nvvkhl::GBuffer>           m_gSimpleBuffers{};       // G-Buffers: RGBA32F, R8, Depth32F
        std::unique_ptr<nvvkhl::GBuffer>            m_gbuffer{}; // position, norm, ...
        std::unique_ptr<nvvk::DebugUtil>           m_dbgUtil{};
        //std::unique_ptr<Silhouette>                m_silhouette{};

        enum ShaderStages
        {
            eVertex,
            eFragment,
            eFragmentOverlay,
            // Last entry is the number of shaders
            mDeferVertex,
            mDeferFrag,
            mComposeVertex,
            mComposeFrag,
            eShaderGroupCount
        };
        std::vector<shaderc::SpvCompilationResult>    m_spvShader;
        std::array<VkShaderModule, eShaderGroupCount> m_shaderModules{};
        std::unique_ptr<nvvk::DescriptorSetContainer>  m_dset;  // Descriptor set

        VkCommandBuffer m_recordedSceneCmd{ VK_NULL_HANDLE };
        VkDevice        m_device{ VK_NULL_HANDLE };
        VkCommandPool   m_commandPool{ VK_NULL_HANDLE };
    };


//--------------------------------------------------------------------------------------------------
// vertex shader和原来的一样
// fragment shader只进行ssao

bool RendererDDGIRaster::initShaders(Resources& res, bool reload)
{
    nvh::ScopedTimer st(__FUNCTION__);
    reload = true;
    if (res.hasGlslCompiler() && (reload))
    {
        // Loading the shaders
        m_spvShader.resize(eShaderGroupCount);
        m_spvShader[eVertex] = res.compileGlslShader("raster.vert.glsl", shaderc_shader_kind::shaderc_vertex_shader);
        m_spvShader[eFragment] = res.compileGlslShader("raster.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);
        m_spvShader[eFragmentOverlay] = res.compileGlslShader("raster_overlay.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);
        m_spvShader[mDeferVertex]   = res.compileGlslShader("m_deferred.vert.glsl", shaderc_shader_kind::shaderc_vertex_shader);
        m_spvShader[mDeferFrag]     = res.compileGlslShader("m_deferred.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);
        m_spvShader[mComposeVertex]   = res.compileGlslShader("m_debugGbuffer.vert.glsl", shaderc_shader_kind::shaderc_vertex_shader);
        m_spvShader[mComposeFrag]     = res.compileGlslShader("m_debugGbuffer.frag.glsl", shaderc_shader_kind::shaderc_fragment_shader);
            
        for (size_t i = 0; i < m_spvShader.size(); i++)
        {
            auto& s = m_spvShader[i];
            if (s.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                LOGE("Error when loading shader_%s\n", std::to_string(i).c_str());
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
    m_dbgUtil->DBG_NAME(m_shaderModules[mComposeVertex]);
    m_dbgUtil->DBG_NAME(m_shaderModules[mComposeFrag]);

    return true;
}

bool RendererDDGIRaster::reloadShaders(Resources& res, Scene& scene)
{
    if (!initShaders(res, true))
        return false;
    deinit();
    createRasterPipeline(res, scene);
    freeRecordCommandBuffer();
    return true;
}

//--------------------------------------------------------------------------------------------------
// Initialize the rasterizer，只有SimpleBuffers
//
bool RendererDDGIRaster::init(Resources& res, Scene& scene)
{
    m_device = res.ctx.device;
    m_commandPool = res.m_tempCommandPool->getCommandPool();
    m_dbgUtil = std::make_unique<nvvk::DebugUtil>(m_device);
    // Create descriptorlayout for composition
    m_dset = std::make_unique<nvvk::DescriptorSetContainer>(m_device);
    m_dset->addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_dset->addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_dset->addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    // m_dset->addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    // m_dset->addBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_dset->initLayout();
    m_dset->initPool(1);  // two frames - allow to change on the fly
    if (!initShaders(res, false))
    {
        return false;
    }



    m_gSimpleBuffers = std::make_unique<nvvkhl::GBuffer>(m_device, res.m_allocator.get());
    m_gbuffer = std::make_unique<nvvkhl::GBuffer>(m_device, res.m_allocator.get());
    createGBuffer(res, scene);
    createRasterPipeline(res, scene);
    return true;
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocated resources
//
void RendererDDGIRaster::deinit()
{
    m_dset->deinit();
    if (m_rasterPipeplineMRT)
        m_rasterPipeplineMRT->destroy(m_device);

    m_rasterPipeplineMRT.reset();
    if (m_rasterPipeplineCOMP)
        m_rasterPipeplineCOMP->destroy(m_device);

    m_rasterPipeplineCOMP.reset();
}

//--------------------------------------------------------------------------------------------------
// 创建一个simplegbuffer即可
//
void RendererDDGIRaster::createGBuffer(Resources& res, Scene& scene)
{
    nvh::ScopedTimer st(std::string(__FUNCTION__));

    static VkFormat depthFormat = nvvk::findDepthFormat(res.ctx.physicalDevice);  // Not all depth are supported

    // Normal size G-Buffer in which the super-sampling will be blitzed
    m_gSimpleBuffers->destroy();
    m_gSimpleBuffers->create(res.m_finalImage->getSize(), { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8_UNORM }, depthFormat);

    m_gbuffer->destroy();
    m_gbuffer->create(res.m_finalImage->getSize(),
        {
            VK_FORMAT_R32G32B32A32_SFLOAT, // position
            VK_FORMAT_R32G32B32A32_SFLOAT, // normal + materialid
            //VK_FORMAT_R32G32B32A32_SFLOAT, // tangent
            //VK_FORMAT_R8G8B8A8_UNORM, // albedo
            VK_FORMAT_R32G32B32A32_SFLOAT, // uv
        }, depthFormat);

    //scene.m_sky->setOutImage(m_gSimpleBuffers->getDescriptorImageInfo());
    //scene.m_hdrDome->setOutImage(m_gSimpleBuffers->getDescriptorImageInfo());


    // writing to descriptors
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> descImageInfos;

    
    for (unsigned int i = 0; i < 3; i++) {
        // descImageInfos.push_back(m_gbuffer->getDescriptorImageInfo(i));
        const VkDescriptorImageInfo& descriptorImage = m_gbuffer->getDescriptorImageInfo(i);
        VkDescriptorImageInfo descriptorImage_ = {
            .sampler = descriptorImage.sampler,
            .imageView = descriptorImage.imageView,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL // VK_IMAGE_LAYOUT_GENERAL//
        };
        // 转换布局
        
        //nvvk::cmdBarrierImageLayout(cmd, m_gbuffer->getColorImage(i), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        writes.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = m_dset->getSet(0),
            .dstBinding = i,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &descriptorImage_
        });
    }
    
    vkUpdateDescriptorSets(res.ctx.device, static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr);
}
//--------------------------------------------------------------------------------------------------
// Rendering the scene
// - Draw first the sky or HDR dome
// - Record the scene rendering (if not already done)
// - Execute the scene rendering
// - 只有一个colorAttachment，没有silhouette
//
void RendererDDGIRaster::render(VkCommandBuffer cmd, Resources& res/*res*/, Scene& scene, Settings& settings, nvvk::ProfilerVK& profiler)
{
    auto scopeDbg = m_dbgUtil->DBG_SCOPE(cmd);
    auto sec = profiler.timeRecurring("Raster", cmd);

    // Push constant
    m_pushConst.dbgMethod = g_rasterSettings1.dbgMethod;

    auto scope_dbg = m_dbgUtil->DBG_SCOPE(cmd);

    // Rendering dome or sky in the background, it is covering the entire screen
    {
        const float      aspect_ratio = m_gSimpleBuffers->getAspectRatio();
        const glm::mat4& view = CameraManip.getMatrix();
        auto& clipPlanes = CameraManip.getClipPlanes();
        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspect_ratio, clipPlanes.x, clipPlanes.y);
        proj[1][1] *= -1;

        const VkExtent2D imgSize = m_gSimpleBuffers->getSize();

        // Scene is recorded to avoid CPU overhead
        if (m_recordedSceneCmd == VK_NULL_HANDLE)
        {
            recordRasterScene(scene);
        }

        // Execute recorded command buffer - the scene graph traversal is already in the secondary command buffer,
        // but still need to execute it
        {
            for (int i = 0; i < 3; i++) {
                nvvk::cmdBarrierImageLayout(cmd, m_gbuffer->getColorImage(i), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
            }
            // nvvk::cmdBarrierImageLayout(cmd, m_gSimpleBuffers->getColorImage(0), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
            nvvk::cmdBarrierImageLayout(cmd, res.m_finalImage->getColorImage(0), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
            
            auto         rastersec = profiler.timeRecurring("RasterMRT", cmd);
            std::vector<VkClearValue> colorClears{
                {.color = {0.0F, 0.0F, 0.0F, 1.0F}},
                {.color = {0.0F, 0.0F, 0.0F, 1.0F}},
                {.color = {0.0F, 0.0F, 0.0F, 1.0F}},
            };
            VkClearValue depthClear{ .depthStencil = {1.0F, 0} };
            // There are two color attachments, one for the super-sampled final image and one for the selection (silhouette)
            // The depth is shared between the two
            // The first color attachment is loaded because we don't want to erase the dome/sky, the second is cleared.
            std::array<VkRenderingAttachmentInfo, 3> colorAttachments = {
                VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = m_gbuffer->getColorImageView(0),
                    .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue = colorClears[0],
                },
                VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = m_gbuffer->getColorImageView(1),
                    .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue = colorClears[1],
                },
                VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = m_gbuffer->getColorImageView(2),
                    .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue = colorClears[2],
                },
            };

            // Shared depth attachment
            VkRenderingAttachmentInfo depthStencilAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = m_gbuffer->getDepthImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = depthClear,
            };

            // Dynamic rendering information: color and depth attachments
            VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
                .renderArea = {{0, 0}, res.m_finalImage->getSize()},
                .layerCount = 1,
                .colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size()),
                .pColorAttachments = colorAttachments.data(),
                .pDepthAttachment = &depthStencilAttachment,
            };

            vkCmdBeginRendering(cmd, &renderingInfo);
            vkCmdExecuteCommands(cmd, 1, &m_recordedSceneCmd);
            vkCmdEndRendering(cmd);
        }
        {
            // 由于是dynamic rendering，所以需要用到Barrier
            std::vector<VkImageMemoryBarrier> gbufferMemoryBarrier;
            for (int32_t i = 0; i < 3; i++) {
                // gbufferMemoryBarrier.push_back(
                //     {
                //         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                //         .pNext = nullptr,
                //         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                //         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                //         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                //         .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                //         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                //         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                //         .image = m_gbuffer->getColorImage(i),
                //         .subresourceRange = {
                //             .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                //             .baseMipLevel = 0,
                //             .levelCount = 1,
                //             .baseArrayLayer = 0,
                //             .layerCount = 1
                //         },
                //     });
                nvvk::cmdBarrierImageLayout(cmd, m_gbuffer->getColorImage(i), VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            // vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            //     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, gbufferMemoryBarrier.size(), gbufferMemoryBarrier.data());
            //nvvk::cmdBarrierImageLayout(cmd, m_gSimpleBuffers->getColorImage(0), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
            
            // Composition
            auto         rastersec = profiler.timeRecurring("RasterCOMP", cmd);
            VkClearValue colorClear{ .color = {settings.solidBackgroundColor.x, settings.solidBackgroundColor.y,
                                              settings.solidBackgroundColor.z, 1.0F} };
            VkClearValue depthClear{ .depthStencil = {1.0F, 0} };


            // There are two color attachments, one for the super-sampled final image and one for the selection (silhouette)
            // The depth is shared between the two
            // The first color attachment is loaded because we don't want to erase the dome/sky, the second is cleared.
            std::vector<VkRenderingAttachmentInfo> colorAttachments = {
                VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = res.m_finalImage->getColorImageView(0),
                    .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                    .loadOp = settings.useSolidBackground ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue = colorClear,
                },
            };

            // Shared depth attachment
            VkRenderingAttachmentInfo depthStencilAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = res.m_finalImage->getDepthImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = depthClear,
            };

            // Dynamic rendering information: color and depth attachments
            VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
                .renderArea = {{0, 0}, res.m_finalImage->getSize()},
                .layerCount = 1,
                .colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size()),
                .pColorAttachments = colorAttachments.data(),
                .pDepthAttachment = &depthStencilAttachment,
            };

            vkCmdBeginRendering(cmd, &renderingInfo);

            VkViewport viewport = {};
            viewport.width = res.m_finalImage->getSize().width;
            viewport.height = res.m_finalImage->getSize().height;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor = {};
            scissor.extent = res.m_finalImage->getSize();
            scissor.offset = { 0, 0 };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            std::vector<VkDescriptorSet> dset = { scene.m_sceneDescriptorSet,m_dset->getSet(0)};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeplineCOMP->layout, 0,
                static_cast<uint32_t>(dset.size()), dset.data(), 0, nullptr);
            // Draw solid
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeplineCOMP->plines[0]);
            
            vkCmdEndRendering(cmd);

            nvvk::cmdBarrierImageLayout(cmd, res.m_finalImage->getColorImage(0), VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            for (int i = 0; i < 3; i++)
                nvvk::cmdBarrierImageLayout(cmd, m_gbuffer->getColorImage(i), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }

    }
}

//--------------------------------------------------------------------------------------------------
// Render the UI of the rasterizer
//
bool RendererDDGIRaster::onUI()
{
    auto& headerManager = CollapsingHeaderManager::getInstance();
    bool changed{ false };

    if (headerManager.beginHeader("RendererRaster"))
    {
        ImGui::PushID("RendererRaster");
        PE::begin();
        changed |= PE::Checkbox("SSAO", &g_rasterSettings1.ssao);
        changed |= PE::Combo("Debug Method", reinterpret_cast<int32_t*>(&g_rasterSettings1.dbgMethod),
            "None\0Metallic\0Roughness\0Normal\0Tangent\0Bitangent\0BaseColor\0Emissive\0Opacity\0TexCoord0\0TexCoord1\0\0");
        PE::end();
        ImGui::PopID();
    }
    if (changed)
    {
        vkDeviceWaitIdle(m_device);
        freeRecordCommandBuffer();
    }
    return changed;
}

//--------------------------------------------------------------------------------------------------
// If the scene has changed, or the resolution changed, we need to re-record the command buffer
//
void RendererDDGIRaster::handleChange(Resources& res, Scene& scene)
{
    static int  lastSelection = -1;
    bool        resetRecorededScene = (lastSelection != scene.getSelectedRenderNode());
    bool        gbufferChanged = res.hasGBuffersChanged();
    bool        updateHdrDome = scene.hasDirtyFlag(Scene::eHdrEnv);
    bool        visibilityChanged = scene.hasDirtyFlag(Scene::eNodeVisibility);

    if (gbufferChanged || updateHdrDome || visibilityChanged)
    {
        vkDeviceWaitIdle(m_device);
        lastSelection = scene.getSelectedRenderNode();
        freeRecordCommandBuffer();
    }
    if (gbufferChanged)
    {
        // Need to recreate the output G-Buffers with the new size
        createGBuffer(res, scene);
        updateHdrDome = true;
    }
    if (updateHdrDome)
    {
        scene.m_hdrDome->setOutImage(m_gSimpleBuffers->getDescriptorImageInfo());
    }

}


//--------------------------------------------------------------------------------------------------
// Create the all pipelines for rendering the scene.
// It uses the same layout for all pipelines
// It uses the same layout for all pipelines
// but the piplines are different for solid, blend, wireframe, etc.
//
void RendererDDGIRaster::createRasterPipeline(Resources& res, Scene& scene)
{
    nvh::ScopedTimer st(__FUNCTION__);

    std::unique_ptr<nvvk::DebugUtil> dutil = std::make_unique<nvvk::DebugUtil>(m_device);
    m_rasterPipeplineMRT = std::make_unique<nvvkhl::PipelineContainer>();
    m_rasterPipeplineCOMP = std::make_unique<nvvkhl::PipelineContainer>();

    VkDescriptorSetLayout sceneSet = scene.m_sceneDescriptorSetLayout;
    VkDescriptorSetLayout hdrDomeSet = scene.m_hdrDome->getDescLayout();
    VkDescriptorSetLayout skySet = scene.m_sky->getDescriptorSetLayout();
    VkDescriptorSetLayout compositionSet = m_dset->getLayout();

    {
        // Creating the Pipeline Layout for mrt
        std::vector<VkDescriptorSetLayout> layouts{ sceneSet }; // , hdrDomeSet, skySet
        const VkPushConstantRange pushConstantRanges = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                        .offset = 0,
                                                        .size = sizeof(DH::PushConstantRaster) };
        VkPipelineLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRanges,
        };
        vkCreatePipelineLayout(m_device, &create_info, nullptr, &m_rasterPipeplineMRT->layout);
    }
    {
        // for composition:
        std::vector<VkDescriptorSetLayout> layouts{ sceneSet, compositionSet }; // , hdrDomeSet, skySet
        const VkPushConstantRange pushConstantRanges = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                        .offset = 0,
                                                        .size = sizeof(DH::PushConstantRaster) };
        VkPipelineLayoutCreateInfo create_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRanges,
        };
        vkCreatePipelineLayout(m_device, &create_info, nullptr, &m_rasterPipeplineCOMP->layout);
    }
    
    {
        std::vector<VkFormat>         color_format = { 
            m_gbuffer->getColorFormat(0),
            m_gbuffer->getColorFormat(1),
            m_gbuffer->getColorFormat(2),
        };
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = uint32_t(color_format.size()),
            .pColorAttachmentFormats = color_format.data(),
            .depthAttachmentFormat = m_gbuffer->getDepthFormat(),
        };

        // Creating the Pipeline for mrt
        nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_rasterPipeplineMRT->layout, {} /*m_offscreenRenderPass*/);
        gpb.createInfo.pNext = &renderingInfo;
 
        // 参考gltf_scene_vk.cpp - 499
        gpb.addBindingDescriptions({ 
            {0, sizeof(glm::vec3)}, // pos
            {1, sizeof(glm::vec3)}, // normal
            //{2, sizeof(glm::vec4)}, // color
            //{3, sizeof(glm::vec4)}, // tangent
            {2, sizeof(glm::vec2)}, // texCoord0
        });// binding = 0, stride = vec3
    
        gpb.addAttributeDescriptions({
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, // pos
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, // normal
            {2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, // texCoord
            //{3, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, // tangent
            //{4, 4, VK_FORMAT_R32G32_SFLOAT, 0}, // 
            // Position(location, binding, format, offset)
            });


        // Solid
        gpb.rasterizationState.depthBiasEnable = VK_TRUE;
        gpb.rasterizationState.depthBiasConstantFactor = -1;
        gpb.rasterizationState.depthBiasSlopeFactor = 1;
        gpb.rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
        gpb.setBlendAttachmentCount(uint32_t(color_format.size()));  // 2 color attachments

        gpb.addShader(m_shaderModules[mDeferVertex  ], VK_SHADER_STAGE_VERTEX_BIT);
        gpb.addShader(m_shaderModules[mDeferFrag    ], VK_SHADER_STAGE_FRAGMENT_BIT);
        m_rasterPipeplineMRT->plines.push_back(gpb.createPipeline());
        m_dbgUtil->DBG_NAME(m_rasterPipeplineMRT->plines[mDeferredSolid]);
        // Double Sided
        gpb.rasterizationState.cullMode = VK_CULL_MODE_NONE;
        m_rasterPipeplineMRT->plines.push_back(gpb.createPipeline());
        m_dbgUtil->DBG_NAME(m_rasterPipeplineMRT->plines[mDeferredDoubleSided]);
    }


    {
        // for composition
        std::vector<VkFormat>         color_format = {
            m_gSimpleBuffers->getColorFormat(0),
        };
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = uint32_t(color_format.size()),
            .pColorAttachmentFormats = color_format.data(),
            .depthAttachmentFormat = m_gSimpleBuffers->getDepthFormat(),
        };

        nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_rasterPipeplineCOMP->layout, {} /*m_offscreenRenderPass*/);
        gpb.createInfo.pNext = &renderingInfo;

        // Solid
        gpb.rasterizationState.depthBiasEnable = VK_FALSE;
        gpb.setBlendAttachmentCount(uint32_t(color_format.size()));  

        gpb.addShader(m_shaderModules[mComposeVertex], VK_SHADER_STAGE_VERTEX_BIT);
        gpb.addShader(m_shaderModules[mComposeFrag], VK_SHADER_STAGE_FRAGMENT_BIT);
        m_rasterPipeplineCOMP->plines.push_back(gpb.createPipeline());
        m_dbgUtil->DBG_NAME(m_rasterPipeplineCOMP->plines[0]);
       
    }
    
    // Cleanup
    vkDestroyShaderModule(m_device, m_shaderModules[eVertex], nullptr);
    vkDestroyShaderModule(m_device, m_shaderModules[eFragment], nullptr);
    vkDestroyShaderModule(m_device, m_shaderModules[eFragmentOverlay], nullptr);
    vkDestroyShaderModule(m_device, m_shaderModules[mDeferVertex], nullptr);
    vkDestroyShaderModule(m_device, m_shaderModules[mDeferFrag], nullptr);
}



//--------------------------------------------------------------------------------------------------
// Raster commands are recorded to be replayed, this allocates that command buffer
//
void RendererDDGIRaster::createRecordCommandBuffer()
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
void RendererDDGIRaster::freeRecordCommandBuffer()
{
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_recordedSceneCmd);
    m_recordedSceneCmd = VK_NULL_HANDLE;
}

// --------------------------------------------------------------------------------------------------
// Recording in a secondary command buffer, the raster rendering of the scene.
//
void RendererDDGIRaster::recordRasterScene(Scene & scene)
{
    nvh::ScopedTimer st(__FUNCTION__);

    createRecordCommandBuffer();

    std::vector<VkFormat> colorFormat = { 
        m_gbuffer->getColorFormat(0),
        m_gbuffer->getColorFormat(1),
        m_gbuffer->getColorFormat(2),
                                       };

    VkCommandBufferInheritanceRenderingInfo inheritanceRenderingInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .colorAttachmentCount = uint32_t(colorFormat.size()),
        .pColorAttachmentFormats = colorFormat.data(),
        .depthAttachmentFormat = m_gSimpleBuffers->getDepthFormat(),
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkCommandBufferInheritanceInfo inheritInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                                               .pNext = &inheritanceRenderingInfo };

    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
        .pInheritanceInfo = &inheritInfo,
    };

    vkBeginCommandBuffer(m_recordedSceneCmd, &beginInfo);
    renderRasterScene(m_recordedSceneCmd, scene);
    vkEndCommandBuffer(m_recordedSceneCmd);
}

//--------------------------------------------------------------------------------------------------
// rendering the gltf nodes contaied in the list
void RendererDDGIRaster::renderNodes(VkCommandBuffer cmd, Scene& scene, const std::vector<uint32_t>& nodeIDs) {
    auto scope_dbg = m_dbgUtil->DBG_SCOPE(cmd);
    const std::array<VkDeviceSize, 3> offsets{};
    const std::vector<nvh::gltf::RenderNode>& renderNodes = scene.m_gltfScene->getRenderNodes();
    const std::vector<nvh::gltf::RenderPrimitive>& subMeshes = scene.m_gltfScene->getRenderPrimitives();

    for (const uint32_t &nodeID : nodeIDs) {
        const nvh::gltf::RenderNode& renderNode = renderNodes[nodeID];
        const nvh::gltf::RenderPrimitive& subMesh = subMeshes[renderNode.renderPrimID];

        if (!renderNode.visible) {
            continue;
        }
        std::array<VkBuffer, 3> vertexBuffers{ 
            scene.m_gltfSceneVk->vertexBuffers()[renderNode.renderPrimID].position.buffer,
            scene.m_gltfSceneVk->vertexBuffers()[renderNode.renderPrimID].normal.buffer,
            //scene.m_gltfSceneVk->vertexBuffers()[renderNode.renderPrimID].color.buffer,
            //scene.m_gltfSceneVk->vertexBuffers()[renderNode.renderPrimID].tangent.buffer,
            scene.m_gltfSceneVk->vertexBuffers()[renderNode.renderPrimID].texCoord0.buffer,
        };
        m_pushConst.materialID = renderNode.materialID;
        m_pushConst.renderPrimID = renderNode.renderPrimID;
        m_pushConst.renderNodeID = static_cast<int>(nodeID);
        m_pushConst.selectedRenderNode = scene.getSelectedRenderNode();

        vkCmdPushConstants(cmd, m_rasterPipeplineMRT->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(DH::PushConstantRaster), &m_pushConst);
        // TODO: 需要修改：gltf_scene_vk 中的VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
        vkCmdBindVertexBuffers(cmd, 0, vertexBuffers.size(), vertexBuffers.data(), offsets.data());
        vkCmdBindIndexBuffer(cmd, scene.m_gltfSceneVk->indices()[renderNode.renderPrimID].buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, 0, 0, 0);
    }
}

//--------------------------------------------------------------------------------------------------
// Render the entire scene for raster. Splitting the solid and blend-able element and rendering
// on top
// This is done in a recoded command buffer to be replay
void RendererDDGIRaster::renderRasterScene(VkCommandBuffer cmd, Scene& scene)
{
    auto scope_dbg = m_dbgUtil->DBG_SCOPE(cmd);

    const VkExtent2D& render_size = m_gSimpleBuffers->getSize();

    const VkViewport viewport{ 0.0F, 0.0F, static_cast<float>(render_size.width), static_cast<float>(render_size.height),
                              0.0F, 1.0F };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const VkRect2D scissor{ {0, 0}, {render_size.width, render_size.height} };
    vkCmdSetScissor(cmd, 0, 1, &scissor);


    std::vector dset = { scene.m_sceneDescriptorSet}; // , scene.m_hdrDome->getDescSet(), scene.m_sky->getDescriptorSet() 
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeplineMRT->layout, 0,
        static_cast<uint32_t>(dset.size()), dset.data(), 0, nullptr);
    // Draw solid
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeplineMRT->plines[mDeferredSolid]);
    renderNodes(cmd, scene, scene.m_gltfScene->getShadedNodes(nvh::gltf::Scene::eRasterSolid));
    //
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipeplineMRT->plines[mDeferredDoubleSided]);
    renderNodes(cmd, scene, scene.m_gltfScene->getShadedNodes(nvh::gltf::Scene::eRasterSolidDoubleSided));
    // Draw blend-able
    // vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rasterPipepline->plines[eRasterBlend]);
    // renderNodes(cmd, scene, scene.m_gltfScene->getShadedNodes(nvh::gltf::Scene::eRasterBlend));

}

std::unique_ptr<Renderer> makeRendererDDGIRaster() {
    return std::make_unique<RendererDDGIRaster>();
}

}
