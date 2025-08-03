#include "GSH_Vulkan_Libretro.h"
#include "../gs/GSH_Vulkan/GSH_Vulkan.h"
#include "../gs/GSH_Vulkan/GSH_VulkanDrawDesktop.h"
#include "vulkan/Loader.h"
#include "Log.h"
#include <map>
#include <thread>

#define LOG_NAME "libretro_vulkan"

extern retro_log_printf_t g_log_cb;
#define RETRO_LOG(level, ...) \
	do { \
		if (g_log_cb) \
			g_log_cb(level, __VA_ARGS__); \
	} while (0)

CGSH_Vulkan_Libretro::CGSH_Vulkan_Libretro()
	: CGSH_Vulkan(false)  // Use protected constructor with gsThreaded=false for single-threaded libretro mode
	, m_is_paused(false)  // Initialize pause state
{
	// Threading is now properly set via base constructor
}

CGSH_Vulkan_Libretro::~CGSH_Vulkan_Libretro()
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] CGSH_Vulkan_Libretro destructor called\n");
    
    CleanupLibretroImages();
    CleanupLibretroCommandPool();  // SHADER MIXING FIX: Clean up our separate command pool
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] CGSH_Vulkan_Libretro destructor completed\n");
}

CGSHandler::FactoryFunction CGSH_Vulkan_Libretro::GetFactoryFunction()
{
	return []() { return new CGSH_Vulkan_Libretro(); };
}

void CGSH_Vulkan_Libretro::InitializeWithInterface(const struct retro_hw_render_interface_vulkan* vk_iface)
{
    g_log_cb(RETRO_LOG_INFO, "[libretro DEBUG] InitializeWithInterface called\n");
    
    if (!vk_iface) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Vulkan interface is null\n");
        return;
    }
    
    // Store the libretro Vulkan interface
    m_vk_iface = vk_iface;
    
    // Initialize libretro image storage
    m_libretro_images.clear();
    m_libretro_image_views.clear();
    m_libretro_image_memory.clear();
    
    // Set up Vulkan context with RetroArch-provided objects
    m_instance = Framework::Vulkan::CInstance(vk_iface->instance);
    m_context = std::make_shared<GSH_Vulkan::CContext>();
    m_context->instance = &m_instance;
    m_context->surface = VK_NULL_HANDLE; // No surface in libretro mode
    m_context->physicalDevice = vk_iface->gpu;
    
    // Initialize device wrapper with external VkDevice
    m_context->device.InitializeWithExternalDevice(m_instance, vk_iface->device);
    m_context->queue = vk_iface->queue;
    
    // Query device properties for compute workgroup limits
    VkPhysicalDeviceProperties deviceProperties = {};
    m_instance.vkGetPhysicalDeviceProperties(m_context->physicalDevice, &deviceProperties);
    
    // Set compute workgroup invocations from device properties
    m_context->computeWorkgroupInvocations = deviceProperties.limits.maxComputeWorkGroupInvocations;
    
    // Query memory properties
    m_instance.vkGetPhysicalDeviceMemoryProperties(m_context->physicalDevice, &m_context->physicalDeviceMemoryProperties);
    
    // Set up surface format (use RetroArch's preferred format)
    m_context->surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
    m_context->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    
    // Initialize annotations system with the libretro context
    m_context->annotations = Framework::Vulkan::CAnnotations<GSH_VULKAN_USE_ANNOTATIONS>(&m_instance, &m_context->device);
    
    // Create command buffer pool with the external device (queue family 0 for graphics)
    m_context->commandBufferPool = Framework::Vulkan::CCommandBufferPool(m_context->device, 0);
    
    g_log_cb(RETRO_LOG_INFO, "[libretro DEBUG] Vulkan context set up with RetroArch objects\n");
    
    // Now call base class initialization
    CGSH_Vulkan::InitializeImpl();
    
    g_log_cb(RETRO_LOG_INFO, "[libretro DEBUG] Vulkan libretro initialization complete\n");
}

void CGSH_Vulkan_Libretro::Reset()
{
    g_log_cb(RETRO_LOG_INFO, "[libretro DEBUG] Vulkan Reset called\n");
    
    // Let base class handle reset properly
    CGSH_Vulkan::Reset();
}

void CGSH_Vulkan_Libretro::MarkNewFrame()
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] MarkNewFrame called\n");
    
    // CRITICAL FIX: Call CGSH_Vulkan::MarkNewFrame() to ensure proper Vulkan frame lifecycle
    // This handles command buffer cycling, draw call recording, and frame management
    CGSH_Vulkan::MarkNewFrame();
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] MarkNewFrame completed\n");
}

void CGSH_Vulkan_Libretro::FlushMailBox()
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] FlushMailBox called\n");
    
    // FlushMailBox implementation for libretro - matches OpenGL pattern
    // This ensures proper single-threaded operation using ProcessSingleFrame()
    bool isFlushed = false;
    SendGSCall([&]() {
        isFlushed = true;
        m_flipped = true;
    }, true);
    
    while(!isFlushed)
    {
        // Wait for flush to complete using single-threaded processing
        ProcessSingleFrame();
    }
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] FlushMailBox completed\n");
}

void CGSH_Vulkan_Libretro::PresentBackbuffer()
{
    // FLICKER FIX: Original override gets sync_index and calls the new version
    if (!m_vk_iface) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Vulkan interface not available\n");
        return;
    }
    
    uint32_t sync_index = m_vk_iface->get_sync_index(m_vk_iface->handle);
    PresentBackbuffer(sync_index);
}

void CGSH_Vulkan_Libretro::PresentBackbuffer(uint32_t sync_index)
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] PresentBackbuffer called with sync_index: %u\n", sync_index);
    
    // PAUSE CRASH FIX: Don't present if paused to avoid MoltenVK descriptor binding crashes
    if (m_is_paused) {
        g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Skipping PresentBackbuffer - core is paused\n");
        return;
    }
    
    if (!m_vk_iface) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Vulkan interface not available\n");
        return;
    }
    
    // FLICKER FIX: Use the provided sync_index instead of getting it again
    // This ensures we present the exact same image we just copied to
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Using provided sync index: %u\n", sync_index);
    
    // Get the image that was already copied in FlipImpl()
    VkImage libretro_image = GetOrCreateLibretroImage(sync_index);
    if (libretro_image == VK_NULL_HANDLE) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to get libretro image\n");
        return;
    }
    
    // Set up retro_vulkan_image structure
    retro_vulkan_image vk_image = {};
    vk_image.image_view = GetLibretroImageView(sync_index);
    vk_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vk_image.create_info.image = libretro_image;
    vk_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vk_image.create_info.format = VK_FORMAT_B8G8R8A8_UNORM;  // Match GLES UNORM format
    vk_image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_image.create_info.subresourceRange.baseMipLevel = 0;
    vk_image.create_info.subresourceRange.levelCount = 1;
    vk_image.create_info.subresourceRange.baseArrayLayer = 0;
    vk_image.create_info.subresourceRange.layerCount = 1;
    
    // Provide the image to RetroArch
    m_vk_iface->set_image(m_vk_iface->handle, &vk_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    
    // GLES PATTERN FIX: Use dynamic CRT resolution like GLES libretro does
    // This ensures proper resolution matching between frontend and backend
    extern int g_res_factor;
    uint32_t crt_width = GetCrtWidth() * g_res_factor;
    uint32_t crt_height = GetCrtHeight() * g_res_factor;
    g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, crt_width, crt_height, 0);
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Frame presented to RetroArch\n");
}

void CGSH_Vulkan_Libretro::FlipImpl(const DISPLAY_INFO& displayInfo)
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] FlipImpl called\n");
    
    // In libretro mode, we handle the flip manually to avoid the infinite loop
    // caused by CGSH_Vulkan::FlipImpl() calling PresentBackbuffer()
    
    // Skip DoPresent() and transfer history - RetroArch manages presentation
    // Just do the basic flip logic and notify frontend
    
    // Call base CGSHandler::FlipImpl for basic flip logic (sets m_flipped = true)
    CGSHandler::FlipImpl(displayInfo);
    
    // GLES PATTERN FIX: Simple GPU sync like GLES libretro does
    // Just ensure rendering is complete before copying, no complex timing
    if (m_context && m_context->queue) {
        // Wait for GPU work to complete - matches GLES pattern
        m_context->device.vkQueueWaitIdle(m_context->queue);
        g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] GPU sync completed - ready to copy\n");
    }
    
    // FLICKER FIX: Get sync index once and use it consistently for both copy and present
    // This eliminates the race condition that causes flickering
    if (m_vk_iface && m_vk_iface->handle) {
        uint32_t sync_index = m_vk_iface->get_sync_index(m_vk_iface->handle);
        VkImage libretro_image = GetOrCreateLibretroImage(sync_index);
        if (libretro_image != VK_NULL_HANDLE) {
            CopyPlayDrawImageToLibretro(libretro_image, sync_index);
            // FLICKER FIX: Pass the same sync_index to PresentBackbuffer
            PresentBackbuffer(sync_index);
        }
    }
}

void CGSH_Vulkan_Libretro::SetPresentationParams(const CGSHandler::PRESENTATION_PARAMS& presentationParams)
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] SetPresentationParams: %dx%d\n", 
             presentationParams.windowWidth, presentationParams.windowHeight);
    
    // Store presentation params for later use
    m_presentationParams = presentationParams;
    
    // In libretro mode, skip swapchain validation - RetroArch manages swapchain
    // Just store the params for PresentBackbuffer to use
}

void CGSH_Vulkan_Libretro::SyncCLUT(const TEX0& tex0)
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] SyncCLUT called\n");
    
    // CRITICAL FIX: Call CGSH_Vulkan::SyncCLUT() to ensure proper Vulkan CLUT processing
    // This handles color lookup table operations and texture management
    CGSH_Vulkan::SyncCLUT(tex0);
}

// Helper methods for libretro image management
VkImage CGSH_Vulkan_Libretro::GetOrCreateLibretroImage(uint32_t sync_index)
{
    // Check if we already have an image for this sync index
    auto it = m_libretro_images.find(sync_index);
    if (it != m_libretro_images.end()) {
        return it->second;
    }
    
    // Create new VkImage for this sync index
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    // GLES PATTERN FIX: Use dynamic CRT resolution like GLES libretro does
    // This matches the proven GLES approach for proper resolution handling
    extern int g_res_factor;
    uint32_t crt_width = GetCrtWidth() * g_res_factor;
    uint32_t crt_height = GetCrtHeight() * g_res_factor;
    
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;  // Back to UNORM like GLES for compatibility
    image_info.extent.width = crt_width;   // Dynamic resolution like GLES
    image_info.extent.height = crt_height; // Dynamic resolution like GLES
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage image;
    VkResult result = m_context->device.vkCreateImage(m_context->device, &image_info, nullptr, &image);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to create VkImage: %d\n", result);
        return VK_NULL_HANDLE;
    }
    
    // Allocate memory for the image
    VkMemoryRequirements mem_requirements;
    m_context->device.vkGetImageMemoryRequirements(m_context->device, image, &mem_requirements);
    
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    
    // Find suitable memory type
    uint32_t memory_type_index = 0;
    for (uint32_t i = 0; i < m_context->physicalDeviceMemoryProperties.memoryTypeCount; i++) {
        if ((mem_requirements.memoryTypeBits & (1 << i)) &&
            (m_context->physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type_index = i;
            break;
        }
    }
    alloc_info.memoryTypeIndex = memory_type_index;
    
    VkDeviceMemory image_memory;
    result = m_context->device.vkAllocateMemory(m_context->device, &alloc_info, nullptr, &image_memory);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to allocate image memory: %d\n", result);
        m_context->device.vkDestroyImage(m_context->device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    // Bind memory to image
    result = m_context->device.vkBindImageMemory(m_context->device, image, image_memory, 0);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to bind image memory: %d\n", result);
        m_context->device.vkFreeMemory(m_context->device, image_memory, nullptr);
        m_context->device.vkDestroyImage(m_context->device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    // Create image view
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    
    VkImageView image_view;
    result = m_context->device.vkCreateImageView(m_context->device, &view_info, nullptr, &image_view);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to create image view: %d\n", result);
        m_context->device.vkFreeMemory(m_context->device, image_memory, nullptr);
        m_context->device.vkDestroyImage(m_context->device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    
    // Store the created resources
    m_libretro_images[sync_index] = image;
    m_libretro_image_views[sync_index] = image_view;
    m_libretro_image_memory[sync_index] = image_memory;
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Created VkImage for sync index %u\n", sync_index);
    return image;
}

VkImageView CGSH_Vulkan_Libretro::GetLibretroImageView(uint32_t sync_index)
{
    auto it = m_libretro_image_views.find(sync_index);
    if (it != m_libretro_image_views.end()) {
        return it->second;
    }
    return VK_NULL_HANDLE;
}

void CGSH_Vulkan_Libretro::CopyPlayDrawImageToLibretro(VkImage dst_image, uint32_t sync_index)
{
    RETRO_LOG(RETRO_LOG_INFO, "=== CopyPlayDrawImageToLibretro called, sync_index=%u ===\n", sync_index);
    
    // PAUSE CRASH FIX: Don't copy images if paused to avoid MoltenVK crashes
    if (m_is_paused) {
        RETRO_LOG(RETRO_LOG_DEBUG, "Skipping image copy - core is paused\n");
        return;
    }
    
    // ANTI-FLICKER FIX: Always copy actual game content, but add stability measures
    VkImage src_image = VK_NULL_HANDLE;
    bool has_source_image = false;
    
    // Access Play!'s draw system to get the rendered image
    auto drawSystem = GetDrawSystem();
    RETRO_LOG(RETRO_LOG_INFO, "GetDrawSystem() returned: %p\n", drawSystem.get());
    
    if (drawSystem) {
        // Try to cast to CDrawDesktop to access the draw image
        auto drawDesktop = std::dynamic_pointer_cast<GSH_Vulkan::CDrawDesktop>(drawSystem);
        RETRO_LOG(RETRO_LOG_INFO, "CDrawDesktop cast result: %p\n", drawDesktop.get());
        
        if (drawDesktop) {
            // Now we can access the draw image using the public getter
            const auto& drawImage = drawDesktop->GetDrawImage();
            src_image = static_cast<VkImage>(drawImage);
            has_source_image = true;
            RETRO_LOG(RETRO_LOG_INFO, "SUCCESS: Found CDrawDesktop and got draw image: %p\n", (void*)src_image);
            
            // Debug: Check if draw image is valid
            if (src_image == VK_NULL_HANDLE) {
                RETRO_LOG(RETRO_LOG_ERROR, "ERROR: Draw image is VK_NULL_HANDLE - draw image not created properly!\n");
                has_source_image = false;
            } else {
                RETRO_LOG(RETRO_LOG_INFO, "Draw image appears valid, will attempt copy\n");
            }
        } else {
            RETRO_LOG(RETRO_LOG_INFO, "Draw system is not CDrawDesktop (might be CDrawMobile)\n");
        }
    } else {
        RETRO_LOG(RETRO_LOG_INFO, "No draw system available\n");
    }
    
    // SHADER MIXING FIX: Use separate command buffer pool for libretro operations
    // This prevents interference with Play!'s active rendering pipeline
    VkCommandBuffer cmd_buffer = AllocateLibretroCommandBuffer();
    if (cmd_buffer == VK_NULL_HANDLE) {
        RETRO_LOG(RETRO_LOG_ERROR, "Failed to allocate libretro command buffer\n");
        return;
    }
    
    // Begin the command buffer
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    VkResult result = m_context->device.vkBeginCommandBuffer(cmd_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "Failed to begin command buffer: %d\n", result);
        return;
    }
    
    // Transition destination image layout for transfer
    VkImageMemoryBarrier dst_barrier = {};
    dst_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.image = dst_image;
    dst_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_barrier.subresourceRange.baseMipLevel = 0;
    dst_barrier.subresourceRange.levelCount = 1;
    dst_barrier.subresourceRange.baseArrayLayer = 0;
    dst_barrier.subresourceRange.layerCount = 1;
    dst_barrier.srcAccessMask = 0;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    m_context->device.vkCmdPipelineBarrier(
        cmd_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier
    );
    
    RETRO_LOG(RETRO_LOG_INFO, "Image copying decision: has_source_image=%s, src_image=%p\n", 
              has_source_image ? "true" : "false", (void*)src_image);
    
    if (has_source_image && src_image != VK_NULL_HANDLE) {
        // We have Play!'s draw image - copy it to the libretro image
        RETRO_LOG(RETRO_LOG_INFO, "TAKING IMAGE COPY PATH: Copying from Play! draw image to libretro image\n");
        
        // Transition source image to transfer source layout
        VkImageMemoryBarrier src_barrier = {};
        src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        src_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // Play! uses this layout
        src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_barrier.image = src_image;
        src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        src_barrier.subresourceRange.baseMipLevel = 0;
        src_barrier.subresourceRange.levelCount = 1;
        src_barrier.subresourceRange.baseArrayLayer = 0;
        src_barrier.subresourceRange.layerCount = 1;
        src_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        
        m_context->device.vkCmdPipelineBarrier(
            cmd_buffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &src_barrier
        );
        
        // GLES PATTERN FIX: Use simple copy like GLES, not complex scaling
        // This matches the proven GLES approach for stable video output
        extern int g_res_factor;
        uint32_t crt_width = GetCrtWidth() * g_res_factor;
        uint32_t crt_height = GetCrtHeight() * g_res_factor;
        
        VkImageCopy copy_region = {};
        copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.srcSubresource.mipLevel = 0;
        copy_region.srcSubresource.baseArrayLayer = 0;
        copy_region.srcSubresource.layerCount = 1;
        copy_region.srcOffset = {0, 0, 0};
        copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.dstSubresource.mipLevel = 0;
        copy_region.dstSubresource.baseArrayLayer = 0;
        copy_region.dstSubresource.layerCount = 1;
        copy_region.dstOffset = {0, 0, 0};
        copy_region.extent = {crt_width, crt_height, 1}; // Dynamic resolution like GLES
        
        RETRO_LOG(RETRO_LOG_INFO, "GLES PATTERN: Copying from Play! draw image to %ux%u\n", crt_width, crt_height);
        
        m_context->device.vkCmdCopyImage(
            cmd_buffer,
            src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy_region
        );
        
        // Transition source image back to color attachment layout
        src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        m_context->device.vkCmdPipelineBarrier(
            cmd_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &src_barrier
        );
        
    } else {
        // ANTI-FLICKER FIX: Instead of test pattern, clear to black for stable output
        RETRO_LOG(RETRO_LOG_INFO, "TAKING FALLBACK PATH: Using stable black clear\n");
        
        VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // Stable black
        
        VkImageSubresourceRange clear_range = {};
        clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear_range.baseMipLevel = 0;
        clear_range.levelCount = 1;
        clear_range.baseArrayLayer = 0;
        clear_range.layerCount = 1;
        
        m_context->device.vkCmdClearColorImage(
            cmd_buffer, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear_color, 1, &clear_range
        );
    }
    
    // Transition to shader read optimal for presentation
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    m_context->device.vkCmdPipelineBarrier(
        cmd_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier
    );
    
    // End the command buffer
    result = m_context->device.vkEndCommandBuffer(cmd_buffer);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "Failed to end command buffer: %d\n", result);
        return;
    }
    
    // CRITICAL VSYNC FIX: Add fence for proper synchronization
    VkFence copy_fence;
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    
    result = m_context->device.vkCreateFence(m_context->device, &fence_info, nullptr, &copy_fence);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to create fence: %d\n", result);
        return;
    }
    
    // Submit command buffer with fence for synchronization
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;
    
    result = m_context->device.vkQueueSubmit(m_context->queue, 1, &submit_info, copy_fence);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to submit command buffer: %d\n", result);
        m_context->device.vkDestroyFence(m_context->device, copy_fence, nullptr);
        return;
    }
    
    // FLICKER FIX: Wait for copy to complete with longer timeout for stability
    result = m_context->device.vkWaitForFences(m_context->device, 1, &copy_fence, VK_TRUE, 100000000); // ~100ms timeout for stability
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_WARN, "[libretro WARN] Fence wait timed out or failed: %d\n", result);
    }
    
    // FLICKER FIX: Additional GPU idle wait to ensure frame is completely stable
    m_context->device.vkQueueWaitIdle(m_context->queue);
    
    // Clean up fence
    m_context->device.vkDestroyFence(m_context->device, copy_fence, nullptr);

    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Copied/cleared image for sync index %u\n", sync_index);
}

void CGSH_Vulkan_Libretro::CleanupLibretroImages()
{
    for (auto& pair : m_libretro_image_views) {
        if (pair.second != VK_NULL_HANDLE) {
            m_context->device.vkDestroyImageView(m_context->device, pair.second, nullptr);
        }
    }
    
    for (auto& pair : m_libretro_images) {
        if (pair.second != VK_NULL_HANDLE) {
            m_context->device.vkDestroyImage(m_context->device, pair.second, nullptr);
        }
    }
    
    for (auto& pair : m_libretro_image_memory) {
        if (pair.second != VK_NULL_HANDLE) {
            m_context->device.vkFreeMemory(m_context->device, pair.second, nullptr);
        }
    }
    
    m_libretro_images.clear();
    m_libretro_image_views.clear();
    m_libretro_image_memory.clear();
    
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Cleaned up libretro images\n");
}

// SHADER MIXING FIX: Separate command pool management
void CGSH_Vulkan_Libretro::CreateLibretroCommandPool()
{
    if (m_libretro_command_pool != VK_NULL_HANDLE) {
        g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Libretro command pool already exists\n");
        return;
    }
    
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0; // Use same queue family as main context
    
    VkResult result = m_context->device.vkCreateCommandPool(m_context->device, &pool_info, nullptr, &m_libretro_command_pool);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to create libretro command pool: %d\n", result);
        m_libretro_command_pool = VK_NULL_HANDLE;
    } else {
        g_log_cb(RETRO_LOG_INFO, "[libretro INFO] Created separate command pool for libretro operations\n");
    }
}

void CGSH_Vulkan_Libretro::CleanupLibretroCommandPool()
{
    if (m_libretro_command_pool != VK_NULL_HANDLE) {
        m_context->device.vkDestroyCommandPool(m_context->device, m_libretro_command_pool, nullptr);
        m_libretro_command_pool = VK_NULL_HANDLE;
        g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Cleaned up libretro command pool\n");
    }
}

VkCommandBuffer CGSH_Vulkan_Libretro::AllocateLibretroCommandBuffer()
{
    if (m_libretro_command_pool == VK_NULL_HANDLE) {
        CreateLibretroCommandPool();
        if (m_libretro_command_pool == VK_NULL_HANDLE) {
            g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Cannot allocate command buffer - no command pool\n");
            return VK_NULL_HANDLE;
        }
    }
    
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = m_libretro_command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    
    VkCommandBuffer cmd_buffer;
    VkResult result = m_context->device.vkAllocateCommandBuffers(m_context->device, &alloc_info, &cmd_buffer);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Failed to allocate libretro command buffer: %d\n", result);
        return VK_NULL_HANDLE;
    }
    
    return cmd_buffer;
}


