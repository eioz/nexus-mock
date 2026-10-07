#include "mock_api.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <thread>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

static MockState s_state;
static ID3D11Device* s_device = nullptr;

MockState& get_mock_state() { return s_state; }

void set_mock_device(ID3D11Device* device) { s_device = device; }

// --- Renderer ---

static void mock_renderer_register(ERenderType type, GUI_RENDER callback)
{
	auto& state = get_mock_state();
	switch (type)
	{
	case ERenderType_PreRender:
		state.pre_render_callbacks.push_back(callback);
		break;
	case ERenderType_Render:
		state.render_callbacks.push_back(callback);
		break;
	case ERenderType_PostRender:
		state.post_render_callbacks.push_back(callback);
		break;
	case ERenderType_OptionsRender:
		state.options_render_callbacks.push_back(callback);
		break;
	}
}

static void mock_renderer_deregister(GUI_RENDER callback)
{
	auto& state = get_mock_state();
	auto remove_from = [&](std::vector<GUI_RENDER>& vec) { vec.erase(std::remove(vec.begin(), vec.end(), callback), vec.end()); };
	remove_from(state.pre_render_callbacks);
	remove_from(state.render_callbacks);
	remove_from(state.post_render_callbacks);
	remove_from(state.options_render_callbacks);
}

// --- Logging ---

static void mock_log(ELogLevel level, const char* channel, const char* message)
{
	const char* tag = "???";
	switch (level)
	{
	case ELogLevel_CRITICAL:
		tag = "CRIT";
		break;
	case ELogLevel_WARNING:
		tag = "WARN";
		break;
	case ELogLevel_INFO:
		tag = "INFO";
		break;
	case ELogLevel_DEBUG:
		tag = "DBG ";
		break;
	case ELogLevel_TRACE:
		tag = "TRAC";
		break;
	default:
		break;
	}
	printf("[%s] [%s] %s\n", tag, channel, message);
}

// --- Paths ---

static const char* mock_get_game_directory()
{
	static std::string path;
	if (path.empty())
		path = (std::filesystem::current_path() / "data").string();
	return path.c_str();
}

static const char* mock_get_addon_directory(const char* name)
{
	if (!name)
		return "";
	// Stable storage per addon name so previously returned pointers remain valid
	static std::map<std::string, std::string> directories;
	auto& path = directories[name];
	if (path.empty())
	{
		path = (std::filesystem::current_path() / "data" / name).string();
		std::filesystem::create_directories(path);
	}
	return path.c_str();
}

static const char* mock_get_common_directory()
{
	static std::string path;
	if (path.empty())
	{
		path = (std::filesystem::current_path() / "data" / "common").string();
		std::filesystem::create_directories(path);
	}
	return path.c_str();
}

// --- DataLink ---

static void* mock_datalink_get(const char* identifier)
{
	if (!identifier)
		return nullptr;
	auto& state = get_mock_state();
	if (strcmp(identifier, "DL_MUMBLE_LINK") == 0)
		return &state.mumble_data;
	if (strcmp(identifier, "DL_NEXUS_LINK") == 0)
		return &state.nexus_link_data;

	auto it = state.shared_blocks.find(identifier);
	return it != state.shared_blocks.end() ? it->second.data() : nullptr;
}

static void* mock_datalink_share(const char* identifier, size_t size)
{
	if (!identifier)
		return nullptr;
	auto& state = get_mock_state();
	auto& block = state.shared_blocks[identifier];
	if (block.size() < size)
		block.resize(size, 0);
	return block.data();
}

// --- InputBinds ---

static Keybind parse_keybind_string(const char* str)
{
	Keybind kb = {};
	if (!str || !*str)
		return kb;

	std::string s = str;
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (unsigned char)std::toupper(c); });

	size_t start = 0;
	while (start <= s.size())
	{
		auto pos = s.find('+', start);
		auto part = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
		start = pos == std::string::npos ? s.size() + 1 : pos + 1;

		if (part == "CTRL")
			kb.Ctrl = true;
		else if (part == "ALT")
			kb.Alt = true;
		else if (part == "SHIFT")
			kb.Shift = true;
		else if (part.length() == 1 && part[0] >= 'A' && part[0] <= 'Z')
			kb.Key = part[0];
		else if (part.length() == 1 && part[0] >= '0' && part[0] <= '9')
			kb.Key = part[0];
		else if (part.size() >= 2 && part[0] == 'F')
		{
			int fn = std::atoi(part.c_str() + 1);
			if (fn >= 1 && fn <= 24)
				kb.Key = (unsigned short)(VK_F1 + fn - 1);
		}
		else if (part == "SPACE")
			kb.Key = VK_SPACE;
		else if (part == "ENTER")
			kb.Key = VK_RETURN;
		else if (part == "ESC" || part == "ESCAPE")
			kb.Key = VK_ESCAPE;
		else if (part == "TAB")
			kb.Key = VK_TAB;
		else if (part == "DEL" || part == "DELETE")
			kb.Key = VK_DELETE;
		else if (part == "INS" || part == "INSERT")
			kb.Key = VK_INSERT;
		else if (part == "HOME")
			kb.Key = VK_HOME;
		else if (part == "END")
			kb.Key = VK_END;
		else if (part == "PGUP")
			kb.Key = VK_PRIOR;
		else if (part == "PGDN")
			kb.Key = VK_NEXT;
	}

	return kb;
}

void invoke_keybind(const char* id, bool is_released)
{
	auto snapshot = get_mock_state().keybinds;
	for (auto& entry : snapshot)
	{
		if (entry.id == id)
		{
			if (entry.callback)
				entry.callback(id, is_released);
			break;
		}
	}
}

bool process_keybind_message(UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP)
		return false;

	// Suppress auto-repeat key-down events (bit 30 of lParam is set when key was already held)
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (lParam & (1 << 30)))
		return false;

	bool is_release = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
	bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
	bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

	// CloseOnEscape: on ESC press with no modifiers, close all registered windows
	if (!is_release && wParam == VK_ESCAPE && !ctrl_down && !alt_down && !shift_down)
	{
		for (auto& [id, pBool] : get_mock_state().close_on_escape)
			if (pBool)
				*pBool = false;
	}

	auto snapshot = get_mock_state().keybinds;
	bool handled = false;

	for (auto& entry : snapshot)
	{
		if (entry.keybind.Key == (unsigned short)wParam && entry.keybind.Ctrl == ctrl_down && entry.keybind.Alt == alt_down && entry.keybind.Shift == shift_down)
		{
			if (entry.callback)
				entry.callback(entry.id.c_str(), is_release);
			handled = true;
		}
	}

	return handled;
}

static void mock_keybinds_invoke(const char* id, bool is_released) { invoke_keybind(id, is_released); }

static void mock_keybinds_register_string(const char* id, KEYBINDS_PROCESS callback, const char* keybind)
{
	auto& state = get_mock_state();
	MockKeybindEntry entry;
	entry.id = id;
	entry.callback = callback;
	entry.keybind = parse_keybind_string(keybind);
	entry.keybind_string = keybind ? keybind : "";

	auto it = std::find_if(state.keybinds.begin(), state.keybinds.end(), [id](const MockKeybindEntry& e) { return e.id == id; });
	if (it != state.keybinds.end())
		*it = std::move(entry);
	else
		state.keybinds.push_back(std::move(entry));

	printf("[MOCK] Keybind registered: %s -> %s\n", id, keybind ? keybind : "");
}

static void mock_keybinds_register_struct(const char* id, KEYBINDS_PROCESS callback, Keybind keybind)
{
	auto& state = get_mock_state();
	MockKeybindEntry entry;
	entry.id = id;
	entry.callback = callback;
	entry.keybind = keybind;
	if (keybind.Ctrl)
		entry.keybind_string += "CTRL+";
	if (keybind.Alt)
		entry.keybind_string += "ALT+";
	if (keybind.Shift)
		entry.keybind_string += "SHIFT+";
	if (keybind.Key >= 'A' && keybind.Key <= 'Z')
		entry.keybind_string += (char)keybind.Key;
	else if (keybind.Key >= '0' && keybind.Key <= '9')
		entry.keybind_string += (char)keybind.Key;
	else
		entry.keybind_string += "0x" + std::to_string(keybind.Key);

	std::string kb_str = entry.keybind_string;
	auto it = std::find_if(state.keybinds.begin(), state.keybinds.end(), [id](const MockKeybindEntry& e) { return e.id == id; });
	if (it != state.keybinds.end())
		*it = std::move(entry);
	else
		state.keybinds.push_back(std::move(entry));

	printf("[MOCK] Keybind registered: %s -> %s\n", id, kb_str.c_str());
}

static void mock_keybinds_deregister(const char* id)
{
	auto& state = get_mock_state();
	state.keybinds.erase(std::remove_if(state.keybinds.begin(), state.keybinds.end(), [id](const MockKeybindEntry& e) { return e.id == id; }), state.keybinds.end());
	printf("[MOCK] Keybind deregistered: %s\n", id);
}

// --- WndProc ---

static void mock_wndproc_register(WNDPROC_CALLBACK callback) { get_mock_state().wndproc_callbacks.push_back(callback); }

static void mock_wndproc_deregister(WNDPROC_CALLBACK callback)
{
	auto& cbs = get_mock_state().wndproc_callbacks;
	cbs.erase(std::remove(cbs.begin(), cbs.end(), callback), cbs.end());
}

static LRESULT mock_wndproc_send_to_game(HWND, UINT, WPARAM, LPARAM) { return 0; }

// --- QuickAccess ---

static void mock_quickaccess_add(const char* id, const char* texture_id, const char*, const char* keybind_id, const char* tooltip)
{
	if (!id)
		return;
	auto& entries = get_mock_state().quick_access;
	auto it = std::find_if(entries.begin(), entries.end(), [id](const auto& e) { return e.id == id; });
	if (it != entries.end())
	{
		it->texture_id = texture_id ? texture_id : "";
		it->keybind_id = keybind_id ? keybind_id : "";
		it->tooltip = tooltip ? tooltip : "";
	}
	else
	{
		entries.push_back({ id, texture_id ? texture_id : "", keybind_id ? keybind_id : "", tooltip ? tooltip : "", {} });
	}
	printf("[MOCK] QuickAccess added: %s (%s)\n", id, tooltip ? tooltip : "");
}

static void mock_quickaccess_remove(const char* id)
{
	if (!id)
		return;
	auto& entries = get_mock_state().quick_access;
	entries.erase(std::remove_if(entries.begin(), entries.end(), [id](const auto& e) { return e.id == id; }), entries.end());
	printf("[MOCK] QuickAccess removed: %s\n", id);
}

static void mock_quickaccess_notify(const char*) {}

static void mock_quickaccess_add_context_menu(const char* menu_id, const char* shortcut_id, GUI_RENDER callback)
{
	auto& entries = get_mock_state().quick_access;
	auto it = std::find_if(entries.begin(), entries.end(), [shortcut_id](const auto& e) { return e.id == shortcut_id; });
	if (it != entries.end())
		it->context_menus.push_back({ menu_id ? menu_id : "", callback });
}

static void mock_quickaccess_remove_context_menu(const char* menu_id)
{
	if (!menu_id)
		return;
	for (auto& entry : get_mock_state().quick_access)
		entry.context_menus.erase(std::remove_if(entry.context_menus.begin(), entry.context_menus.end(), [menu_id](const auto& m) { return m.first == menu_id; }), entry.context_menus.end());
}

// --- Textures ---

// Helper used by background threads — decodes image bytes and queues for main-thread D3D11 upload
static void queue_texture_upload(const std::string& id, const unsigned char* file_data, int file_size, TEXTURES_RECEIVECALLBACK callback)
{
	int w, h, channels;
	unsigned char* pixels = stbi_load_from_memory(file_data, file_size, &w, &h, &channels, 4);
	if (!pixels)
	{
		printf("[MOCK] Texture decode failed: %s\n", id.c_str());
		return;
	}

	PendingTextureUpload upload;
	upload.id = id;
	upload.pixels.assign(pixels, pixels + w * h * 4);
	upload.width = w;
	upload.height = h;
	upload.callback = callback;
	stbi_image_free(pixels);

	std::lock_guard<std::mutex> lock(s_state.pending_uploads_mutex);
	s_state.pending_uploads.push_back(std::move(upload));
}

// Helper for synchronous file-based loading (no thread needed)
static void queue_texture_upload_from_file(const std::string& id, const std::string& path, TEXTURES_RECEIVECALLBACK callback)
{
	int w, h, channels;
	unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
	if (!pixels)
	{
		printf("[MOCK] Texture file load failed: %s (%s)\n", id.c_str(), path.c_str());
		return;
	}

	PendingTextureUpload upload;
	upload.id = id;
	upload.pixels.assign(pixels, pixels + w * h * 4);
	upload.width = w;
	upload.height = h;
	upload.callback = callback;
	stbi_image_free(pixels);

	std::lock_guard<std::mutex> lock(s_state.pending_uploads_mutex);
	s_state.pending_uploads.push_back(std::move(upload));
}

static std::string build_url(const std::string& remote, const std::string& endpoint)
{
	std::string url = remote;
	if (!url.empty() && url.back() != '/' && !endpoint.empty() && endpoint.front() != '/')
		url += '/';
	url += endpoint;
	return url;
}

static bool wininet_download(const std::string& url, std::vector<unsigned char>& out)
{
	HINTERNET inet = InternetOpenA("NexusMock/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!inet)
		return false;

	DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
	if (url.rfind("https://", 0) == 0)
		flags |= INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
	HINTERNET hurl = InternetOpenUrlA(inet, url.c_str(), nullptr, 0, flags, 0);
	if (!hurl)
	{
		printf("[MOCK] WinInet open URL failed (error %lu): %s\n", GetLastError(), url.c_str());
		InternetCloseHandle(inet);
		return false;
	}

	// Verify HTTP 200 before reading body
	DWORD status = 0, status_len = sizeof(status);
	if (HttpQueryInfoA(hurl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &status_len, nullptr) && status != 200)
	{
		printf("[MOCK] Texture URL returned HTTP %lu: %s\n", status, url.c_str());
		InternetCloseHandle(hurl);
		InternetCloseHandle(inet);
		return false;
	}

	char buf[8192];
	DWORD bytes_read;
	while (InternetReadFile(hurl, buf, sizeof(buf), &bytes_read) && bytes_read > 0)
		out.insert(out.end(), buf, buf + bytes_read);

	InternetCloseHandle(hurl);
	InternetCloseHandle(inet);
	return !out.empty();
}

// Called each frame on the main thread — creates D3D11 resources and fires callbacks
void flush_pending_texture_uploads()
{
	if (!s_device)
		return;

	std::vector<PendingTextureUpload> uploads;
	{
		std::lock_guard<std::mutex> lock(s_state.pending_uploads_mutex);
		uploads.swap(s_state.pending_uploads);
	}

	for (auto& u : uploads)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = (UINT)u.width;
		desc.Height = (UINT)u.height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = u.pixels.data();
		init.SysMemPitch = (UINT)(u.width * 4);

		ID3D11Texture2D* tex2d = nullptr;
		if (FAILED(s_device->CreateTexture2D(&desc, &init, &tex2d)))
		{
			printf("[MOCK] D3D11 CreateTexture2D failed: %s\n", u.id.c_str());
			continue;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		ID3D11ShaderResourceView* srv = nullptr;
		s_device->CreateShaderResourceView(tex2d, &srv_desc, &srv);
		tex2d->Release();

		if (!srv)
		{
			printf("[MOCK] D3D11 CreateShaderResourceView failed: %s\n", u.id.c_str());
			continue;
		}

		auto* tex = new Texture{ (UINT)u.width, (UINT)u.height, srv };
		{
			std::lock_guard<std::mutex> lock(s_state.textures_mutex);
			s_state.textures[u.id] = tex;
		}

		printf("[MOCK] Texture uploaded: %s (%dx%d)\n", u.id.c_str(), u.width, u.height);

		if (u.callback)
			u.callback(u.id.c_str(), tex);
	}
}

static Texture* mock_textures_get_internal(const char* id)
{
	std::lock_guard<std::mutex> lock(s_state.textures_mutex);
	auto it = s_state.textures.find(id);
	return it != s_state.textures.end() ? it->second : nullptr;
}

Texture* mock_textures_get(const char* id) { return mock_textures_get_internal(id); }

// Forward declaration (defined with the other load_from_* functions below)
static void mock_textures_load_from_resource(const char* id, unsigned resource_id, HMODULE module, TEXTURES_RECEIVECALLBACK callback);

static Texture* mock_textures_get_or_create_file(const char* id, const char* filename)
{
	if (auto* existing = mock_textures_get_internal(id))
		return existing;
	queue_texture_upload_from_file(id, filename ? filename : "", nullptr);
	return nullptr;
}

static Texture* mock_textures_get_or_create_resource(const char* id, unsigned resource_id, HMODULE module)
{
	if (auto* existing = mock_textures_get_internal(id))
		return existing;
	mock_textures_load_from_resource(id, resource_id, module, nullptr);
	return nullptr;
}

static Texture* mock_textures_get_or_create_url(const char* id, const char* remote, const char* endpoint)
{
	if (auto* existing = mock_textures_get_internal(id))
		return existing;

	std::string sid = id, url = build_url(remote ? remote : "", endpoint ? endpoint : "");
	std::thread([sid, url]() {
		std::vector<unsigned char> data;
		if (wininet_download(url, data))
			queue_texture_upload(sid, data.data(), (int)data.size(), nullptr);
		else
			printf("[MOCK] Texture URL download failed: %s\n", sid.c_str());
	}).detach();

	return nullptr;
}

static Texture* mock_textures_get_or_create_memory(const char* id, void* data, size_t size)
{
	if (auto* existing = mock_textures_get_internal(id))
		return existing;

	PendingTextureUpload upload;
	upload.id = id;
	int w, h, channels;
	unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &channels, 4);
	if (!pixels)
		return nullptr;
	upload.pixels.assign(pixels, pixels + w * h * 4);
	upload.width = w;
	upload.height = h;
	stbi_image_free(pixels);

	std::lock_guard<std::mutex> lock(s_state.pending_uploads_mutex);
	s_state.pending_uploads.push_back(std::move(upload));
	return nullptr;
}

static void mock_textures_load_from_file(const char* id, const char* filename, TEXTURES_RECEIVECALLBACK callback)
{
	std::string sid = id, sfile = filename ? filename : "";
	std::thread([sid, sfile, callback]() { queue_texture_upload_from_file(sid, sfile, callback); }).detach();
}

static void mock_textures_load_from_resource(const char* id, unsigned resource_id, HMODULE module, TEXTURES_RECEIVECALLBACK callback)
{
	HRSRC hres = FindResourceA(module, MAKEINTRESOURCEA(resource_id), "PNG");
	if (!hres)
	{
		printf("[MOCK] LoadFromResource: FindResource failed for id=%u (error %lu)\n", resource_id, GetLastError());
		return;
	}
	HGLOBAL hdata = LoadResource(module, hres);
	if (!hdata)
	{
		printf("[MOCK] LoadFromResource: LoadResource failed for id=%u\n", resource_id);
		return;
	}
	DWORD size = SizeofResource(module, hres);
	const void* ptr = LockResource(hdata);
	if (!ptr || size == 0)
	{
		printf("[MOCK] LoadFromResource: LockResource failed for id=%u\n", resource_id);
		return;
	}

	std::string sid = id;
	std::vector<unsigned char> bytes(static_cast<const unsigned char*>(ptr), static_cast<const unsigned char*>(ptr) + size);
	printf("[MOCK] Texture load from resource: id=%u -> %s (%lu bytes)\n", resource_id, id, size);
	std::thread([sid, bytes, callback]() { queue_texture_upload(sid, bytes.data(), (int)bytes.size(), callback); }).detach();
}

static void mock_textures_load_from_url(const char* id, const char* remote, const char* endpoint, TEXTURES_RECEIVECALLBACK callback)
{
	std::string sid = id, url = build_url(remote ? remote : "", endpoint ? endpoint : "");
	printf("[MOCK] Texture load from URL: %s -> %s\n", url.c_str(), id);

	std::thread([sid, url, callback]() {
		std::vector<unsigned char> data;
		if (wininet_download(url, data))
			queue_texture_upload(sid, data.data(), (int)data.size(), callback);
		else
			printf("[MOCK] Texture URL download failed: %s\n", sid.c_str());
	}).detach();
}

static void mock_textures_load_from_memory(const char* id, void* data, size_t size, TEXTURES_RECEIVECALLBACK callback)
{
	std::vector<unsigned char> copy((unsigned char*)data, (unsigned char*)data + size);
	std::string sid = id;
	std::thread([sid, copy = std::move(copy), callback]() { queue_texture_upload(sid, copy.data(), (int)copy.size(), callback); }).detach();
}

// --- Events ---

static void mock_events_raise(const char* event_name, void* data)
{
	if (!event_name)
		return;
	auto& subs = get_mock_state().events_subscribers;
	auto it = subs.find(event_name);
	if (it == subs.end())
		return;
	// Copy to avoid iterator invalidation if a callback unsubscribes during dispatch
	auto snapshot = it->second;
	for (auto& cb : snapshot)
		if (cb)
			cb(data);
}

static void mock_events_raise_notification(const char* event_name) { mock_events_raise(event_name, nullptr); }

static void mock_events_raise_targeted(signed int, const char*, void*) {}
static void mock_events_raise_notification_targeted(signed int, const char*) {}

static void mock_events_subscribe(const char* event_name, EVENT_CONSUME callback)
{
	if (!event_name || !callback)
		return;
	auto& subs = get_mock_state().events_subscribers;
	auto& vec = subs[event_name];
	if (std::find(vec.begin(), vec.end(), callback) == vec.end())
		vec.push_back(callback);
}

static void mock_events_unsubscribe(const char* event_name, EVENT_CONSUME callback)
{
	if (!event_name || !callback)
		return;
	auto& subs = get_mock_state().events_subscribers;
	auto it = subs.find(event_name);
	if (it == subs.end())
		return;
	it->second.erase(std::remove(it->second.begin(), it->second.end(), callback), it->second.end());
	if (it->second.empty())
		subs.erase(it);
}

// --- UI ---

static void mock_alerts_notify(const char* message)
{
	if (!message)
		return;
	std::lock_guard<std::mutex> lock(get_mock_state().alerts_mutex);
	get_mock_state().alerts.push_back({ message, std::chrono::steady_clock::now() });
}

static void mock_register_close_on_escape(const char* id, bool* pBool)
{
	if (!id || !pBool)
		return;
	auto& list = get_mock_state().close_on_escape;
	auto it = std::find_if(list.begin(), list.end(), [id](const auto& e) { return e.first == id; });
	if (it == list.end())
		list.push_back({ id, pBool });
	else
		it->second = pBool;
}

static void mock_deregister_close_on_escape(const char* id)
{
	if (!id)
		return;
	auto& list = get_mock_state().close_on_escape;
	list.erase(std::remove_if(list.begin(), list.end(), [id](const auto& e) { return e.first == id; }), list.end());
}

// --- Updater ---

static void mock_request_update(signed int, const char*) {}

// --- MiniHook ---

static EMHStatus __stdcall mock_minhook_create(LPVOID, LPVOID, LPVOID*) { return MH_OK; }
static EMHStatus __stdcall mock_minhook_remove(LPVOID) { return MH_OK; }
static EMHStatus __stdcall mock_minhook_enable(LPVOID) { return MH_OK; }
static EMHStatus __stdcall mock_minhook_disable(LPVOID) { return MH_OK; }

// --- GameBinds ---

static void mock_gamebinds_press_async(EGameBinds) {}
static void mock_gamebinds_release_async(EGameBinds) {}
static void mock_gamebinds_invoke_async(EGameBinds, int) {}
static void mock_gamebinds_press(EGameBinds) {}
static void mock_gamebinds_release(EGameBinds) {}
static bool mock_gamebinds_is_bound(EGameBinds) { return false; }

// --- Localization ---

static const char* mock_localization_translate(const char* id) { return id; }
static const char* mock_localization_translate_to(const char* id, const char*) { return id; }
static void mock_localization_set(const char*, const char*, const char*) {}

// --- Fonts ---

static void mock_fonts_get(const char* id, FONTS_RECEIVECALLBACK callback)
{
	if (!callback)
		return;
	ImFont* font = static_cast<ImFont*>(get_mock_state().nexus_link_data.Font);
	callback(id, font);
}

static void mock_fonts_release(const char*, FONTS_RECEIVECALLBACK) {}
static void mock_fonts_add_from_file(const char*, float, const char*, FONTS_RECEIVECALLBACK, void*) {}
static void mock_fonts_add_from_resource(const char*, float, unsigned, HMODULE, FONTS_RECEIVECALLBACK, void*) {}
static void mock_fonts_add_from_memory(const char*, float, void*, size_t, FONTS_RECEIVECALLBACK, void*) {}
static void mock_fonts_resize(const char*, float) {}

// --- Assemble the mock API ---

AddonAPI* create_mock_api()
{
	static AddonAPI api = {};

	api.SwapChain = nullptr;
	api.Renderer.Register = mock_renderer_register;
	api.Renderer.Deregister = mock_renderer_deregister;
	api.RequestUpdate = mock_request_update;
	api.Log = mock_log;
	api.UI.SendAlert = mock_alerts_notify;
	api.UI.RegisterCloseOnEscape = mock_register_close_on_escape;
	api.UI.DeregisterCloseOnEscape = mock_deregister_close_on_escape;
	api.Paths.GetGameDirectory = mock_get_game_directory;
	api.Paths.GetAddonDirectory = mock_get_addon_directory;
	api.Paths.GetCommonDirectory = mock_get_common_directory;
	api.MinHook.Create = mock_minhook_create;
	api.MinHook.Remove = mock_minhook_remove;
	api.MinHook.Enable = mock_minhook_enable;
	api.MinHook.Disable = mock_minhook_disable;
	api.Events.Raise = mock_events_raise;
	api.Events.RaiseNotification = mock_events_raise_notification;
	api.Events.RaiseTargeted = mock_events_raise_targeted;
	api.Events.RaiseNotificationTargeted = mock_events_raise_notification_targeted;
	api.Events.Subscribe = mock_events_subscribe;
	api.Events.Unsubscribe = mock_events_unsubscribe;
	api.WndProc.Register = mock_wndproc_register;
	api.WndProc.Deregister = mock_wndproc_deregister;
	api.WndProc.SendToGameOnly = mock_wndproc_send_to_game;
	api.InputBinds.Invoke = mock_keybinds_invoke;
	api.InputBinds.RegisterWithString = mock_keybinds_register_string;
	api.InputBinds.RegisterWithStruct = mock_keybinds_register_struct;
	api.InputBinds.Deregister = mock_keybinds_deregister;
	api.GameBinds.PressAsync = mock_gamebinds_press_async;
	api.GameBinds.ReleaseAsync = mock_gamebinds_release_async;
	api.GameBinds.InvokeAsync = mock_gamebinds_invoke_async;
	api.GameBinds.Press = mock_gamebinds_press;
	api.GameBinds.Release = mock_gamebinds_release;
	api.GameBinds.IsBound = mock_gamebinds_is_bound;
	api.DataLink.Get = mock_datalink_get;
	api.DataLink.Share = mock_datalink_share;
	api.Textures.Get = mock_textures_get;
	api.Textures.GetOrCreateFromFile = mock_textures_get_or_create_file;
	api.Textures.GetOrCreateFromResource = mock_textures_get_or_create_resource;
	api.Textures.GetOrCreateFromURL = mock_textures_get_or_create_url;
	api.Textures.GetOrCreateFromMemory = mock_textures_get_or_create_memory;
	api.Textures.LoadFromFile = mock_textures_load_from_file;
	api.Textures.LoadFromResource = mock_textures_load_from_resource;
	api.Textures.LoadFromURL = mock_textures_load_from_url;
	api.Textures.LoadFromMemory = mock_textures_load_from_memory;
	api.QuickAccess.Add = mock_quickaccess_add;
	api.QuickAccess.Remove = mock_quickaccess_remove;
	api.QuickAccess.Notify = mock_quickaccess_notify;
	api.QuickAccess.AddContextMenu = mock_quickaccess_add_context_menu;
	api.QuickAccess.RemoveContextMenu = mock_quickaccess_remove_context_menu;
	api.Localization.Translate = mock_localization_translate;
	api.Localization.TranslateTo = mock_localization_translate_to;
	api.Localization.Set = mock_localization_set;
	api.Fonts.Get = mock_fonts_get;
	api.Fonts.Release = mock_fonts_release;
	api.Fonts.AddFromFile = mock_fonts_add_from_file;
	api.Fonts.AddFromResource = mock_fonts_add_from_resource;
	api.Fonts.AddFromMemory = mock_fonts_add_from_memory;
	api.Fonts.Resize = mock_fonts_resize;

	return &api;
}
