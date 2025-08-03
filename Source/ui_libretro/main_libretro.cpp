#include "ext/libretro.h"
#include "libretro_vulkan.h"

// Define Vulkan libretro support based on build configuration
#if defined(VULKAN_SUPPORTED) || defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
#define VULKAN_LIBRETRO_SUPPORTED
#endif

// Include MoltenVK headers for iOS/macOS Vulkan support
#ifdef __APPLE__
#include <MoltenVK/vk_mvk_moltenvk.h>
#endif

#include "Log.h"
#include "AppConfig.h"
#include "PS2VM.h"
#include "ui_shared/BootableUtils.h"

#include "PS2VM_Preferences.h"
#include "GSH_OpenGL_Libretro.h"
#ifdef __APPLE__
#include "GSH_Vulkan_Libretro.h"
#endif
#include "SH_LibreAudio.h"
#include "PH_Libretro_Input.h"

#include "PathUtils.h"
#include "PtrStream.h"
#include "MemStream.h"

#include "filesystem_def.h"
#include "DefaultAppConfig.h"

#include <vector>
#include <cstdlib>

#define LOG_NAME "LIBRETRO"

static CPS2VM* m_virtualMachine = nullptr;
static bool first_run = false;

bool libretro_supports_bitmasks = false;
retro_video_refresh_t g_video_cb;
retro_environment_t g_environ_cb;
retro_input_poll_t g_input_poll_cb;
retro_input_state_t g_input_state_cb;
retro_audio_sample_batch_t g_set_audio_sample_batch_cb;
retro_log_printf_t g_log_cb = nullptr;

// SIMPLE: Global Vulkan interface for simple initialization
const struct retro_hw_render_interface_vulkan* g_vulkan_iface = nullptr;

std::map<int, int> g_ds2_to_retro_btn_map;
struct retro_hw_render_callback g_hw_render
{
};

int g_res_factor = 1;
CGSHandler::PRESENTATION_MODE g_presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FIT;
bool g_forceBilinearTextures = false;

static std::vector<struct retro_variable> m_vars =
    {
        {"play_res_multi", "Resolution Multiplier; 1x|2x|4x|8x"},
        {"play_presentation_mode", "Presentation Mode; Fit Screen|Fill Screen|Original Size"},
        {"play_bilinear_filtering", "Force Bilinear Filtering; false|true"},
#ifdef __APPLE__
        {"play_graphics_backend", "Graphics Backend; OpenGL ES|Vulkan"},
#endif
        {NULL, NULL},
};

enum class BootType
{
	CD,
	ELF
};

struct LastOpenCommand
{
	LastOpenCommand() = default;
	LastOpenCommand(BootType type, fs::path path)
	    : type(type)
	    , path(path)
	{
	}
	BootType type = BootType::CD;
	fs::path path;
};

LastOpenCommand m_bootCommand;

unsigned retro_api_version()
{
	return RETRO_API_VERSION;
}

// Forward declarations
void SetupVideoHandler();
static bool UseVulkanBackend();

#ifdef __APPLE__
// Vulkan context callbacks
static void retro_vk_context_reset();
static void retro_vk_context_destroy();

// Vulkan negotiation interface functions
static const VkApplicationInfo* retro_vulkan_get_application_info(void);
static bool retro_vulkan_create_device(struct retro_vulkan_context *context,
										   VkInstance instance,
										   VkPhysicalDevice gpu,
										   VkSurfaceKHR surface,
										   PFN_vkGetInstanceProcAddr get_instance_proc_addr,
										   const char **required_device_extensions,
										   unsigned num_required_device_extensions,
										   const char **required_device_layers,
										   unsigned num_required_device_layers,
										   const VkPhysicalDeviceFeatures *required_features);
static void retro_vulkan_destroy_device(void);

// Hardware render context negotiation interface for Vulkan
// This provides the complete interface that RetroArch expects
static struct retro_hw_render_context_negotiation_interface_vulkan hw_render_negotiation = {
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION, // Use version 2
	retro_vulkan_get_application_info,
	retro_vulkan_create_device,
	retro_vulkan_destroy_device,
	nullptr, // create_instance (v2 only, we'll let frontend handle this)
	nullptr  // create_device2 (v2 only, we'll use create_device instead)
};

// Vulkan context reset callback - this is where we get the hardware render interface
static void retro_vk_context_reset()
{
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] Context reset callback called\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Vulkan context reset callback\n");

	// Get the libretro Vulkan interface - this is now available
	retro_hw_render_interface* vulkan_interface = nullptr;
	if (!g_environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&vulkan_interface) || !vulkan_interface) {
		if (g_log_cb) {
			g_log_cb(RETRO_LOG_ERROR, "[VULKAN] Failed to get HW render interface in context reset\n");
		}
		CLog::GetInstance().Warn(LOG_NAME, "Failed to get HW render interface in context reset\n");
		return;
	}

	if (vulkan_interface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) {
		if (g_log_cb) {
			g_log_cb(RETRO_LOG_ERROR, "[VULKAN] HW render interface is not Vulkan (type: %d)\n", vulkan_interface->interface_type);
		}
		CLog::GetInstance().Warn(LOG_NAME, "HW render interface is not Vulkan (type: %d)\n", vulkan_interface->interface_type);
		return;
	}

	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] Successfully acquired Vulkan hardware render interface\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Successfully acquired Vulkan hardware render interface\n");

	// Cast to the specific Vulkan interface
	const struct retro_hw_render_interface_vulkan* vk_iface =
		reinterpret_cast<const struct retro_hw_render_interface_vulkan*>(vulkan_interface);

	// Initialize the Vulkan handler with the interface (if it exists)
	if (m_virtualMachine && m_virtualMachine->GetGSHandler()) {
		auto gsHandler = m_virtualMachine->GetGSHandler();
		if (UseVulkanBackend()) {
#ifdef VULKAN_LIBRETRO_SUPPORTED
			// SIMPLE: Store Vulkan interface globally for simple initialization
			g_vulkan_iface = vk_iface;
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SIMPLE] Vulkan interface stored for simple initialization\n");
			}
#else
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_WARN, "[VULKAN] Vulkan libretro support not compiled in, falling back to OpenGL\n");
			}
#endif
		}
	} else {
		// Create the video handler if it doesn't exist yet
		SetupVideoHandler();

		// Then initialize it with the interface
		if (m_virtualMachine && m_virtualMachine->GetGSHandler() && UseVulkanBackend()) {
#ifdef VULKAN_LIBRETRO_SUPPORTED
			// SIMPLE: Store Vulkan interface globally for simple initialization
			g_vulkan_iface = vk_iface;
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SIMPLE] Vulkan interface stored for new handler initialization\n");
			}
#else
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_WARN, "[VULKAN] Vulkan libretro support not compiled in, falling back to OpenGL\n");
			}
#endif
		}
	}
}

// Vulkan context destroy callback
static void retro_vk_context_destroy()
{
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] Context destroy callback called\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Vulkan context destroy callback\n");

	// Clean up Vulkan resources
	if (m_virtualMachine && m_virtualMachine->GetGSHandler()) {
		auto gsHandler = m_virtualMachine->GetGSHandler();
		gsHandler->Release();
	}
}

// Vulkan negotiation interface function implementations
static const VkApplicationInfo* retro_vulkan_get_application_info(void)
{
	// Static VkApplicationInfo that RetroArch will use for Vulkan instance creation
	static const VkApplicationInfo app_info = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO,
		nullptr, // pNext
		"Play! PS2 Emulator", // pApplicationName
		VK_MAKE_VERSION(1, 0, 0), // applicationVersion
		"libretro", // pEngineName
		VK_MAKE_VERSION(1, 0, 0), // engineVersion
		VK_API_VERSION_1_1 // apiVersion - use 1.1 for optimal iOS/Android compatibility
	};
	
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] Providing application info: Play! PS2 Emulator, Vulkan 1.1\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Providing Vulkan application info\n");
	
	return &app_info;
}

static bool retro_vulkan_create_device(struct retro_vulkan_context *context,
										   VkInstance instance,
										   VkPhysicalDevice gpu,
										   VkSurfaceKHR surface,
										   PFN_vkGetInstanceProcAddr get_instance_proc_addr,
										   const char **required_device_extensions,
										   unsigned num_required_device_extensions,
										   const char **required_device_layers,
										   unsigned num_required_device_layers,
										   const VkPhysicalDeviceFeatures *required_features)
{
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] create_device called - letting frontend handle device creation\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Vulkan create_device called - using frontend default\n");
	
	// Return false to let the frontend handle device creation with its defaults
	// This is the recommended approach for most libretro cores
	// The frontend will create a suitable device and provide it via the hardware render interface
	return false;
}

static void retro_vulkan_destroy_device(void)
{
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[VULKAN] destroy_device called\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "Vulkan destroy_device called\n");
	
	// Nothing to clean up since we let the frontend handle device creation
	// Any core-specific Vulkan resources are cleaned up in retro_vk_context_destroy
}

#endif

static bool UseVulkanBackend()
{
	struct retro_variable var = {"play_graphics_backend", nullptr};
	bool get_result = g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
	
	// Debug logging
	CLog::GetInstance().Print(LOG_NAME, "[DEBUG] UseVulkanBackend: GET_VARIABLE result=%s, value=%s\n", 
		get_result ? "SUCCESS" : "FAILED", var.value ? var.value : "NULL");
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_DEBUG, "[DEBUG] UseVulkanBackend: GET_VARIABLE result=%s, value=%s\n", 
			get_result ? "SUCCESS" : "FAILED", var.value ? var.value : "NULL");
	}
	
	if(get_result && var.value)
	{
		bool is_vulkan = strcmp(var.value, "Vulkan") == 0;
		CLog::GetInstance().Print(LOG_NAME, "[DEBUG] UseVulkanBackend: Comparing '%s' with 'Vulkan' = %s\n", 
			var.value, is_vulkan ? "TRUE" : "FALSE");
		return is_vulkan;
	}
	CLog::GetInstance().Print(LOG_NAME, "[DEBUG] UseVulkanBackend: Defaulting to OpenGL\n");
	return false; // Default to OpenGL
}

void SetupVideoHandler()
{
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[SETUP] SetupVideoHandler called\n");
	}
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	auto gsHandler = m_virtualMachine->GetGSHandler();
	if(!gsHandler)
	{
#ifdef __APPLE__
		if(UseVulkanBackend())
		{
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SETUP] Creating Vulkan graphics handler\n");
			}
			CLog::GetInstance().Print(LOG_NAME, "Creating Vulkan graphics handler\n");
			try {
				m_virtualMachine->CreateGSHandler(CGSH_Vulkan_Libretro::GetFactoryFunction());
				if (g_log_cb) {
					g_log_cb(RETRO_LOG_INFO, "[SETUP] Vulkan graphics handler created successfully\n");
				}
			} catch (const std::exception& ex) {
				if (g_log_cb) {
					g_log_cb(RETRO_LOG_ERROR, "[SETUP] Vulkan graphics handler creation failed: %s\n", ex.what());
				}
				CLog::GetInstance().Warn(LOG_NAME, "Vulkan graphics handler creation failed: %s\n", ex.what());
				// Fall back to OpenGL
				if (g_log_cb) {
					g_log_cb(RETRO_LOG_WARN, "[SETUP] Falling back to OpenGL graphics handler\n");
				}
				m_virtualMachine->CreateGSHandler(CGSH_OpenGL_Libretro::GetFactoryFunction());
			}
		}
		else
#endif
		{
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SETUP] Creating OpenGL graphics handler\n");
			}
			CLog::GetInstance().Print(LOG_NAME, "Creating OpenGL graphics handler\n");
			m_virtualMachine->CreateGSHandler(CGSH_OpenGL_Libretro::GetFactoryFunction());
		}
	}
	else
	{
#ifdef __APPLE__
		if(UseVulkanBackend())
		{
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SETUP] Resetting existing Vulkan graphics handler\n");
			}
			auto retro_gs = static_cast<CGSH_Vulkan_Libretro*>(gsHandler);
			try {
				retro_gs->Reset();
				if (g_log_cb) {
					g_log_cb(RETRO_LOG_INFO, "[SETUP] Vulkan graphics handler reset successfully\n");
				}
			} catch (const std::exception& ex) {
				if (g_log_cb) {
					g_log_cb(RETRO_LOG_ERROR, "[SETUP] Vulkan graphics handler reset failed: %s\n", ex.what());
				}
				CLog::GetInstance().Warn(LOG_NAME, "Vulkan graphics handler reset failed: %s\n", ex.what());
			}
		}
		else
#endif
		{
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_INFO, "[SETUP] Resetting existing OpenGL graphics handler\n");
			}
			auto retro_gs = static_cast<CGSH_OpenGL_Libretro*>(gsHandler);
			retro_gs->Reset();
		}
	}
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[SETUP] SetupVideoHandler completed\n");
	}
}

static void retro_context_destroy()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

static void retro_context_reset()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		SetupVideoHandler();
	}
}

void SetupSoundHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->CreateSoundHandler(&CSH_LibreAudio::HandlerFactory);
	}
}

void SetupInputHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(!m_virtualMachine->GetPadHandler())
	{
		static struct retro_input_descriptor descDS2[] =
		    {
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},
		        {0},
		    };

		g_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descDS2);

		static const struct retro_controller_description controllers[] = {
		    {"PS2 DualShock2", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)},
		};

		static const struct retro_controller_info ports[] = {
		    {controllers, 1},
		    {NULL, 0},
		};

		g_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

		for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
		{
			auto ds2_button = static_cast<PS2::CControllerInfo::BUTTON>(i);
			auto retro_button = descDS2[i].id;
			g_ds2_to_retro_btn_map[ds2_button] = retro_button;
		}

		m_virtualMachine->CreatePadHandler(CPH_Libretro_Input::GetFactoryFunction());
	}
}

void retro_get_system_info(struct retro_system_info* info)
{
	*info = {};
	info->library_name = "Play!";
	info->library_version = PLAY_VERSION;
	info->need_fullpath = true;
	info->valid_extensions = "elf|iso|cso|isz|cue|chd";
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	*info = {};
	info->timing.fps = 60.0;
	info->timing.sample_rate = 44100;
	// SCALING FIX: Report actual image dimensions to prevent squishing
	// We're using 1024x1024 images to match Play!'s DRAW_AREA_SIZE
	info->geometry.base_width = 1024;
	info->geometry.base_height = 1024;
	info->geometry.max_width = 1024;
	info->geometry.max_height = 1024;
	info->geometry.aspect_ratio = 1.0; // Square aspect ratio for 1024x1024
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_video_cb = cb;
}

void retro_set_environment(retro_environment_t cb)
{
	g_environ_cb = cb;

	// Set up libretro logging interface
	struct retro_log_callback log_cb;
	if (g_environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
	{
		g_log_cb = log_cb.log;
	}

	// Register core options early - this MUST be done in retro_set_environment()
	// according to libretro standards, not in retro_load_game()
	bool options_result = g_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)m_vars.data());

	// Debug: Always log this, even without g_log_cb
	CLog::GetInstance().Print(LOG_NAME, "[ENV] Core options registration attempted, result: %s\n", options_result ? "SUCCESS" : "FAILED");
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[ENV] Core options registered in retro_set_environment, result: %s\n", options_result ? "SUCCESS" : "FAILED");
	}
}

void retro_set_input_poll(retro_input_poll_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_state_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_set_audio_sample_batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

unsigned retro_get_region(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return RETRO_REGION_NTSC;
}

size_t retro_serialize_size(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return 40 * 1024 * 1024;
}

bool retro_serialize(void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	try
	{
		Framework::CMemStream stateStream;
		Framework::CZipArchiveWriter archive;

		m_virtualMachine->m_ee->SaveState(archive);
		m_virtualMachine->m_iop->SaveState(archive);
		m_virtualMachine->m_ee->m_gs->SaveState(archive);

		archive.Write(stateStream);
		stateStream.Seek(0, Framework::STREAM_SEEK_DIRECTION::STREAM_SEEK_SET);
		stateStream.Read(data, size);
	}
	catch(...)
	{
		return false;
	}

	return true;
}

bool retro_unserialize(const void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	try
	{
		Framework::CPtrStream stateStream(data, size);
		Framework::CZipArchiveReader archive(stateStream);

		try
		{
			m_virtualMachine->m_ee->LoadState(archive);
			m_virtualMachine->m_iop->LoadState(archive);
			m_virtualMachine->m_ee->m_gs->LoadState(archive);
		}
		catch(...)
		{
			//Any error that occurs in the previous block is critical
			throw;
		}
	}
	catch(...)
	{
		return false;
	}

	m_virtualMachine->OnMachineStateChange();
	return true;
}

void* retro_get_memory_data(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return m_virtualMachine->m_ee->m_ram;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return PS2::EE_RAM_SIZE;
	}
	return 0;
}

void retro_cheat_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	(void)index;
	(void)enabled;
	(void)code;
}

void updateVars()
{
	for(int i = 0; i < m_vars.size() - 1; ++i)
	{
		auto item = m_vars[i];
		if(!item.key)
			continue;

		struct retro_variable var = {nullptr};
		var.key = item.key;
		if(g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool videoUpdate = false;
			switch(i)
			{
			case 0:
			{
				std::string val = var.value;
				auto res_factor = std::atoi(val.substr(0, -1).c_str());
				if(res_factor != g_res_factor)
				{
					g_res_factor = res_factor;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, res_factor);
					videoUpdate = true;
				}
			}
			break;
			case 1:
			{
				CGSHandler::PRESENTATION_MODE presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FIT;

				std::string val(var.value);
				if(val == "Fill Screen")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FILL;
				else if(val == "Original Size")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_ORIGINAL;

				if(presentation_mode != g_presentation_mode)
				{
					g_presentation_mode = presentation_mode;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE, presentation_mode);
					videoUpdate = true;
				}
			}
			break;
			case 2:
			{
				bool forceBilinearTextures = (std::string(var.value) == "true");
				if(forceBilinearTextures != g_forceBilinearTextures)
				{
					g_forceBilinearTextures = forceBilinearTextures;
					CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, forceBilinearTextures);
					videoUpdate = true;
				}
			}
			break;
			}

			if(videoUpdate)
			{
				if(m_virtualMachine)
					if(m_virtualMachine->GetGSHandler())
						static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler())->UpdatePresentation();
			}
		}
	}
}

void checkVarsUpdates()
{
	static bool updates = true;
	if(!updates)
		g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updates);

	if(updates)
	{
		updateVars();
	}
	updates = false;
}

void retro_run()
{
	// CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	checkVarsUpdates();

	if(!first_run)
	{
		if(m_virtualMachine)
		{
			// m_virtualMachine->Pause();
			m_virtualMachine->Reset();
			if(m_bootCommand.type == BootType::CD)
			{
				m_virtualMachine->m_ee->m_os->BootFromCDROM();
			}
			else
			{
				m_virtualMachine->m_ee->m_os->BootFromFile(m_bootCommand.path);
			}
			m_virtualMachine->Resume();
			first_run = true;
			CLog::GetInstance().Print(LOG_NAME, "%s\n", "Start Game");
		}
	}

	if(m_virtualMachine)
	{
		auto pad = m_virtualMachine->GetPadHandler();
		if(pad)
			static_cast<CPH_Libretro_Input*>(pad)->UpdateInputState();

		if(m_virtualMachine->GetSoundHandler())
			static_cast<CSH_LibreAudio*>(m_virtualMachine->GetSoundHandler())->ProcessBuffer();

		if(m_virtualMachine->GetGSHandler())
			m_virtualMachine->GetGSHandler()->ProcessSingleFrame();
	}
}

void retro_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		if(!m_virtualMachine->GetGSHandler())
			SetupVideoHandler();
		// m_virtualMachine->Pause();
		m_virtualMachine->Reset();
		m_virtualMachine->m_ee->m_os->BootFromCDROM();
		m_virtualMachine->Resume();
		CLog::GetInstance().Print(LOG_NAME, "%s\n", "Reset Game");
	}
	first_run = false;
}

bool retro_load_game(const retro_game_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

#if defined(IOS)
	bool can_jit = false;
	if(g_environ_cb(RETRO_ENVIRONMENT_GET_JIT_CAPABLE, &can_jit) && !can_jit)
	{
		// trying to run without the jit will cause a crash.
		retro_message retromsg;
		retromsg.msg = "Cannot run without JIT";
		retromsg.frames = 5000 / 17;
		g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retromsg);
		return false;
	}
#endif

	fs::path filePath = info->path;
	if(BootableUtils::IsBootableExecutablePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::ELF, filePath);
	}
	else if(BootableUtils::IsBootableDiscImagePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::CD, filePath);
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);
		CAppConfig::GetInstance().Save();
	}
	first_run = false;

	auto rgb = RETRO_PIXEL_FORMAT_XRGB8888;
	g_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb);

	// Debug: Check if we're on Apple platform and using Vulkan
	if (g_log_cb) {
		g_log_cb(RETRO_LOG_INFO, "[LOAD] Platform check: __APPLE__ defined, UseVulkanBackend() = %s\n", UseVulkanBackend() ? "true" : "false");
	}
	CLog::GetInstance().Print(LOG_NAME, "Platform check: __APPLE__ defined, UseVulkanBackend() = %s\n", UseVulkanBackend() ? "true" : "false");

#ifdef __APPLE__
	if(UseVulkanBackend())
	{
		if (g_log_cb) {
			g_log_cb(RETRO_LOG_INFO, "[LOAD] Setting up Vulkan hardware render with negotiation interface\n");
		}
		CLog::GetInstance().Print(LOG_NAME, "Setting up Vulkan hardware render with negotiation interface\n");
		
		// CRITICAL: Set up the hardware render context negotiation interface FIRST
		// This tells RetroArch we want Vulkan, not OpenGL
		if (!g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&hw_render_negotiation)) {
			if (g_log_cb) {
				g_log_cb(RETRO_LOG_WARN, "[LOAD] Failed to set Vulkan negotiation interface, falling back to OpenGL\n");
			}
			// Fall back to OpenGL if negotiation fails
			goto setup_opengl;
		}
		
		// Set up Vulkan hardware render context
		g_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
		g_hw_render.version_major = VK_API_VERSION_1_0;
		g_hw_render.version_minor = 0;
		// Set Vulkan-specific callbacks
		g_hw_render.context_reset = retro_vk_context_reset;
		g_hw_render.context_destroy = retro_vk_context_destroy;
		
		if (g_log_cb) {
			g_log_cb(RETRO_LOG_INFO, "[LOAD] Vulkan hardware render setup complete\n");
		}
		CLog::GetInstance().Print(LOG_NAME, "Vulkan hardware render setup complete\n");
	}
	else
#endif
	{
setup_opengl:
		CLog::GetInstance().Print(LOG_NAME, "Setting up OpenGL context\n");
#ifdef GLES_COMPATIBILITY
		g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
#else
		g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
		g_hw_render.version_major = 3;
		g_hw_render.version_minor = 2;
		// Set OpenGL callbacks only for OpenGL backend
		g_hw_render.context_reset = retro_context_reset;
		g_hw_render.context_destroy = retro_context_destroy;
	}
	g_hw_render.cache_context = false;
	g_hw_render.bottom_left_origin = true;
	g_hw_render.depth = true;
	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	// Core options are now registered in retro_set_environment() where they belong
	// according to libretro standards

	return true;
}

void retro_unload_game(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return false;
}

void retro_init()
{
#ifdef __ANDROID__
	Framework::PathUtils::SetFilesDirPath(getenv("EXTERNAL_STORAGE"));
#endif
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(g_environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
		libretro_supports_bitmasks = true;

	CAppConfig::GetInstance().RegisterPreferenceInteger(PREF_AUDIO_SPUBLOCKCOUNT, 22);

	m_virtualMachine = new CPS2VM();
	m_virtualMachine->Initialize();

	//Disable frame limiter, RetroArch handles this on its own
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, false);
	m_virtualMachine->ReloadFrameRateLimit();

	SetupInputHandler();
	SetupSoundHandler();
	first_run = false;
}

void retro_deinit()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->PauseAsync();
		auto gsHandler = static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler());
		if(gsHandler)
		{
			// Note: since we've forced GS into running on this/main/libretro thread
			// we need to clear its queue, to prevent it from locking up VM
			while(m_virtualMachine->GetStatus() != CVirtualMachine::PAUSED)
			{
				std::this_thread::yield();
				gsHandler->Release();
			}
		}
		m_virtualMachine->DestroyPadHandler();
		m_virtualMachine->DestroyGSHandler();
		m_virtualMachine->DestroySoundHandler();
		m_virtualMachine->Destroy();
		delete m_virtualMachine;
		m_virtualMachine = nullptr;
	}
	libretro_supports_bitmasks = false;
}
