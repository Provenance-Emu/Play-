#pragma once

#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include "ext/libretro.h"
#include "libretro_vulkan.h"

extern retro_video_refresh_t g_video_cb;
extern retro_log_printf_t g_log_cb;
extern const struct retro_hw_render_interface_vulkan* g_vulkan_iface;

// DIRECT RENDERING: Play! renders directly to RetroArch's target VkImage
// No copying, no format conversion, no intermediate images
class CGSH_Vulkan_Libretro : public CGSH_Vulkan
{
public:
    CGSH_Vulkan_Libretro();
    virtual ~CGSH_Vulkan_Libretro();

    // Factory function for libretro
    static CGSHandler::FactoryFunction GetFactoryFunction();

    void InitializeImpl() override;
    void ReleaseImpl() override;
    void FlipImpl(const DISPLAY_INFO& displayInfo) override;
    void PresentBackbuffer() override;
    
    // CRITICAL: Override to tell base class we're in libretro mode
    bool IsLibretroMode() const override { return true; }

private:
    // DIRECT RENDERING: Create VkImage that Play! renders to, then provide to RetroArch
    void CreateDirectRenderTarget();
    void SetupPlayRenderTarget();
    void CopyPlayRenderedContentToLibretroImage();
    void ProvideRenderedImageToRetroArch();
    void SetupVulkanContext();
    
    // Direct rendering target that Play! renders to (in RetroArch's expected format)
    VkImage m_direct_render_image = VK_NULL_HANDLE;
    VkDeviceMemory m_direct_render_memory = VK_NULL_HANDLE;
    VkImageView m_direct_render_view = VK_NULL_HANDLE;
    
    // libretro image to provide to RetroArch
    retro_vulkan_image m_libretro_image = {};
    
    // CRITICAL: Store libretro Vulkan instance to keep it alive for annotations
    std::unique_ptr<Framework::Vulkan::CInstance> m_libretro_instance;
    
    // Deferred initialization flag
    bool m_vulkan_setup_deferred = false;
    
    uint32_t m_frame_count = 0;
    static constexpr uint32_t RENDER_WIDTH = 640;
    static constexpr uint32_t RENDER_HEIGHT = 480;
};
