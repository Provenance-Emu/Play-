#include "GSH_Vulkan_Libretro.h"
#include "../gs/GSH_Vulkan/GSH_Vulkan.h"
#include "../gs/GSH_Vulkan/GSH_VulkanDrawDesktop.h"
#include "vulkan/Loader.h"
#include "Log.h"
#include <map>

#define LOG_NAME "libretro_vulkan"

extern retro_log_printf_t g_log_cb;
#define RETRO_LOG(level, ...) \
	do { \
		if (g_log_cb) \
			g_log_cb(level, __VA_ARGS__); \
	} while (0)

CGSH_Vulkan_Libretro::CGSH_Vulkan_Libretro()
	: CGSH_Vulkan(false)  // Use protected constructor with gsThreaded=false for single-threaded libretro mode
{
	// Threading is now properly set via base constructor
}

CGSH_Vulkan_Libretro::~CGSH_Vulkan_Libretro()
{
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Vulkan libretro destructor called\n");
    CleanupLibretroImages();
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
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] PresentBackbuffer called\n");
    
    if (!m_vk_iface) {
        g_log_cb(RETRO_LOG_ERROR, "[libretro ERROR] Vulkan interface not available\n");
        return;
    }
    
    // Get current sync index from RetroArch
    uint32_t sync_index = m_vk_iface->get_sync_index(m_vk_iface->handle);
    g_log_cb(RETRO_LOG_DEBUG, "[libretro DEBUG] Sync index: %u\n", sync_index);
    
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
    vk_image.create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    vk_image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_image.create_info.subresourceRange.baseMipLevel = 0;
    vk_image.create_info.subresourceRange.levelCount = 1;
    vk_image.create_info.subresourceRange.baseArrayLayer = 0;
    vk_image.create_info.subresourceRange.layerCount = 1;
    
    // Provide the image to RetroArch
    m_vk_iface->set_image(m_vk_iface->handle, &vk_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    
    // Notify RetroArch that a frame is ready
    g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
    
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
    
    // CRITICAL FIX: Copy the image RIGHT AFTER rendering is complete, BEFORE presentation
    // This ensures we capture the actual rendered content before it gets cleared
    if (m_vk_iface && m_vk_iface->handle) {
        uint32_t sync_index = m_vk_iface->get_sync_index(m_vk_iface->handle);
        VkImage libretro_image = GetOrCreateLibretroImage(sync_index);
        if (libretro_image != VK_NULL_HANDLE) {
            CopyPlayDrawImageToLibretro(libretro_image, sync_index);
        }
    }
    
    // Now call PresentBackbuffer to notify frontend
    PresentBackbuffer();
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
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_info.extent.width = 1024;  // Match Play!'s DRAW_AREA_SIZE to avoid scaling
    image_info.extent.height = 1024; // This prevents flickering from format conversion
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
    
    // Try to get Play!'s draw image and copy it to the libretro image
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
    
    // Get a command buffer for the copy operation
    VkCommandBuffer cmd_buffer = m_context->commandBufferPool.AllocateBuffer();
    
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
        
        // Copy the image
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
        copy_region.extent = {1024, 1024, 1}; // DRAW_AREA_SIZE from Play!
        
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
        // Fall back to test pattern if we can't access Play!'s draw image
        RETRO_LOG(RETRO_LOG_INFO, "TAKING FALLBACK PATH: Using test pattern fallback\n");
        
        VkClearColorValue clear_color;
        
        // Create a simple test pattern based on sync_index to verify different frames
        switch(sync_index % 3) {
            case 0:
                clear_color = {{1.0f, 0.0f, 0.0f, 1.0f}}; // Red
                break;
            case 1:
                clear_color = {{0.0f, 1.0f, 0.0f, 1.0f}}; // Green
                break;
            case 2:
                clear_color = {{0.0f, 0.0f, 1.0f, 1.0f}}; // Blue
                break;
        }
        
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
    
    // Wait for copy to complete with timeout to prevent hanging
    result = m_context->device.vkWaitForFences(m_context->device, 1, &copy_fence, VK_TRUE, 16666666); // ~16ms timeout (60fps)
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_WARN, "[libretro WARN] Fence wait timed out or failed: %d\n", result);
    }
    
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


