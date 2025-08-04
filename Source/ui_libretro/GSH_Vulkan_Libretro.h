#pragma once

#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include "ext/libretro.h"
#include "libretro_vulkan.h"

extern retro_video_refresh_t g_video_cb;
extern retro_log_printf_t g_log_cb;
extern const struct retro_hw_render_interface_vulkan* g_vulkan_iface;
extern int g_res_factor;
extern CGSHandler::PRESENTATION_MODE g_presentation_mode;

/// Vulkan libretro handler that overrides the presentation system
/// to render to libretro VkImage instead of swapchain
class CGSH_Vulkan_Libretro : public CGSH_Vulkan
{
public:
    CGSH_Vulkan_Libretro();
    virtual ~CGSH_Vulkan_Libretro();

    static CGSHandler::FactoryFunction GetFactoryFunction();

    void InitializeImpl() override;
    void ReleaseImpl() override;
    void FlipImpl(const DISPLAY_INFO& displayInfo) override;
    void SetPresentationParams(const CGSHandler::PRESENTATION_PARAMS& presentationParams) override;
    void PresentBackbuffer() override;

    /// CRITICAL: Override to tell base class we're in libretro mode
    bool IsLibretroMode() const override { return true; }

private:
    void SetupVulkanContext();
    void UpdatePresentationForRetroArch();
    void CreateLibretroRenderTarget();
    void DestroyLibretroRenderTarget();
    void RenderToLibretroTarget(const DISPLAY_INFO& displayInfo);
    void RenderPS2Layer(VkCommandBuffer commandBuffer, const CGSHandler::DISPLAY_INFO::LAYER& layer, const CGSHandler::DISPLAY_INFO& displayInfo);
    void CapturePlayRenderedContent(const CGSHandler::DISPLAY_INFO& displayInfo);
    void RenderPS2MemoryContent(VkCommandBuffer commandBuffer, const CGSHandler::DISPLAY_INFO::LAYER& layer, const CGSHandler::DISPLAY_INFO& displayInfo);
    bool AnalyzePS2Buffer(const CGSHandler::DISPLAY_INFO::LAYER& layer, float& r, float& g, float& b, float& a);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    /// CRITICAL: Store libretro Vulkan instance to keep it alive
    std::unique_ptr<Framework::Vulkan::CInstance> m_libretro_instance;

    /// Deferred initialization flag
    bool m_vulkan_setup_deferred = false;

    /// Current frame size for RetroArch
    uint32_t m_current_width = 640;
    uint32_t m_current_height = 480;

    /// LibretroRender target that we render to instead of swapchain
    VkImage m_libretro_image = VK_NULL_HANDLE;
    VkDeviceMemory m_libretro_memory = VK_NULL_HANDLE;
    VkImageView m_libretro_image_view = VK_NULL_HANDLE;
    VkFramebuffer m_libretro_framebuffer = VK_NULL_HANDLE;
    VkRenderPass m_libretro_renderpass = VK_NULL_HANDLE;



    /// libretro image structure to provide to RetroArch
    retro_vulkan_image m_retro_image = {};

    uint32_t m_frame_count = 0;
};
