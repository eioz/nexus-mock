#include "mock_api.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#ifndef NEXUS_MOCK_DATA_DIR
#define NEXUS_MOCK_DATA_DIR "."
#endif

namespace mac_mock
{
static State mock_state;
static Texture_t mock_texture;

State& state() { return mock_state; }

static void gui_register(ERenderType type, GUI_RENDER callback)
{
	if (callback == nullptr)
		return;

	if (type == RT_Render)
		state().render_callbacks.push_back(callback);
	else if (type == RT_OptionsRender)
		state().options_render_callbacks.push_back(callback);
}

static void gui_deregister(GUI_RENDER callback)
{
	auto remove_callback = [callback](auto& callbacks) { callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), callback), callbacks.end()); };
	remove_callback(state().render_callbacks);
	remove_callback(state().options_render_callbacks);
}

static void log(ELogLevel level, const char* channel, const char* message)
{
	const char* tag = "INFO";
	switch (level)
	{
	case LOGL_CRITICAL:
		tag = "CRIT";
		break;
	case LOGL_WARNING:
		tag = "WARN";
		break;
	case LOGL_DEBUG:
		tag = "DBG";
		break;
	case LOGL_TRACE:
		tag = "TRC";
		break;
	default:
		break;
	}

	std::cout << "[" << tag << "] [" << (channel ? channel : "mock") << "] " << (message ? message : "") << '\n';
}

static const char* addon_directory(const char* name)
{
	if (name == nullptr)
		name = "";

	auto& directories = state().addon_directories;
	auto& path = directories[name];
	if (path.empty())
	{
		auto directory = std::filesystem::path(NEXUS_MOCK_DATA_DIR);
		if (*name != '\0')
			directory /= name;

		std::filesystem::create_directories(directory);
		path = directory.string();
	}

	return path.c_str();
}

static void* datalink_get(const char* identifier)
{
	if (identifier != nullptr && std::string(identifier) == DL_MUMBLE_LINK)
		return &state().mumble_data;
	return nullptr;
}

static void keybind_register(const char* id, INPUTBINDS_PROCESS callback, const char* keybind)
{
	if (id == nullptr)
		return;

	KeybindEntry entry;
	entry.id = id;
	entry.callback = callback;
	entry.keybind = keybind ? keybind : "";

	auto& keybinds = state().keybinds;
	auto it = std::find_if(keybinds.begin(), keybinds.end(), [&](const auto& current) { return current.id == entry.id; });
	if (it == keybinds.end())
		keybinds.push_back(std::move(entry));
	else
		*it = std::move(entry);
}

static void keybind_deregister(const char* id)
{
	if (id == nullptr)
		return;

	auto& keybinds = state().keybinds;
	keybinds.erase(std::remove_if(keybinds.begin(), keybinds.end(), [&](const auto& entry) { return entry.id == id; }), keybinds.end());
}

void invoke_keybind(const char* id, bool is_released)
{
	if (id == nullptr)
		return;

	auto snapshot = state().keybinds;
	for (const auto& entry : snapshot)
	{
		if (entry.id == id && entry.callback != nullptr)
		{
			entry.callback(id, is_released);
			return;
		}
	}
}

static void texture_load_from_memory(const char*, void*, uint64_t, TEXTURES_RECEIVECALLBACK callback)
{
	if (callback != nullptr)
		callback("mock-texture", &mock_texture);
}

static void quick_access_add(const char* id, const char*, const char*, const char* keybind_id, const char* tooltip)
{
	if (id == nullptr)
		return;

	QuickAccessEntry entry;
	entry.id = id;
	entry.keybind_id = keybind_id ? keybind_id : "";
	entry.tooltip = tooltip ? tooltip : "";

	auto& entries = state().quick_access;
	auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& current) { return current.id == entry.id; });
	if (it == entries.end())
		entries.push_back(std::move(entry));
	else
		*it = std::move(entry);
}

static void quick_access_remove(const char* id)
{
	if (id == nullptr)
		return;

	auto& entries = state().quick_access;
	entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.id == id; }), entries.end());
}

void render_quick_access()
{
	auto& entries = state().quick_access;
	if (entries.empty())
		return;

	ImGui::Begin("Mock Quick Access");
	for (const auto& entry : entries)
	{
		if (ImGui::Button(entry.tooltip.empty() ? entry.id.c_str() : entry.tooltip.c_str()))
			invoke_keybind(entry.keybind_id.c_str(), false);
	}
	ImGui::End();
}

AddonAPI_t* create_api(ImGuiContext* imgui_context)
{
	auto& api = state().api;
	api.ImguiContext = imgui_context;
	api.ImguiMalloc = reinterpret_cast<void*>(+[](size_t size, void*) { return std::malloc(size); });
	api.ImguiFree = reinterpret_cast<void*>(+[](void* ptr, void*) { std::free(ptr); });
	api.GUI_Register = gui_register;
	api.GUI_Deregister = gui_deregister;
	api.Log = log;
	api.Paths_GetAddonDirectory = addon_directory;
	api.DataLink_Get = datalink_get;
	api.InputBinds_RegisterWithString = keybind_register;
	api.InputBinds_Deregister = keybind_deregister;
	api.Textures_LoadFromMemory = texture_load_from_memory;
	api.QuickAccess_Add = quick_access_add;
	api.QuickAccess_Remove = quick_access_remove;
	return &api;
}
} // namespace mac_mock
