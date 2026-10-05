#pragma once

#include "Mumble.h"
#include "Nexus.h"

#include <chrono>
#include <d3d11.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct MockKeybindEntry
{
	std::string id;
	KEYBINDS_PROCESS callback;
	Keybind keybind;
	std::string keybind_string;
};

struct MockQuickAccessEntry
{
	std::string id;
	std::string texture_id;
	std::string keybind_id;
	std::string tooltip;
	std::vector<std::pair<std::string, GUI_RENDER>> context_menus;
};

struct PendingTextureUpload
{
	std::string id;
	std::vector<unsigned char> pixels; // RGBA, 4 bytes per pixel
	int width = 0;
	int height = 0;
	TEXTURES_RECEIVECALLBACK callback = nullptr;
};

struct MockAlert
{
	std::string message;
	std::chrono::steady_clock::time_point created;
};

struct MockState
{
	std::vector<GUI_RENDER> pre_render_callbacks;
	std::vector<GUI_RENDER> render_callbacks;
	std::vector<GUI_RENDER> post_render_callbacks;
	std::vector<GUI_RENDER> options_render_callbacks;

	std::vector<MockKeybindEntry> keybinds;
	std::vector<WNDPROC_CALLBACK> wndproc_callbacks;
	std::vector<MockQuickAccessEntry> quick_access;

	std::map<std::string, std::vector<EVENT_CONSUME>> events_subscribers;
	std::vector<std::pair<std::string, bool*>> close_on_escape;

	std::vector<MockAlert> alerts;
	std::mutex alerts_mutex;

	std::map<std::string, Texture*> textures;
	std::mutex textures_mutex;

	std::vector<PendingTextureUpload> pending_uploads;
	std::mutex pending_uploads_mutex;

	// textures that were requested once, so per-frame GetOrCreate calls do not repeat downloads
	std::set<std::string> requested_textures;
	std::mutex requested_textures_mutex;

	std::map<std::string, std::vector<uint8_t>> shared_blocks;

	Mumble::Data mumble_data = {};
	Mumble::Identity mumble_identity = {};
	NexusLinkData nexus_link_data = {};
};

MockState& get_mock_state();
AddonAPI* create_mock_api();

void set_mock_device(ID3D11Device* device);
void flush_pending_texture_uploads();
Texture* mock_textures_get(const char* id);
void invoke_keybind(const char* id, bool is_released);
bool process_keybind_message(UINT msg, WPARAM wParam, LPARAM lParam);
