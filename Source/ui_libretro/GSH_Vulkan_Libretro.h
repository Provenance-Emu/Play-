#pragma once

#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include "libretro_vulkan.h"

extern retro_video_refresh_t g_video_cb;

// Libretro Vulkan handler following iOS CGSH_VulkaniOS pattern
// Proper Vulkan integration with RetroArch's Vulkan context
class CGSH_Vulkan_Libretro : public CGSH_Vulkan
{
public:
	CGSH_Vulkan_Libretro();
	virtual ~CGSH_Vulkan_Libretro();

	// Libretro-specific initialization
	void InitializeWithInterface(const struct retro_hw_render_interface_vulkan* vk_iface);
	
	// Factory function for libretro
	static CGSHandler::FactoryFunction GetFactoryFunction();
	
	// Public methods called from main_libretro.cpp
	void Reset();
	void FlushMailBox();
	
protected:
	// Override to indicate this is libretro mode
	bool IsLibretroMode() const override { return true; }

	// Following iOS pattern: Override only what's necessary for libretro
	void PresentBackbuffer() override;
	void FlipImpl(const DISPLAY_INFO& displayInfo) override;
	void SetPresentationParams(const CGSHandler::PRESENTATION_PARAMS& presentationParams) override;
	void MarkNewFrame() override;
	void SyncCLUT(const TEX0& tex0) override;

private:

	// Store libretro Vulkan interface
	const struct retro_hw_render_interface_vulkan* m_vk_iface = nullptr;
    
    // Libretro image management
    std::map<uint32_t, VkImage> m_libretro_images;
    std::map<uint32_t, VkImageView> m_libretro_image_views;
    std::map<uint32_t, VkDeviceMemory> m_libretro_image_memory;
    
    // Helper methods for image management
    VkImage GetOrCreateLibretroImage(uint32_t sync_index);
    VkImageView GetLibretroImageView(uint32_t sync_index);
    void CopyPlayDrawImageToLibretro(VkImage dst_image, uint32_t sync_index);
    void CleanupLibretroImages();
};