#include "GSH_Vulkan_Libretro.h"
#include "ext/libretro.h"
#include "gs/GSH_Vulkan/GSH_VulkanDrawMobile.h"
#include "gs/GSH_Vulkan/GSH_VulkanDrawDesktop.h"
#include "MemStream.h"
#include "nuanceur/Builder.h"
#include "nuanceur/generators/SpirvShaderGenerator.h"
#include <chrono>

extern retro_video_refresh_t g_video_cb;
extern retro_log_printf_t g_log_cb;
extern const struct retro_hw_render_interface_vulkan* g_vulkan_iface;

CGSH_Vulkan_Libretro::CGSH_Vulkan_Libretro()
    : CGSH_Vulkan(false) // Enable threading like iOS
{
    // Initialize libretro-specific members
    m_libretro_image_views.resize(3);
    m_libretro_image_memories.resize(3);
    for (int i = 0; i < 3; ++i) {
        m_libretro_images[i] = VK_NULL_HANDLE;
        m_libretro_image_views[i] = VK_NULL_HANDLE;
        m_libretro_image_memories[i] = VK_NULL_HANDLE;
    }
    m_frame_count = 0;

    // Initialize conversion pipeline members
    m_conversionRenderPass = VK_NULL_HANDLE;
    m_conversionDescriptorSetLayout = VK_NULL_HANDLE;
    m_conversionPipelineLayout = VK_NULL_HANDLE;
    m_conversionPipeline = VK_NULL_HANDLE;
    m_conversionDescriptorPool = VK_NULL_HANDLE;
}

CGSH_Vulkan_Libretro::~CGSH_Vulkan_Libretro()
{
    CleanupConversionPipeline();
    CleanupLibretroImages();
}

CGSHandler::FactoryFunction CGSH_Vulkan_Libretro::GetFactoryFunction()
{
    return []() { return new CGSH_Vulkan_Libretro(); };
}

void CGSH_Vulkan_Libretro::InitializeImpl()
{
    g_log_cb(RETRO_LOG_INFO, "[SIMPLE] Vulkan libretro initialization starting\n");

    // Set up RetroArch's Vulkan context BEFORE base class init
    if (g_vulkan_iface) {
        // Set up minimal context that base class expects in libretro mode
        m_instance = Framework::Vulkan::CInstance(g_vulkan_iface->instance);
        m_context->surface = VK_NULL_HANDLE; // libretro manages presentation

        // CRITICAL: Set up external device BEFORE base class init so function pointers are available
        m_context->device.InitializeWithExternalDevice(m_instance, g_vulkan_iface->device);
        m_context->queue = g_vulkan_iface->queue;
        m_context->commandBufferPool = Framework::Vulkan::CCommandBufferPool(m_context->device, g_vulkan_iface->queue_index);

        // Initialize annotations system with external context to prevent assertion failure
        m_context->annotations = Framework::Vulkan::CAnnotations<GSH_VULKAN_USE_ANNOTATIONS>(&m_instance, &m_context->device);

        // Query device properties to initialize compute limits (prevents CTransferLocal assertion)
        VkPhysicalDeviceProperties deviceProperties = {};
        m_instance.vkGetPhysicalDeviceProperties(g_vulkan_iface->gpu, &deviceProperties);
        m_context->computeWorkgroupInvocations = deviceProperties.limits.maxComputeWorkGroupInvocations;

        // Create conversion pipeline for format conversion
        CreateConversionPipeline();
        g_log_cb(RETRO_LOG_INFO, "[VULKAN] Conversion pipeline created successfully\n");

        g_log_cb(RETRO_LOG_INFO, "[SIMPLE] External Vulkan device set up before base class init\n");
    } else {
        g_log_cb(RETRO_LOG_ERROR, "[SIMPLE] No Vulkan interface available - cannot initialize\n");
        return;
    }

    // Let base class handle everything - it will skip device creation in IsLibretroMode()
    CGSH_Vulkan::InitializeImpl();

    g_log_cb(RETRO_LOG_INFO, "[SIMPLE] Vulkan libretro initialization complete\n");
}

void CGSH_Vulkan_Libretro::FlipImpl(const DISPLAY_INFO& displayInfo)
{
    // Simple flip - let base class handle rendering
    CGSH_Vulkan::FlipImpl(displayInfo);
}

void CGSH_Vulkan_Libretro::PresentBackbuffer()
{
    if (!g_vulkan_iface || !g_video_cb) {
        return;
    }

    // Get current sync index from RetroArch
    uint32_t sync_index = g_vulkan_iface->get_sync_index(g_vulkan_iface->handle);

    // Create or get the VkImage for this sync index
    VkImage dst_image = GetOrCreateLibretroImage(sync_index);
    if (dst_image == VK_NULL_HANDLE) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to create libretro image for sync_index %u\n", sync_index);
        return;
    }

    // Copy Play!'s rendered content to libretro image
    CopyPlayRenderedContent(dst_image, sync_index);

    // Set up the image for RetroArch
    retro_vulkan_image vk_image = {};
    vk_image.image_view = GetLibretroImageView(sync_index);
    vk_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk_image.create_info.image = dst_image;

    // Provide image to RetroArch
    g_vulkan_iface->set_image(g_vulkan_iface->handle, &vk_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);

    // Notify RetroArch that frame is ready
    g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);

    m_frame_count++;
    if (m_frame_count % 60 == 0) {
        g_log_cb(RETRO_LOG_INFO, "[TEST] Frame %u presented (sync_index=%u)\n", m_frame_count, sync_index);
    }
}

VkImage CGSH_Vulkan_Libretro::GetOrCreateLibretroImage(uint32_t sync_index)
{
    if (sync_index >= 3) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Invalid sync_index %u (max 2)\n", sync_index);
        return VK_NULL_HANDLE;
    }

    // Return existing image if already created
    if (m_libretro_images[sync_index] != VK_NULL_HANDLE) {
        return m_libretro_images[sync_index];
    }

    // Create new VkImage for this sync index
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM; // STANDARD: Use RetroArch standard format, avoid blit shaders
    image_info.extent.width = 640;
    image_info.extent.height = 480;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = m_context->device.vkCreateImage(m_context->device, &image_info, nullptr, &m_libretro_images[sync_index]);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to create VkImage for sync_index %u: %d\n", sync_index, result);
        return VK_NULL_HANDLE;
    }

    // Allocate memory for the image
    VkMemoryRequirements mem_reqs;
    m_context->device.vkGetImageMemoryRequirements(m_context->device, m_libretro_images[sync_index], &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = FindMemoryType(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    result = m_context->device.vkAllocateMemory(m_context->device, &alloc_info, nullptr, &m_libretro_image_memories[sync_index]);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to allocate memory for sync_index %u: %d\n", sync_index, result);
        m_context->device.vkDestroyImage(m_context->device, m_libretro_images[sync_index], nullptr);
        m_libretro_images[sync_index] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Bind memory to image
    result = m_context->device.vkBindImageMemory(m_context->device, m_libretro_images[sync_index], m_libretro_image_memories[sync_index], 0);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to bind memory for sync_index %u: %d\n", sync_index, result);
        m_context->device.vkFreeMemory(m_context->device, m_libretro_image_memories[sync_index], nullptr);
        m_context->device.vkDestroyImage(m_context->device, m_libretro_images[sync_index], nullptr);
        m_libretro_images[sync_index] = VK_NULL_HANDLE;
        m_libretro_image_memories[sync_index] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    g_log_cb(RETRO_LOG_INFO, "[TEST] Created VkImage for sync_index %u: %p\n", sync_index, m_libretro_images[sync_index]);
    return m_libretro_images[sync_index];
}

VkImageView CGSH_Vulkan_Libretro::GetLibretroImageView(uint32_t sync_index)
{
    if (sync_index >= 3 || m_libretro_images[sync_index] == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    // Return existing image view if already created
    if (m_libretro_image_views[sync_index] != VK_NULL_HANDLE) {
        return m_libretro_image_views[sync_index];
    }

    // Create image view
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = m_libretro_images[sync_index];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM; // STANDARD: Use RetroArch standard format
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    VkResult result = m_context->device.vkCreateImageView(m_context->device, &view_info, nullptr, &m_libretro_image_views[sync_index]);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to create image view for sync_index %u: %d\n", sync_index, result);
        return VK_NULL_HANDLE;
    }

    g_log_cb(RETRO_LOG_INFO, "[TEST] Created image view for sync_index %u: %p\n", sync_index, m_libretro_image_views[sync_index]);
    return m_libretro_image_views[sync_index];
}

VkFramebuffer CGSH_Vulkan_Libretro::GetOrCreateFramebuffer(uint32_t sync_index)
{
    if (sync_index >= 3) {
        g_log_cb(RETRO_LOG_ERROR, "[VULKAN] Invalid sync_index %u for framebuffer\n", sync_index);
        return VK_NULL_HANDLE;
    }

    // Return existing framebuffer if already created
    if (m_conversionFramebuffers[sync_index] != VK_NULL_HANDLE) {
        return m_conversionFramebuffers[sync_index];
    }

    // Ensure we have a valid image view first
    VkImageView imageView = GetLibretroImageView(sync_index);
    if (imageView == VK_NULL_HANDLE) {
        g_log_cb(RETRO_LOG_ERROR, "[VULKAN] No valid image view for sync_index %u, cannot create framebuffer\n", sync_index);
        return VK_NULL_HANDLE;
    }

    // Create framebuffer
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_conversionRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &imageView;
    framebufferInfo.width = 640;
    framebufferInfo.height = 480;
    framebufferInfo.layers = 1;

    VkResult result = m_context->device.vkCreateFramebuffer(m_context->device, &framebufferInfo, nullptr, &m_conversionFramebuffers[sync_index]);
    if (result != VK_SUCCESS) {
        g_log_cb(RETRO_LOG_ERROR, "[VULKAN] Failed to create framebuffer for sync_index %u: %d\n", sync_index, result);
        return VK_NULL_HANDLE;
    }

    g_log_cb(RETRO_LOG_INFO, "[VULKAN] Created framebuffer for sync_index %u: %p\n", sync_index, m_conversionFramebuffers[sync_index]);
    return m_conversionFramebuffers[sync_index];
}

void CGSH_Vulkan_Libretro::CopyPlayRenderedContent(VkImage dst_image, uint32_t sync_index)
{
    g_log_cb(RETRO_LOG_ERROR, "[DEBUG] CopyPlayRenderedContent called with sync_index %u\n", sync_index);
    
    // Get Play!'s draw system to access the rendered image
    auto drawSystem = GetDrawSystem();
    if (!drawSystem) {
        g_log_cb(RETRO_LOG_ERROR, "[PLAY] No draw system available - falling back to test pattern\n");
        RenderFallbackPattern(dst_image, sync_index);
        return;
    }

    // Try to get the mobile draw system (iOS/tvOS should use CDrawMobile)
    auto drawMobile = std::dynamic_pointer_cast<GSH_Vulkan::CDrawMobile>(drawSystem);
    if (drawMobile) {
        g_log_cb(RETRO_LOG_INFO, "[PLAY] Using CDrawMobile for image copy\n");

        // Get the draw image from mobile draw system
        const Framework::Vulkan::CImage& drawImage = drawMobile->GetDrawImage();
        VkImage src_image = drawImage;

        if (src_image != VK_NULL_HANDLE) {
            CopyVkImageToLibretro(src_image, dst_image, sync_index);
            return;
        } else {
            g_log_cb(RETRO_LOG_WARN, "[PLAY] CDrawMobile draw image is null\n");
        }
    }

    // Try desktop draw system as fallback
    auto drawDesktop = std::dynamic_pointer_cast<GSH_Vulkan::CDrawDesktop>(drawSystem);
    if (drawDesktop) {
        g_log_cb(RETRO_LOG_INFO, "[PLAY] Using CDrawDesktop for image copy\n");

        // Get the draw image from desktop draw system
        const Framework::Vulkan::CImage& drawImage = drawDesktop->GetDrawImage();
        VkImage src_image = drawImage;

        if (src_image != VK_NULL_HANDLE) {
            CopyVkImageToLibretro(src_image, dst_image, sync_index);
            return;
        } else {
            g_log_cb(RETRO_LOG_WARN, "[PLAY] CDrawDesktop draw image is null\n");
        }
    }

    g_log_cb(RETRO_LOG_ERROR, "[PLAY] No valid draw system found - falling back to test pattern\n");
    RenderFallbackPattern(dst_image, sync_index);
}



void CGSH_Vulkan_Libretro::RenderFallbackPattern(VkImage dst_image, uint32_t sync_index)
{
    g_log_cb(RETRO_LOG_ERROR, "[DEBUG] RenderFallbackPattern called - ORANGE FALLBACK ACTIVE\n");
    
    // Allocate command buffer for fallback pattern rendering
    VkCommandBuffer cmd_buffer = m_context->commandBufferPool.AllocateBuffer();

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_context->device.vkBeginCommandBuffer(cmd_buffer, &begin_info);

    // Transition image to transfer destination
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dst_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    m_context->device.vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Use a distinctive fallback color (orange) to indicate fallback mode
    VkClearColorValue clear_color = {1.0f, 0.5f, 0.0f, 1.0f}; // Orange

    // Clear image with fallback color
    VkImageSubresourceRange clear_range = {};
    clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_range.baseMipLevel = 0;
    clear_range.levelCount = 1;
    clear_range.baseArrayLayer = 0;
    clear_range.layerCount = 1;

    m_context->device.vkCmdClearColorImage(cmd_buffer, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &clear_range);

    // Transition to shader read optimal for RetroArch
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    m_context->device.vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // End and submit command buffer
    m_context->device.vkEndCommandBuffer(cmd_buffer);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;

    m_context->device.vkQueueSubmit(m_context->queue, 1, &submit_info, VK_NULL_HANDLE);
    m_context->device.vkQueueWaitIdle(m_context->queue); // Wait for completion

    g_log_cb(RETRO_LOG_WARN, "[PLAY] Using fallback pattern (orange) - Play! content not available\n");
}

uint32_t CGSH_Vulkan_Libretro::FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_properties;
    m_instance.vkGetPhysicalDeviceMemoryProperties(g_vulkan_iface->gpu, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    g_log_cb(RETRO_LOG_ERROR, "[TEST] Failed to find suitable memory type\n");
    return 0;
}

void CGSH_Vulkan_Libretro::CleanupConversionPipeline()
{
    auto& device = m_context->device;
    if(device.IsEmpty()) return;

    if(m_conversionPipeline != VK_NULL_HANDLE) {
        device.vkDestroyPipeline(device, m_conversionPipeline, nullptr);
        m_conversionPipeline = VK_NULL_HANDLE;
    }
    if(m_conversionPipelineLayout != VK_NULL_HANDLE) {
        device.vkDestroyPipelineLayout(device, m_conversionPipelineLayout, nullptr);
        m_conversionPipelineLayout = VK_NULL_HANDLE;
    }
    if(m_conversionRenderPass != VK_NULL_HANDLE) {
        device.vkDestroyRenderPass(device, m_conversionRenderPass, nullptr);
        m_conversionRenderPass = VK_NULL_HANDLE;
    }
    if(m_conversionDescriptorSetLayout != VK_NULL_HANDLE) {
        device.vkDestroyDescriptorSetLayout(device, m_conversionDescriptorSetLayout, nullptr);
        m_conversionDescriptorSetLayout = VK_NULL_HANDLE;
    }

    for(auto framebuffer : m_conversionFramebuffers)
    {
        if(framebuffer != VK_NULL_HANDLE) {
            device.vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    m_conversionFramebuffers.clear();

    if(m_conversionDescriptorPool != VK_NULL_HANDLE)
    {
        device.vkDestroyDescriptorPool(device, m_conversionDescriptorPool, nullptr);
        m_conversionDescriptorPool = VK_NULL_HANDLE;
    }
}

void CGSH_Vulkan_Libretro::CreateConversionPipeline()
{
    auto& device = m_context->device;

    // 1. Create Render Pass
    {
        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorAttachmentRef = {};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        if(device.vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_conversionRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create conversion render pass.");
        }
    }

    // 2. Create Descriptor Set Layout
    {
        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 0;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.pImmutableSamplers = nullptr;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &samplerLayoutBinding;

        if(device.vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_conversionDescriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create conversion descriptor set layout.");
        }
    }

    // 3. Create Pipeline Layout
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_conversionDescriptorSetLayout;

        if(device.vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_conversionPipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create conversion pipeline layout.");
        }
    }

    // 4. Create Graphics Pipeline
    {
        // Create simple fragment shader using Nuanceur - just pass through texture coordinates
        Framework::Vulkan::CShaderModule fragShaderModule;
        {
            using namespace Nuanceur;
            auto b = CShaderBuilder();
            
            // Fragment shader outputs
            auto outputColor = CFloat4Lvalue(b.CreateOutput(SEMANTIC_SYSTEM_COLOR, 0));
            
            // SPIR-V FIX: Use the simplest possible shader to avoid assertion failures
            // Output a solid cyan color (0,1,1,1) to debug color channels
            outputColor = NewFloat4(b, 0.0f, 1.0f, 1.0f, 1.0f);
            
            Framework::CMemStream shaderStream;
            Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
            shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
            fragShaderModule = Framework::Vulkan::CShaderModule(device, shaderStream);
        }

        // Create vertex shader using Nuanceur - full screen triangle
        Framework::Vulkan::CShaderModule vertShaderModule;
        {
            using namespace Nuanceur;
            auto b = CShaderBuilder();
            
            // Vertex Inputs
            auto vertexIndex = CIntLvalue(b.CreateInputInt(Nuanceur::SEMANTIC_SYSTEM_VERTEXINDEX));
            
            // Outputs
            auto outputPosition = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
            auto outputTexCoord = CFloat2Lvalue(b.CreateOutput(SEMANTIC_TEXCOORD, 0));
            
            // Generate full-screen triangle positions
            auto position = NewFloat2(
                ToFloat(vertexIndex << NewInt(b, 1) & NewInt(b, 2)),
                ToFloat(vertexIndex & NewInt(b, 2)));
            outputPosition = NewFloat4(
                position->x() * NewFloat(b, 2) + NewFloat(b, -1),
                position->y() * NewFloat(b, 2) + NewFloat(b, -1),
                NewFloat(b, 0),
                NewFloat(b, 1));
            
            // Output texture coordinates
            outputTexCoord = position;
            
            Framework::CMemStream shaderStream;
            Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_VERTEX);
            shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
            vertShaderModule = Framework::Vulkan::CShaderModule(device, shaderStream);
        }

        // Create shader stage infos
        VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = static_cast<VkShaderModule>(vertShaderModule);
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = static_cast<VkShaderModule>(fragShaderModule);
        fragShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport = {};
        viewport.width = 640;
        viewport.height = 480;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.extent = {640, 480};

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending = {};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = m_conversionPipelineLayout;
        pipelineInfo.renderPass = m_conversionRenderPass;
        pipelineInfo.subpass = 0;

        if(device.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_conversionPipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create conversion pipeline.");
        }
        
        // Note: Framework::Vulkan::CShaderModule objects are managed automatically
        // No manual cleanup needed for vertShaderModule and fragShaderModule
    }

    // 5. Initialize Framebuffer Storage (defer creation until images are ready)
    {
        m_conversionFramebuffers.resize(3, VK_NULL_HANDLE);
        g_log_cb(RETRO_LOG_INFO, "[VULKAN] Conversion pipeline created, framebuffers will be created on demand\n");
    }

    // 6. Create Descriptor Pool and Sets
    {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 3;

        if(device.vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_conversionDescriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create conversion descriptor pool.");
        }

        std::vector<VkDescriptorSetLayout> layouts(3, m_conversionDescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_conversionDescriptorPool;
        allocInfo.descriptorSetCount = 3;
        allocInfo.pSetLayouts = layouts.data();

        m_conversionDescriptorSets.resize(3);
        if(device.vkAllocateDescriptorSets(device, &allocInfo, m_conversionDescriptorSets.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate conversion descriptor sets.");
        }
    }
}

void CGSH_Vulkan_Libretro::CopyVkImageToLibretro(VkImage src_image, VkImage dst_image, uint32_t sync_index)
{
    g_log_cb(RETRO_LOG_ERROR, "[DEBUG] CopyVkImageToLibretro called - COPYING GAME CONTENT\n");
    
    // BYPASS SHADER: Just clear the destination image with cyan to test render target
    VkCommandBuffer cmd_buffer = m_context->commandBufferPool.AllocateBuffer();

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    m_context->device.vkBeginCommandBuffer(cmd_buffer, &begin_info);

    // COPY GAME CONTENT: Copy Play!'s rendered content to libretro image
    // Now that render target is proven to work, copy actual game graphics
    
    // Transition source image to transfer source
    VkImageMemoryBarrier src_barrier = {};
    src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.image = src_image;
    src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_barrier.subresourceRange.baseMipLevel = 0;
    src_barrier.subresourceRange.levelCount = 1;
    src_barrier.subresourceRange.baseArrayLayer = 0;
    src_barrier.subresourceRange.layerCount = 1;
    src_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    m_context->device.vkCmdPipelineBarrier(cmd_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &src_barrier);
    
    // Transition destination image to transfer destination
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

    m_context->device.vkCmdPipelineBarrier(cmd_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier);
    
    // Copy Play!'s rendered content to libretro image
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
    copy_region.extent = {640, 480, 1}; // Copy to 4:3 aspect ratio
    
    m_context->device.vkCmdCopyImage(cmd_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    
    // Transition destination image to shader read layout for RetroArch
    VkImageMemoryBarrier final_barrier = {};
    final_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    final_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    final_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    final_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    final_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    final_barrier.image = dst_image;
    final_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    final_barrier.subresourceRange.baseMipLevel = 0;
    final_barrier.subresourceRange.levelCount = 1;
    final_barrier.subresourceRange.baseArrayLayer = 0;
    final_barrier.subresourceRange.layerCount = 1;
    final_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    final_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    m_context->device.vkCmdPipelineBarrier(cmd_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &final_barrier);
    
    // End command buffer and submit
    m_context->device.vkEndCommandBuffer(cmd_buffer);
    
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer;
    
    m_context->device.vkQueueSubmit(m_context->queue, 1, &submit_info, VK_NULL_HANDLE);
    m_context->device.vkQueueWaitIdle(m_context->queue);
    
    g_log_cb(RETRO_LOG_DEBUG, "[VULKAN] Game content copy completed for sync_index %u\n", sync_index);
}

void CGSH_Vulkan_Libretro::CleanupLibretroImages()
{
    if (!m_context || m_context->device.IsEmpty()) {
        return;
    }

    for (int i = 0; i < 3; ++i) {
        if (m_libretro_image_views[i] != VK_NULL_HANDLE) {
            m_context->device.vkDestroyImageView(m_context->device, m_libretro_image_views[i], nullptr);
            m_libretro_image_views[i] = VK_NULL_HANDLE;
        }

        if (m_libretro_images[i] != VK_NULL_HANDLE) {
            m_context->device.vkDestroyImage(m_context->device, m_libretro_images[i], nullptr);
            m_libretro_images[i] = VK_NULL_HANDLE;
        }

        if (m_libretro_image_memories[i] != VK_NULL_HANDLE) {
            m_context->device.vkFreeMemory(m_context->device, m_libretro_image_memories[i], nullptr);
            m_libretro_image_memories[i] = VK_NULL_HANDLE;
        }
    }
}
