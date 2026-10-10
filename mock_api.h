#pragma once

#include "Mumble.h"
#include "Nexus.h"

#include <chrono>
#include <d3d11.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A bind as Nexus stores it: a keyboard scan code, with 0xE000 for extended keys, or a mouse button from 1 to 5.
struct MockBind
{
	enum class Device
	{
		Keyboard,
		Mouse
	};

	Device device = Device::Keyboard;
	unsigned short code = 0; // 0 is unbound
	bool alt = false;
	bool ctrl = false;
	bool shift = false;

	bool operator==(const MockBind&) const = default;
};

struct MockInputBind
{
	std::string id;
	INPUTBINDS_PROCESS handler = nullptr;
	MockBind bind;
	bool held = false;
};

struct MockShortcut
{
	std::string texture;
	std::string hover_texture;
	std::string input_bind;
	std::string tooltip;
	bool notified = false;
	std::map<std::string, GUI_RENDER> context_menu; // by item identifier, the order Nexus draws them in
};

struct MockAlert
{
	std::string message;
	std::chrono::steady_clock::time_point shown;
};

// What the addon registered. The mutex guards everything but the Mumble and Nexus links, which addons read without
// locking like shared memory. Addon callbacks are called on copies, never while the mutex is held.
struct MockState
{
	std::mutex mutex;

	std::vector<GUI_RENDER> pre_render;
	std::vector<GUI_RENDER> render;
	std::vector<GUI_RENDER> post_render;
	std::vector<GUI_RENDER> options_render;
	std::vector<WNDPROC_CALLBACK> wndprocs;
	std::vector<MockInputBind> input_binds;
	std::map<std::string, std::vector<EVENT_CONSUME>> events;
	std::vector<std::pair<std::string, bool*>> close_on_escape;
	std::map<std::string, MockShortcut> shortcuts; // sorted by identifier like the Nexus quick access
	std::vector<MockAlert> alerts;

	// modifiers, which Nexus tracks from key messages
	bool alt_down = false;
	bool ctrl_down = false;
	bool shift_down = false;

	Mumble::Data mumble = {};
	Mumble::Identity identity = {};
	NexusLinkData_t nexus_link = {};
};

// the quick access shortcut of Nexus itself, which gets the context menu items without a target
inline constexpr const char* NexusShortcut = "0_QA_MENU";

MockState& get_mock_state();

// The game directory holds addons\<name> like the Guild Wars 2 directory.
AddonAPI_t* create_mock_api(const std::filesystem::path& game_directory, ID3D11Device* device, IDXGISwapChain* swap_chain);
void shutdown_mock_api();

// Remembers the addon's image, which tells its callbacks apart from the mock's own.
void set_addon_module(HMODULE module, uint32_t signature);
// Drops everything the addon left registered in its image, like Nexus after Unload. Returns how much it dropped.
size_t release_addon_module();

// Registers the Nexus fonts with the given faces, ImGui's default font when empty. Returns false for a file that is no font.
bool add_nexus_fonts(const std::string& body_file, const std::string& heading_file);

// Once per frame before the ImGui frame, like Nexus: creates loaded textures, applies localized strings and rebuilds the
// font atlas. Returns false when the atlas cannot be built.
bool advance_mock_api(bool& fonts_rebuilt);

// The Nexus window procedure steps: addon callbacks, ImGui input, close on escape and input binds. Returns true when one
// consumed the message.
bool process_window_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Calls the handler on the input bind worker. Returns false when no handler is registered under the identifier.
bool invoke_input_bind(const std::string& id, bool release);
std::string bind_text(const MockBind& bind);
void wait_for_input_binds();

void raise_event(const char* id, void* data);

// Set the window size in the Nexus link and the DPI scale, which Nexus keeps in io.FontGlobalScale. The link scaling follows
// at the next frame.
void update_nexus_link(unsigned width, unsigned height);
void set_dpi_scale(float scale);

Texture_t* get_texture(const std::string& id);
// Loads a local image under an identifier that is not taken yet, for scripted tests.
bool preload_texture(const std::string& id, const std::filesystem::path& file);
std::vector<std::pair<std::string, void*>> texture_resources();
