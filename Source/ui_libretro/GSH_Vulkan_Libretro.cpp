#include "GSH_Vulkan_Libretro.h"
#include "gs/GSH_Vulkan/GSH_VulkanDrawDesktop.h"
#include "gs/GSH_Vulkan/GSH_VulkanDrawMobile.h"
#include <thread>
#include <chrono>
#include <stdexcept>

// These are defined in main_libretro.cpp
extern retro_video_refresh_t g_video_cb;
extern retro_log_printf_t g_log_cb;
extern const struct retro_hw_render_interface_vulkan* g_vulkan_iface;

#define RETRO_LOG(level, ...) \
    do { \
        if (g_log_cb) { \
            g_log_cb(level, __VA_ARGS__); \
        } \
    } while(0)

CGSH_Vulkan_Libretro::CGSH_Vulkan_Libretro()
    : CGSH_Vulkan(false)  // false = non-threaded mode for libretro compatibility
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] CGSH_Vulkan_Libretro constructor - DIRECT RENDERING MODE (non-threaded)\n");
}

CGSH_Vulkan_Libretro::~CGSH_Vulkan_Libretro()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] CGSH_Vulkan_Libretro destructor\n");
    ReleaseImpl();
}

CGSHandler::FactoryFunction CGSH_Vulkan_Libretro::GetFactoryFunction()
{
    return []() { return new CGSH_Vulkan_Libretro(); };
}

void CGSH_Vulkan_Libretro::InitializeImpl()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] InitializeImpl - Deferring Vulkan setup until interface is available\n");
    
    // CRITICAL: In libretro mode, the Vulkan interface is not available yet during InitializeImpl
    // We need to defer the actual Vulkan setup until the interface is provided
    if (!g_vulkan_iface) {
        RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Vulkan interface not yet available - deferring initialization\n");
        m_vulkan_setup_deferred = true;
        return;
    }
    
    // If we get here, the interface is available, so set up normally
    SetupVulkanContext();
}

void CGSH_Vulkan_Libretro::SetupVulkanContext()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] SetupVulkanContext - Setting up Vulkan context with libretro interface\n");
    
    if (!g_vulkan_iface) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] ERROR: No Vulkan interface available!\n");
        return;
    }
    
    // CRITICAL: Set up libretro device context BEFORE calling base class initialization
    // This prevents the assertion failure in SetLibretroDevice (m_handle must be VK_NULL_HANDLE)
    
    // First, create a proper CInstance wrapper around the libretro VkInstance as member variable
    m_libretro_instance = std::make_unique<Framework::Vulkan::CInstance>(g_vulkan_iface->instance);
    
    // CRITICAL: Assign libretro instance to context so annotations can use it
    m_context->instance = m_libretro_instance.get();
    
    // Now set up the device with the proper instance
    m_context->device.SetLibretroDevice(*m_libretro_instance, g_vulkan_iface->device);
    m_context->queue = g_vulkan_iface->queue;
    
    // Initialize command buffer pool with libretro queue family
    m_context->commandBufferPool = Framework::Vulkan::CCommandBufferPool(m_context->device, g_vulkan_iface->queue_index);
    
    // CRITICAL: Initialize annotations system with libretro context (prevents SetObjectName crash)
    m_context->annotations = decltype(m_context->annotations)(m_context->instance, &m_context->device);
    
    // CRITICAL: Initialize device properties from libretro physical device (prevents compute workgroup assertion)
    {
        VkPhysicalDeviceProperties deviceProperties = {};
        m_context->instance->vkGetPhysicalDeviceProperties(g_vulkan_iface->gpu, &deviceProperties);
        m_context->storageBufferAlignment = static_cast<uint32>(deviceProperties.limits.minStorageBufferOffsetAlignment);
        m_context->computeWorkgroupInvocations = deviceProperties.limits.maxComputeWorkGroupInvocations;
        RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Device properties: storageBufferAlignment=%u, computeWorkgroupInvocations=%u\n",
                  m_context->storageBufferAlignment, m_context->computeWorkgroupInvocations);
    }
    
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Libretro device context configured - calling base initialization\n");
    
    // Now call base class initialization - it will use our pre-configured libretro device
    CGSH_Vulkan::InitializeImpl();
    
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Vulkan context set up from libretro interface\n");
    
    // Create direct render target that Play! will render to
    CreateDirectRenderTarget();
    
    // Set up Play!'s render targets to use our direct render target
    SetupPlayRenderTarget();
    
    // Mark setup as complete
    m_vulkan_setup_deferred = false;
    
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Direct rendering initialized successfully with libretro Vulkan context\n");
}

void CGSH_Vulkan_Libretro::ReleaseImpl()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] ReleaseImpl - Cleaning up direct rendering\n");
    
    // Clean up direct render target
    if (m_direct_render_view != VK_NULL_HANDLE) {
        m_context->device.vkDestroyImageView(m_context->device, m_direct_render_view, nullptr);
        m_direct_render_view = VK_NULL_HANDLE;
    }
    
    if (m_direct_render_memory != VK_NULL_HANDLE) {
        m_context->device.vkFreeMemory(m_context->device, m_direct_render_memory, nullptr);
        m_direct_render_memory = VK_NULL_HANDLE;
    }
    
    if (m_direct_render_image != VK_NULL_HANDLE) {
        m_context->device.vkDestroyImage(m_context->device, m_direct_render_image, nullptr);
        m_direct_render_image = VK_NULL_HANDLE;
    }
    
    // Call base class cleanup
    CGSH_Vulkan::ReleaseImpl();
}

void CGSH_Vulkan_Libretro::FlipImpl(const DISPLAY_INFO& displayInfo)
{
    // Check if we need to complete deferred Vulkan setup
    if (m_vulkan_setup_deferred && g_vulkan_iface) {
        RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Vulkan interface now available - completing deferred setup\n");
        SetupVulkanContext();
    }
    
    // If setup is not complete, skip rendering
    if (m_vulkan_setup_deferred) {
        RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Vulkan setup still deferred - skipping flip\n");
        return;
    }
    
    // Simple flip - let base class handle it
    CGSH_Vulkan::FlipImpl(displayInfo);
}

void CGSH_Vulkan_Libretro::PresentBackbuffer()
{
    if (!g_vulkan_iface || !g_video_cb) {
        return;
    }

    // DIRECT RENDERING: Copy Play!'s rendered content to our libretro image
    CopyPlayRenderedContentToLibretroImage();

    // DIRECT RENDERING: Provide our rendered image to RetroArch
    ProvideRenderedImageToRetroArch();
    
    // Signal frame completion
    g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, RENDER_WIDTH, RENDER_HEIGHT, 0);
    
    if (m_frame_count % 60 == 0) {
        RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Frame %u presented via DIRECT RENDERING\n", m_frame_count);
    }
    m_frame_count++;
}



void CGSH_Vulkan_Libretro::CreateDirectRenderTarget()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Creating direct render target (%ux%u)\n", RENDER_WIDTH, RENDER_HEIGHT);
    
    // Create VkImage in RetroArch's expected format
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;  // RetroArch's expected format
    image_info.extent.width = RENDER_WIDTH;
    image_info.extent.height = RENDER_HEIGHT;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkResult result = m_context->device.vkCreateImage(m_context->device, &image_info, nullptr, &m_direct_render_image);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to create direct render image: %d\n", result);
        return;
    }
    
    // Allocate memory for the image
    VkMemoryRequirements mem_requirements;
    m_context->device.vkGetImageMemoryRequirements(m_context->device, m_direct_render_image, &mem_requirements);
    
    // Find suitable memory type
    VkPhysicalDeviceMemoryProperties mem_properties;
    m_instance.vkGetPhysicalDeviceMemoryProperties(g_vulkan_iface->gpu, &mem_properties);
    
    uint32_t memory_type_index = UINT32_MAX;
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((mem_requirements.memoryTypeBits & (1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type_index = i;
            break;
        }
    }
    
    if (memory_type_index == UINT32_MAX) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to find suitable memory type\n");
        return;
    }
    
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = memory_type_index;
    
    result = m_context->device.vkAllocateMemory(m_context->device, &alloc_info, nullptr, &m_direct_render_memory);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to allocate direct render memory: %d\n", result);
        return;
    }
    
    result = m_context->device.vkBindImageMemory(m_context->device, m_direct_render_image, m_direct_render_memory, 0);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to bind direct render memory: %d\n", result);
        return;
    }
    
    // Create image view
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_direct_render_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    
    result = m_context->device.vkCreateImageView(m_context->device, &view_info, nullptr, &m_direct_render_view);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to create direct render image view: %d\n", result);
        return;
    }
    
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Direct render target created successfully\n");
}

void CGSH_Vulkan_Libretro::SetupPlayRenderTarget()
{
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Setting up Play! render target access\n");
    
    // REAL IMPLEMENTATION: We don't override Play!'s render targets.
    // Instead, we copy from Play!'s rendered image to our libretro image.
    // Play! renders to its own m_drawImage (Desktop) or m_drawColorImage (Mobile),
    // and we copy that content to m_direct_render_image in PresentBackbuffer().
    
    RETRO_LOG(RETRO_LOG_INFO, "[DIRECT] Play! render target access setup complete\n");
}

void CGSH_Vulkan_Libretro::ProvideRenderedImageToRetroArch()
{
    if (!g_vulkan_iface || m_direct_render_image == VK_NULL_HANDLE) {
        return;
    }
    
    // Set up libretro image structure - create_info is VkImageViewCreateInfo, not VkImageCreateInfo!
    m_libretro_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    m_libretro_image.create_info.pNext = nullptr;
    m_libretro_image.create_info.flags = 0;
    m_libretro_image.create_info.image = m_direct_render_image;
    m_libretro_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    m_libretro_image.create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    m_libretro_image.create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    m_libretro_image.create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    m_libretro_image.create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    m_libretro_image.create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    m_libretro_image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    m_libretro_image.create_info.subresourceRange.baseMipLevel = 0;
    m_libretro_image.create_info.subresourceRange.levelCount = 1;
    m_libretro_image.create_info.subresourceRange.baseArrayLayer = 0;
    m_libretro_image.create_info.subresourceRange.layerCount = 1;
    
    m_libretro_image.image_view = m_direct_render_view;
    m_libretro_image.image_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    // Provide the image to RetroArch
    g_vulkan_iface->set_image(g_vulkan_iface->handle, &m_libretro_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    
    RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Provided rendered image to RetroArch\n");
}

void CGSH_Vulkan_Libretro::CopyPlayRenderedContentToLibretroImage()
{
    auto drawSystem = GetDrawSystem();
    if (!drawSystem || m_direct_render_image == VK_NULL_HANDLE) {
        RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Cannot copy - draw system or image not available\n");
        return;
    }
    
    // Get Play!'s rendered image - works for both Desktop and Mobile draw systems
    const Framework::Vulkan::CImage* playDrawImage = nullptr;
    
    // Try to get the draw image from Play!'s draw system
    // Both CDrawDesktop and CDrawMobile have GetDrawImage() methods
    try {
        // Access the draw image through the public GetDrawImage() method
        // This works for both Desktop (m_drawImage) and Mobile (m_drawColorImage) systems
        auto drawDesktop = std::dynamic_pointer_cast<GSH_Vulkan::CDrawDesktop>(drawSystem);
        auto drawMobile = std::dynamic_pointer_cast<GSH_Vulkan::CDrawMobile>(drawSystem);
        
        if (drawDesktop) {
            playDrawImage = &drawDesktop->GetDrawImage();
            RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Using Desktop draw image\n");
        } else if (drawMobile) {
            playDrawImage = &drawMobile->GetDrawImage();
            RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Using Mobile draw image\n");
        } else {
            RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Unknown draw system type\n");
            return;
        }
    } catch (const std::exception& e) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to access draw image: %s\n", e.what());
        return;
    }
    
    if (!playDrawImage || static_cast<VkImage>(*playDrawImage) == VK_NULL_HANDLE) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Play! draw image is not valid\n");
        return;
    }
    
    // Create a command buffer for the copy operation
    auto commandBuffer = m_context->commandBufferPool.AllocateBuffer();
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    VkResult result = m_context->device.vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to begin command buffer: %d\n", result);
        return;
    }
    
    // Transition Play!'s image to transfer source layout
    VkImageMemoryBarrier srcBarrier = {};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = static_cast<VkImage>(*playDrawImage);
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    srcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    // Transition our libretro image to transfer destination layout
    VkImageMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.image = m_direct_render_image;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    VkImageMemoryBarrier barriers[] = {srcBarrier, dstBarrier};
    m_context->device.vkCmdPipelineBarrier(commandBuffer,
                                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0, 0, nullptr, 0, nullptr, 2, barriers);
    
    // Copy from Play!'s image to our libretro image
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.srcOffset = {0, 0, 0};
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.dstOffset = {0, 0, 0};
    copyRegion.extent = {RENDER_WIDTH, RENDER_HEIGHT, 1};
    
    m_context->device.vkCmdCopyImage(commandBuffer,
                                    static_cast<VkImage>(*playDrawImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    m_direct_render_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &copyRegion);
    
    // Transition our libretro image to shader read layout for frontend
    VkImageMemoryBarrier finalBarrier = {};
    finalBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.image = m_direct_render_image;
    finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    finalBarrier.subresourceRange.baseMipLevel = 0;
    finalBarrier.subresourceRange.levelCount = 1;
    finalBarrier.subresourceRange.baseArrayLayer = 0;
    finalBarrier.subresourceRange.layerCount = 1;
    finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    m_context->device.vkCmdPipelineBarrier(commandBuffer,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                          0, 0, nullptr, 0, nullptr, 1, &finalBarrier);
    
    // End command buffer
    result = m_context->device.vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to end command buffer: %d\n", result);
        return;
    }
    
    // Submit command buffer
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    result = m_context->device.vkQueueSubmit(m_context->queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to submit command buffer: %d\n", result);
        return;
    }
    
    // Wait for completion
    result = m_context->device.vkQueueWaitIdle(m_context->queue);
    if (result != VK_SUCCESS) {
        RETRO_LOG(RETRO_LOG_ERROR, "[DIRECT] Failed to wait for queue idle: %d\n", result);
        return;
    }
    
    RETRO_LOG(RETRO_LOG_DEBUG, "[DIRECT] Successfully copied Play! rendered content to libretro image\n");
}
