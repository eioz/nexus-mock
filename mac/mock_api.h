#pragma once

#include "Mumble.h"
#include <Nexus.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ImGuiContext;

namespace mac_mock
{
struct KeybindEntry
{
	std::string id;
	INPUTBINDS_PROCESS callback = nullptr;
	std::string keybind;
};

struct QuickAccessEntry
{
	std::string id;
	std::string keybind_id;
	std::string tooltip;
};

struct State
{
	std::vector<GUI_RENDER> render_callbacks;
	std::vector<GUI_RENDER> options_render_callbacks;
	std::vector<KeybindEntry> keybinds;
	std::vector<QuickAccessEntry> quick_access;
	std::map<std::string, std::string> addon_directories;
	Mumble::Data mumble_data = {};
	AddonAPI_t api = {};
	bool options_open = true;
};

State& state();
AddonAPI_t* create_api(ImGuiContext* imgui_context);
void invoke_keybind(const char* id, bool is_released);
void render_quick_access();
} // namespace mac_mock
