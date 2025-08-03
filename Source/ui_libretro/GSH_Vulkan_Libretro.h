#pragma once

#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include "ext/libretro.h"
#include "libretro_vulkan.h"

extern retro_video_refresh_t g_video_cb;
extern retro_log_printf_t g_log_cb;
extern const struct retro_hw_render_interface_vulkan* g_vulkan_iface;

// Simple Vulkan libretro handler following iOS CGSH_VulkaniOS pattern
// Minimal interface - let base class handle everything
class CGSH_Vulkan_Libretro : public CGSH_Vulkan
{
public:
    CGSH_Vulkan_Libretro();
    virtual ~CGSH_Vulkan_Libretro();

    // Factory function for libretro
    static CGSHandler::FactoryFunction GetFactoryFunction();

protected:
    // Following iOS pattern: Override only what's necessary for libretro
    void InitializeImpl() override;
    void FlipImpl(const DISPLAY_INFO& displayInfo) override;
    void PresentBackbuffer() override;
    
    // Critical: Override to tell base class we're in libretro mode
    bool IsLibretroMode() const override { return true; }

private:
    // Play! content copying and image management methods
    VkImage GetOrCreateLibretroImage(uint32_t sync_index);
    VkImageView GetLibretroImageView(uint32_t sync_index);
    VkFramebuffer GetOrCreateFramebuffer(uint32_t sync_index);
    void CopyPlayRenderedContent(VkImage dst_image, uint32_t sync_index);
    void CopyVkImageToLibretro(VkImage src_image, VkImage dst_image, uint32_t sync_index);
    void RenderFallbackPattern(VkImage dst_image, uint32_t sync_index);
    uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties);
    void CreateLibretroImages();
    void CleanupLibretroImages();

    void CreateConversionPipeline();
    void CleanupConversionPipeline();
    
    // Libretro-specific members for image management
    VkImage m_libretro_images[3];
    std::vector<VkImageView> m_libretro_image_views;
    std::vector<VkDeviceMemory> m_libretro_image_memories;

    // Resources for converting Play!'s R32_UINT draw image to libretro's R8G8B8A8_UNORM
    VkRenderPass m_conversionRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_conversionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_conversionPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_conversionDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_conversionDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_conversionDescriptorSets;
    std::vector<VkFramebuffer> m_conversionFramebuffers;

    uint32_t m_frame_count;
    uint32_t m_last_sync_index = -1;

};
