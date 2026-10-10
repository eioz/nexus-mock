#include "mock_api.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <psapi.h>
#include <string_view>
#include <thread>
#include <tuple>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

static MockState s_state;

MockState& get_mock_state() { return s_state; }

static std::vector<unsigned char> read_file(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), {});
}

// --- Workers ---

// Runs posted jobs one after another on its own thread.
class Worker
{
public:
	Worker() : thread([this] { run(); }) {}

	// drops the jobs that did not start yet
	~Worker()
	{
		{
			std::lock_guard lock(mutex);
			jobs.clear();
			stopping = true;
		}
		changed.notify_all();
		thread.join();
	}

	void post(std::function<void()> job)
	{
		{
			std::lock_guard lock(mutex);
			jobs.push_back(std::move(job));
		}
		changed.notify_all();
	}

	// returns once every posted job ran
	void wait()
	{
		std::unique_lock lock(mutex);
		changed.wait(lock, [this] { return jobs.empty() && !busy; });
	}

private:
	void run()
	{
		std::unique_lock lock(mutex);
		while (true)
		{
			changed.wait(lock, [this] { return stopping || !jobs.empty(); });
			if (stopping)
				return;
			auto job = std::move(jobs.front());
			jobs.pop_front();
			busy = true;
			lock.unlock();
			job();
			lock.lock();
			busy = false;
			changed.notify_all();
		}
	}

	std::mutex mutex;
	std::condition_variable changed;
	std::deque<std::function<void()>> jobs;
	bool busy = false;
	bool stopping = false;
	std::thread thread; // last, so it starts after the members it uses
};

// Nexus calls input bind handlers off the render thread and downloads textures in the background
static std::unique_ptr<Worker> s_bind_worker;
static std::unique_ptr<Worker> s_download_worker;

// --- Addon ---

// the addon's image, which tells its callbacks apart from the mock's own like Nexus does
static struct
{
	uintptr_t begin = 0;
	uintptr_t end = 0;
	uint32_t signature = 0;
} s_addon;

template <typename T> static bool inside_addon(T pointer)
{
	const auto address = reinterpret_cast<uintptr_t>(pointer);
	return address >= s_addon.begin && address < s_addon.end;
}

void set_addon_module(HMODULE module, uint32_t signature)
{
	MODULEINFO info{};
	GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
	s_addon.begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
	s_addon.end = s_addon.begin + info.SizeOfImage;
	s_addon.signature = signature;
}

// --- Renderer ---

static void mock_gui_register(ERenderType type, GUI_RENDER callback)
{
	if (!callback)
		return;
	std::lock_guard lock(s_state.mutex);
	switch (type)
	{
	case RT_PreRender:
		s_state.pre_render.push_back(callback);
		break;
	case RT_Render:
		s_state.render.push_back(callback);
		break;
	case RT_PostRender:
		s_state.post_render.push_back(callback);
		break;
	case RT_OptionsRender:
		s_state.options_render.push_back(callback);
		break;
	}
}

static void mock_gui_deregister(GUI_RENDER callback)
{
	std::lock_guard lock(s_state.mutex);
	for (auto* callbacks : { &s_state.pre_render, &s_state.render, &s_state.post_render, &s_state.options_render })
		std::erase(*callbacks, callback);
}

// --- Logging ---

static void mock_log(ELogLevel level, const char* channel, const char* message)
{
	static constexpr const char* Tags[] = { "????", "CRIT", "WARN", "INFO", "DBG ", "TRAC" };
	const auto tag = level >= LOGL_CRITICAL && level <= LOGL_TRACE ? Tags[level] : Tags[0];
	printf("[%s] [%s] %s\n", tag, channel ? channel : "", message ? message : "");
}

// --- UI ---

// Nexus queues alerts and drops a repeat of the one it shows
static void mock_gui_send_alert(const char* message)
{
	if (!message)
		return;
	std::lock_guard lock(s_state.mutex);
	if (!s_state.alerts.empty() && s_state.alerts.front().message == message)
		return;
	s_state.alerts.push_back({ message, {} });
}

static void mock_gui_register_close_on_escape(const char* name, bool* visible)
{
	if (!name || !visible)
		return;
	std::lock_guard lock(s_state.mutex);
	auto& windows = s_state.close_on_escape;
	if (auto found = std::ranges::find(windows, std::string_view(name), &std::pair<std::string, bool*>::first); found != windows.end())
		found->second = visible;
	else
		windows.emplace_back(name, visible);
}

static void mock_gui_deregister_close_on_escape(const char* name)
{
	if (!name)
		return;
	std::lock_guard lock(s_state.mutex);
	std::erase_if(s_state.close_on_escape, [name](const auto& window) { return window.first == name; });
}

// Nexus closes the frontmost open window registered under its ImGui name, one per Escape press.
static bool close_on_escape(UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg != WM_KEYDOWN || wParam != VK_ESCAPE || (lParam & (1 << 30)) || !ImGui::GetCurrentContext())
		return false;
	std::lock_guard lock(s_state.mutex);
	const auto& windows = ImGui::GetCurrentContext()->Windows;
	for (int i = windows.Size - 1; i > 0; --i)
		for (auto& [name, visible] : s_state.close_on_escape)
			if (*visible && name == windows[i]->Name)
			{
				*visible = false;
				return true;
			}
	return false;
}

// --- Paths ---

// "<game>", "<game>\addons", "<game>\addons\<name>" and "<game>\addons\common" like Nexus, which creates none of them here
static std::filesystem::path s_game_directory;

static const char* mock_paths_get_game_directory()
{
	static const std::string path = s_game_directory.string();
	return path.c_str();
}

static const char* mock_paths_get_addon_directory(const char* name)
{
	static const std::string addons = (s_game_directory / "addons").string();
	if (!name || !*name)
		return addons.c_str();
	// the same name keeps its pointer
	static std::mutex mutex;
	static std::map<std::string, std::string> directories;
	std::lock_guard lock(mutex);
	auto& path = directories[name];
	if (path.empty())
		path = (s_game_directory / "addons" / name).string();
	return path.c_str();
}

static const char* mock_paths_get_common_directory()
{
	static const std::string path = (s_game_directory / "addons" / "common").string();
	return path.c_str();
}

// --- MinHook ---

// There is no game to hook, so hooks are accepted but never enabled, and the original is the target itself.
static EMHStatus __stdcall mock_minhook_create(LPVOID target, LPVOID, LPVOID* original)
{
	if (original)
		*original = target;
	return MH_OK;
}

static EMHStatus __stdcall mock_minhook_remove(LPVOID) { return MH_OK; }
static EMHStatus __stdcall mock_minhook_enable(LPVOID) { return MH_OK; }
static EMHStatus __stdcall mock_minhook_disable(LPVOID) { return MH_OK; }

// --- Events ---

// Nexus raises events on the caller's thread, to every subscription in order
void raise_event(const char* id, void* data)
{
	if (!id)
		return;
	std::vector<EVENT_CONSUME> subscriptions;
	{
		std::lock_guard lock(s_state.mutex);
		if (const auto found = s_state.events.find(id); found != s_state.events.end())
			subscriptions = found->second;
	}
	for (auto callback : subscriptions)
		callback(data);
}

static void mock_events_raise_notification(const char* id) { raise_event(id, nullptr); }

// only the first subscription of the addon with the signature, like in Nexus
static void mock_events_raise_targeted(uint32_t signature, const char* id, void* data)
{
	if (!id)
		return;
	EVENT_CONSUME target = nullptr;
	{
		std::lock_guard lock(s_state.mutex);
		if (const auto found = s_state.events.find(id); found != s_state.events.end())
			for (auto callback : found->second)
				if ((inside_addon(callback) ? s_addon.signature : 0) == signature)
				{
					target = callback;
					break;
				}
	}
	if (target)
		target(data);
}

static void mock_events_raise_notification_targeted(uint32_t signature, const char* id) { mock_events_raise_targeted(signature, id, nullptr); }

// Nexus keeps duplicate subscriptions
static void mock_events_subscribe(const char* id, EVENT_CONSUME callback)
{
	if (!id || !callback)
		return;
	std::lock_guard lock(s_state.mutex);
	s_state.events[id].push_back(callback);
}

static void mock_events_unsubscribe(const char* id, EVENT_CONSUME callback)
{
	if (!id || !callback)
		return;
	std::lock_guard lock(s_state.mutex);
	const auto found = s_state.events.find(id);
	if (found == s_state.events.end())
		return;
	std::erase(found->second, callback);
	if (found->second.empty())
		s_state.events.erase(found);
}

// --- WndProc ---

static void mock_wndproc_register(WNDPROC_CALLBACK callback)
{
	if (!callback)
		return;
	std::lock_guard lock(s_state.mutex);
	s_state.wndprocs.push_back(callback);
}

static void mock_wndproc_deregister(WNDPROC_CALLBACK callback)
{
	std::lock_guard lock(s_state.mutex);
	std::erase(s_state.wndprocs, callback);
}

// there is no game to send it to
static LRESULT mock_wndproc_send_to_game_only(HWND, UINT, WPARAM, LPARAM) { return 0; }

// Feeds ImGui like Nexus: mouse buttons and the wheel only over ImGui windows, keys and characters only while a text field
// is active. Returns true when ImGui took the message from the game.
static bool feed_imgui(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!ImGui::GetCurrentContext())
		return false;
	auto& io = ImGui::GetIO();
	switch (msg)
	{
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
	{
		const int button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ? 0 : 1;
		if (!io.WantCaptureMouse)
		{
			if (button == 0)
				ImGui::ClearActiveID();
			return false;
		}
		io.MouseDown[button] = true;
		// keeps the button up when a drag leaves the window
		if (!GetCapture())
			SetCapture(hwnd);
		return true;
	}
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
		io.MouseDown[msg == WM_LBUTTONUP ? 0 : 1] = false;
		if (GetCapture() == hwnd && !io.MouseDown[0] && !io.MouseDown[1])
			ReleaseCapture();
		return false;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		if (!io.WantCaptureMouse)
			return false;
		(msg == WM_MOUSEWHEEL ? io.MouseWheel : io.MouseWheelH) += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
		return true;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (!io.WantTextInput)
			return false;
		if (wParam < 256)
			io.KeysDown[wParam] = true;
		return true;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wParam < 256)
			io.KeysDown[wParam] = false;
		return false;
	case WM_CHAR:
		if (!io.WantTextInput)
			return false;
		if (wParam > 0 && wParam < 0x10000)
			io.AddInputCharacterUTF16(static_cast<unsigned short>(wParam));
		return true;
	}
	return false;
}

// --- Input binds ---

static constexpr const char* MouseButtons[] = { "LMB", "RMB", "MMB", "M4", "M5" };

static LPARAM key_lparam(unsigned short code) { return (static_cast<LPARAM>(code & 0xFF) << 16) | ((code & 0xE000) ? 1 << 24 : 0); }

// Windows' names for the keys of the active layout, which Nexus reads bind strings with. The first name wins.
static const std::map<std::string, unsigned short>& key_codes()
{
	static const auto codes = [] {
		std::map<std::string, unsigned short> codes;
		for (unsigned short code = 0; code < 256; ++code)
			for (unsigned short extended : { 0, 0xE000 })
			{
				char name[64];
				if (GetKeyNameTextA(static_cast<LONG>(key_lparam(code | extended)), name, sizeof(name)) > 0)
					codes.try_emplace(name, static_cast<unsigned short>(code | extended));
			}
		return codes;
	}();
	return codes;
}

// Parses "CTRL+SHIFT+I" like Nexus: ALT, CTRL and SHIFT before the last "+", then a key name or LMB, RMB, MMB, M4 or M5.
// The input is uppercased, so key names Windows writes in mixed case, like "Space", stay unbound.
static MockBind parse_bind(const char* text)
{
	MockBind bind;
	std::string upper = text ? text : "";
	std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	const auto last = upper.rfind('+');
	for (size_t start = 0; last != std::string::npos && start < last;)
	{
		const auto end = upper.find('+', start);
		const auto modifier = std::string_view(upper).substr(start, end - start);
		bind.alt |= modifier == "ALT";
		bind.ctrl |= modifier == "CTRL";
		bind.shift |= modifier == "SHIFT";
		start = end + 1;
	}
	const auto key = last == std::string::npos ? upper : upper.substr(last + 1);
	if (const auto* button = std::find(std::begin(MouseButtons), std::end(MouseButtons), key); button != std::end(MouseButtons))
	{
		bind.device = MockBind::Device::Mouse;
		bind.code = static_cast<unsigned short>(button - std::begin(MouseButtons) + 1);
	}
	else if (const auto found = key_codes().find(key); found != key_codes().end())
		bind.code = found->second;
	return bind;
}

std::string bind_text(const MockBind& bind)
{
	if (bind.code == 0)
		return "(null)";
	std::string text = std::string(bind.alt ? "ALT+" : "") + (bind.ctrl ? "CTRL+" : "") + (bind.shift ? "SHIFT+" : "");
	if (bind.device == MockBind::Device::Mouse)
		return text + (bind.code <= std::size(MouseButtons) ? MouseButtons[bind.code - 1] : "?");
	char name[64] = "?";
	GetKeyNameTextA(static_cast<LONG>(key_lparam(bind.code)), name, sizeof(name));
	return text + name;
}

// Nexus keeps the bind an identifier has, and leaves a new one unbound when another identifier uses the combination.
static void register_input_bind(const char* id, INPUTBINDS_PROCESS handler, MockBind bind)
{
	if (!id)
		return;
	{
		std::lock_guard lock(s_state.mutex);
		auto& binds = s_state.input_binds;
		if (auto found = std::ranges::find(binds, std::string_view(id), &MockInputBind::id); found != binds.end())
		{
			found->handler = handler;
			bind = found->bind;
		}
		else
		{
			if (bind.code != 0 && std::ranges::any_of(binds, [&](const auto& other) { return other.bind == bind; }))
				bind = {};
			binds.push_back({ id, handler, bind });
		}
	}
	printf("[MOCK] Input bind registered: %s: %s\n", id, bind_text(bind).c_str());
	s_bind_worker->post([] { raise_event("EV_INPUTBIND_UPDATED", nullptr); });
}

static void mock_input_binds_register_with_string(const char* id, INPUTBINDS_PROCESS handler, const char* bind) { register_input_bind(id, handler, parse_bind(bind)); }

// the key is a scan code
static void mock_input_binds_register_with_struct(const char* id, INPUTBINDS_PROCESS handler, Keybind_t bind) { register_input_bind(id, handler, { MockBind::Device::Keyboard, bind.Key, bind.Alt, bind.Ctrl, bind.Shift }); }

// Nexus keeps the bind and only drops the handler
static void mock_input_binds_deregister(const char* id)
{
	if (!id)
		return;
	std::lock_guard lock(s_state.mutex);
	if (auto found = std::ranges::find(s_state.input_binds, std::string_view(id), &MockInputBind::id); found != s_state.input_binds.end())
	{
		found->handler = nullptr;
		found->held = false;
	}
}

bool invoke_input_bind(const std::string& id, bool release)
{
	INPUTBINDS_PROCESS handler = nullptr;
	{
		std::lock_guard lock(s_state.mutex);
		if (const auto found = std::ranges::find(s_state.input_binds, id, &MockInputBind::id); found != s_state.input_binds.end())
			handler = found->handler;
	}
	if (!handler)
		return false;
	s_bind_worker->post([handler, id, release] { handler(id.c_str(), release); });
	return true;
}

static void mock_input_binds_invoke(const char* id, bool release)
{
	if (id)
		invoke_input_bind(id, release);
}

void wait_for_input_binds() { s_bind_worker->wait(); }

// Nexus's input bind step: the modifiers come from key messages, and a press only matches a bind with exactly its
// modifiers. Returns true when a key press invoked a handler, which keeps it from the game.
static bool process_input_binds(UINT msg, WPARAM wParam, LPARAM lParam)
{
	std::vector<std::pair<std::string, bool>> invoked; // identifier and release
	{
		std::lock_guard lock(s_state.mutex);
		auto press = [&](const MockBind& input) {
			for (auto& bind : s_state.input_binds)
				if (bind.handler && bind.bind == input)
				{
					bind.held = true;
					invoked.emplace_back(bind.id, false);
				}
		};
		auto release = [&](auto matches) {
			for (auto& bind : s_state.input_binds)
				if (bind.held && matches(bind.bind))
				{
					bind.held = false;
					if (bind.handler)
						invoked.emplace_back(bind.id, true);
				}
		};

		const bool key_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
		const bool key_up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
		const unsigned short button = msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ? 3 : GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 4 : 5;
		if (key_down || key_up)
		{
			bool* modifier = wParam == VK_MENU ? &s_state.alt_down : wParam == VK_CONTROL ? &s_state.ctrl_down : wParam == VK_SHIFT ? &s_state.shift_down : nullptr;
			if (modifier)
				*modifier = key_down;
			const auto code = static_cast<unsigned short>(((lParam >> 16) & 0xFF) | ((lParam & (1 << 24)) ? 0xE000 : 0));
			if (key_down && !(lParam & (1 << 30)) && code != 0)
			{
				// a modifier pressed on its own is a key without modifiers
				MockBind input{ MockBind::Device::Keyboard, code, s_state.alt_down, s_state.ctrl_down, s_state.shift_down };
				if (modifier)
					input.alt = input.ctrl = input.shift = false;
				press(input);
			}
			else if (key_up)
				release([&](const MockBind& bind) { return (bind.device == MockBind::Device::Keyboard && bind.code == code) || (wParam == VK_MENU && bind.alt) || (wParam == VK_CONTROL && bind.ctrl) || (wParam == VK_SHIFT && bind.shift); });
		}
		else if (msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
			press({ MockBind::Device::Mouse, button, s_state.alt_down, s_state.ctrl_down, s_state.shift_down });
		else if (msg == WM_MBUTTONUP || msg == WM_XBUTTONUP)
			release([&](const MockBind& bind) { return bind.device == MockBind::Device::Mouse && bind.code == button; });
		else if (msg == WM_ACTIVATEAPP)
		{
			s_state.alt_down = s_state.ctrl_down = s_state.shift_down = false;
			release([](const MockBind&) { return true; });
		}
	}
	bool pressed = false;
	for (const auto& [id, release] : invoked)
	{
		pressed |= !release;
		invoke_input_bind(id, release);
	}
	return pressed && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
}

bool process_window_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// the window also gets messages before create_mock_api and after shutdown_mock_api
	if (!s_bind_worker)
		return false;
	std::vector<WNDPROC_CALLBACK> callbacks;
	{
		std::lock_guard lock(s_state.mutex);
		callbacks = s_state.wndprocs;
	}
	// an addon returning 0 handled the message
	for (auto callback : callbacks)
		if (callback(hwnd, msg, wParam, lParam) == 0)
			return true;
	return feed_imgui(hwnd, msg, wParam, lParam) || close_on_escape(msg, wParam, lParam) || process_input_binds(msg, wParam, lParam);
}

// --- GameBinds ---

// there is no game to send them to
static void mock_gamebinds_press_async(EGameBinds) {}
static void mock_gamebinds_release_async(EGameBinds) {}
static void mock_gamebinds_invoke_async(EGameBinds, int32_t) {}
static void mock_gamebinds_press(EGameBinds) {}
static void mock_gamebinds_release(EGameBinds) {}
static bool mock_gamebinds_is_bound(EGameBinds) { return false; }

// --- DataLink ---

// A block keeps its address, and sharing an identifier again only works with the same size, like in Nexus.
struct SharedBlock
{
	void* data = nullptr;
	size_t size = 0;
	std::unique_ptr<unsigned char[]> owned;
};

static std::map<std::string, SharedBlock> s_shared_blocks;

static void* mock_datalink_get(const char* id)
{
	if (!id)
		return nullptr;
	std::lock_guard lock(s_state.mutex);
	const auto found = s_shared_blocks.find(id);
	return found != s_shared_blocks.end() ? found->second.data : nullptr;
}

static void* mock_datalink_share(const char* id, uint64_t size)
{
	if (!id || size == 0)
		return nullptr;
	std::lock_guard lock(s_state.mutex);
	auto [found, added] = s_shared_blocks.try_emplace(id);
	auto& block = found->second;
	if (added)
	{
		block.owned = std::make_unique<unsigned char[]>(size);
		block.data = block.owned.get();
		block.size = size;
	}
	else if (block.size != size)
	{
		printf("[MOCK] WARN: DataLink %s is shared with %zu bytes, not %llu.\n", id, block.size, size);
		return nullptr;
	}
	return block.data;
}

// --- Textures ---

// Like Nexus, images from files, resources and memory decode in the call and URLs on a worker. The next frame creates
// the textures and calls back on the render thread. Loading an identifier again moves the old texture to "<id>_<n>",
// and a failed load is tried again by the next GetOrCreate call.
struct QueuedTexture
{
	TEXTURES_RECEIVECALLBACK callback = nullptr;
	bool done = false;
	int width = 0;
	int height = 0;
	std::vector<unsigned char> pixels; // RGBA, empty when the load failed
};

static struct
{
	std::mutex mutex;
	std::map<std::string, Texture_t*> loaded;
	std::map<std::string, QueuedTexture> queued;
	ID3D11Device* device = nullptr;
	HINTERNET internet = nullptr;
} s_textures;

static Texture_t* create_texture(const unsigned char* pixels, int width, int height)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = static_cast<UINT>(width);
	desc.Height = static_cast<UINT>(height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA data = { pixels, static_cast<UINT>(width * 4), 0 };

	ID3D11Texture2D* texture = nullptr;
	if (FAILED(s_textures.device->CreateTexture2D(&desc, &data, &texture)))
		return nullptr;
	ID3D11ShaderResourceView* view = nullptr;
	s_textures.device->CreateShaderResourceView(texture, nullptr, &view);
	texture->Release();
	return view ? new Texture_t{ static_cast<uint32_t>(width), static_cast<uint32_t>(height), view } : nullptr;
}

Texture_t* get_texture(const std::string& id)
{
	std::lock_guard lock(s_textures.mutex);
	const auto found = s_textures.loaded.find(id);
	return found != s_textures.loaded.end() ? found->second : nullptr;
}

static Texture_t* mock_textures_get(const char* id) { return id ? get_texture(id) : nullptr; }

// Returns false when the identifier is queued already, which drops the request like in Nexus.
static bool queue_texture(const char* id, TEXTURES_RECEIVECALLBACK callback)
{
	if (!id)
		return false;
	std::lock_guard lock(s_textures.mutex);
	if (s_textures.queued.contains(id))
		return false;
	if (const auto found = s_textures.loaded.find(id); found != s_textures.loaded.end())
	{
		int number = 1;
		while (s_textures.loaded.contains(std::string(id) + "_" + std::to_string(number)))
			++number;
		s_textures.loaded.emplace(std::string(id) + "_" + std::to_string(number), found->second);
		s_textures.loaded.erase(found);
	}
	s_textures.queued[id].callback = callback;
	return true;
}

static void decode_texture(const std::string& id, const unsigned char* data, size_t size)
{
	int width = 0;
	int height = 0;
	unsigned char* pixels = data && size ? stbi_load_from_memory(data, static_cast<int>(size), &width, &height, nullptr, 4) : nullptr;
	if (data && size && !pixels)
		printf("[MOCK] WARN: Texture %s is no image stb_image can decode.\n", id.c_str());
	std::lock_guard lock(s_textures.mutex);
	if (const auto found = s_textures.queued.find(id); found != s_textures.queued.end())
	{
		auto& texture = found->second;
		texture.done = true;
		if (pixels)
		{
			texture.width = width;
			texture.height = height;
			texture.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
		}
	}
	stbi_image_free(pixels);
}

static void mock_textures_load_from_file(const char* id, const char* filename, TEXTURES_RECEIVECALLBACK callback)
{
	if (!queue_texture(id, callback))
		return;
	const auto data = filename ? read_file(filename) : std::vector<unsigned char>();
	if (data.empty())
		printf("[MOCK] WARN: Texture %s: the file does not exist: %s\n", id, filename ? filename : "(null)");
	decode_texture(id, data.data(), data.size());
}

static void mock_textures_load_from_resource(const char* id, uint32_t resource_id, HMODULE module, TEXTURES_RECEIVECALLBACK callback)
{
	if (!queue_texture(id, callback))
		return;
	// Nexus only finds resources of the type "PNG"
	const unsigned char* data = nullptr;
	DWORD size = 0;
	if (HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(resource_id), "PNG"))
		if (HGLOBAL handle = LoadResource(module, resource))
		{
			data = static_cast<const unsigned char*>(LockResource(handle));
			size = SizeofResource(module, resource);
		}
	if (!data)
		printf("[MOCK] WARN: Texture %s: there is no PNG resource %u.\n", id, resource_id);
	decode_texture(id, data, size);
}

static void mock_textures_load_from_memory(const char* id, void* data, uint64_t size, TEXTURES_RECEIVECALLBACK callback)
{
	if (queue_texture(id, callback))
		decode_texture(id, static_cast<const unsigned char*>(data), size);
}

static std::vector<unsigned char> download(const std::string& url)
{
	std::vector<unsigned char> data;
	HINTERNET request = InternetOpenUrlA(s_textures.internet, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
	if (!request)
	{
		printf("[MOCK] WARN: Cannot download %s (error %lu).\n", url.c_str(), GetLastError());
		return data;
	}
	// Nexus only accepts a 200 response
	DWORD status = 0;
	DWORD length = sizeof(status);
	if (!HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &length, nullptr) || status != 200)
		printf("[MOCK] WARN: %s returned HTTP %lu.\n", url.c_str(), status);
	else
	{
		char buffer[8192];
		DWORD read = 0;
		while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0)
			data.insert(data.end(), buffer, buffer + read);
	}
	InternetCloseHandle(request);
	return data;
}

static void mock_textures_load_from_url(const char* id, const char* remote, const char* endpoint, TEXTURES_RECEIVECALLBACK callback)
{
	if (!queue_texture(id, callback))
		return;
	// Nexus joins them as they are
	const auto url = std::string(remote ? remote : "") + (endpoint ? endpoint : "");
	s_download_worker->post([id = std::string(id), url] {
		const auto data = download(url);
		decode_texture(id, data.data(), data.size());
	});
}

static Texture_t* mock_textures_get_or_create_from_file(const char* id, const char* filename)
{
	if (auto* texture = mock_textures_get(id))
		return texture;
	mock_textures_load_from_file(id, filename, nullptr);
	return nullptr;
}

static Texture_t* mock_textures_get_or_create_from_resource(const char* id, uint32_t resource_id, HMODULE module)
{
	if (auto* texture = mock_textures_get(id))
		return texture;
	mock_textures_load_from_resource(id, resource_id, module, nullptr);
	return nullptr;
}

static Texture_t* mock_textures_get_or_create_from_url(const char* id, const char* remote, const char* endpoint)
{
	if (auto* texture = mock_textures_get(id))
		return texture;
	mock_textures_load_from_url(id, remote, endpoint, nullptr);
	return nullptr;
}

static Texture_t* mock_textures_get_or_create_from_memory(const char* id, void* data, uint64_t size)
{
	if (auto* texture = mock_textures_get(id))
		return texture;
	mock_textures_load_from_memory(id, data, size, nullptr);
	return nullptr;
}

// creates the decoded textures and calls back, at the start of the frame like Nexus
static void advance_textures()
{
	std::vector<std::tuple<std::string, Texture_t*, TEXTURES_RECEIVECALLBACK>> finished;
	{
		std::lock_guard lock(s_textures.mutex);
		for (auto it = s_textures.queued.begin(); it != s_textures.queued.end();)
		{
			auto& [id, queued] = *it;
			if (!queued.done)
			{
				++it;
				continue;
			}
			Texture_t* texture = queued.pixels.empty() ? nullptr : create_texture(queued.pixels.data(), queued.width, queued.height);
			if (texture)
			{
				s_textures.loaded[id] = texture;
				printf("[MOCK] Texture loaded: %s (%ux%u)\n", id.c_str(), texture->Width, texture->Height);
			}
			finished.emplace_back(id, texture, queued.callback);
			it = s_textures.queued.erase(it);
		}
	}
	for (const auto& [id, texture, callback] : finished)
		if (callback)
			callback(id.c_str(), texture);
}

bool preload_texture(const std::string& id, const std::filesystem::path& file)
{
	{
		std::lock_guard lock(s_textures.mutex);
		if (s_textures.loaded.contains(id) || s_textures.queued.contains(id))
		{
			printf("[MOCK] ERROR: The texture %s is loaded or loading already, so the preload would not replace it.\n", id.c_str());
			return false;
		}
	}
	const auto data = read_file(file);
	int width = 0;
	int height = 0;
	unsigned char* pixels = data.empty() ? nullptr : stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, nullptr, 4);
	Texture_t* texture = pixels ? create_texture(pixels, width, height) : nullptr;
	stbi_image_free(pixels);
	if (!texture)
	{
		printf("[MOCK] ERROR: Cannot load the texture %s from %s\n", id.c_str(), reinterpret_cast<const char*>(file.u8string().c_str()));
		return false;
	}
	std::lock_guard lock(s_textures.mutex);
	s_textures.loaded.emplace(id, texture);
	return true;
}

std::vector<std::pair<std::string, void*>> texture_resources()
{
	std::lock_guard lock(s_textures.mutex);
	std::vector<std::pair<std::string, void*>> resources;
	for (const auto& [id, texture] : s_textures.loaded)
		resources.emplace_back(id, texture->Resource);
	return resources;
}

static void release_textures()
{
	std::lock_guard lock(s_textures.mutex);
	for (const auto& [id, texture] : s_textures.loaded)
	{
		static_cast<ID3D11ShaderResourceView*>(texture->Resource)->Release();
		delete texture;
	}
	s_textures.loaded.clear();
	s_textures.queued.clear();
}

// --- Quick access ---

// The first registration of an identifier wins, like in Nexus.
static void mock_quickaccess_add(const char* id, const char* texture, const char* hover_texture, const char* input_bind, const char* tooltip)
{
	if (!id || !texture || !hover_texture)
		return;
	std::lock_guard lock(s_state.mutex);
	auto [found, added] = s_state.shortcuts.try_emplace(id);
	if (!added)
		return;
	auto& shortcut = found->second;
	shortcut.texture = texture;
	shortcut.hover_texture = hover_texture;
	shortcut.input_bind = input_bind ? input_bind : "";
	shortcut.tooltip = tooltip ? tooltip : "";
}

static void mock_quickaccess_remove(const char* id)
{
	if (!id)
		return;
	std::lock_guard lock(s_state.mutex);
	s_state.shortcuts.erase(id);
}

static void mock_quickaccess_notify(const char* id)
{
	if (!id)
		return;
	std::lock_guard lock(s_state.mutex);
	if (const auto found = s_state.shortcuts.find(id); found != s_state.shortcuts.end())
		found->second.notified = true;
}

// Without a target the item goes to the Nexus shortcut. Nexus never shows an item whose shortcut does not exist yet.
static void mock_quickaccess_add_context_menu(const char* id, const char* target, GUI_RENDER callback)
{
	if (!id)
		return;
	const std::string shortcut = target ? target : NexusShortcut;
	std::lock_guard lock(s_state.mutex);
	const auto found = s_state.shortcuts.find(shortcut);
	if (found == s_state.shortcuts.end())
	{
		printf("[MOCK] WARN: There is no quick access shortcut %s, so Nexus never shows the context menu item %s.\n", shortcut.c_str(), id);
		return;
	}
	found->second.context_menu[id] = callback;
}

static void mock_quickaccess_remove_context_menu(const char* id)
{
	if (!id)
		return;
	std::lock_guard lock(s_state.mutex);
	for (auto& [shortcut_id, shortcut] : s_state.shortcuts)
		shortcut.context_menu.erase(id);
}

// --- Localization ---

// Nexus's languages. Set queues strings for the next frame, and drops strings for other languages.
static constexpr std::string_view Languages[] = { "en", "de", "fr", "es", "pt-br", "cz", "it", "pl", "ru", "cn" };

static struct
{
	std::mutex mutex;
	std::map<std::string, std::map<std::string, std::string>, std::less<>> strings; // by language, then identifier
	std::vector<std::tuple<std::string, std::string, std::string>> queued;          // identifier, language and text
	std::string language = "en";
} s_locale;

// the given language, then the active one, then English, then the identifier itself
static const char* mock_localization_translate_to(const char* id, const char* language)
{
	if (!id)
		return id;
	std::lock_guard lock(s_locale.mutex);
	auto lookup = [id](std::string_view name) -> const char* {
		const auto strings = s_locale.strings.find(name);
		if (strings == s_locale.strings.end())
			return nullptr;
		const auto text = strings->second.find(id);
		return text != strings->second.end() ? text->second.c_str() : nullptr;
	};
	for (const std::string_view name : { std::string_view(language ? language : ""), std::string_view(s_locale.language), std::string_view("en") })
		if (const char* text = lookup(name))
			return text;
	return id;
}

static const char* mock_localization_translate(const char* id) { return mock_localization_translate_to(id, nullptr); }

static void mock_localization_set(const char* id, const char* language, const char* text)
{
	if (!id || !language || !text)
		return;
	std::lock_guard lock(s_locale.mutex);
	s_locale.queued.emplace_back(id, language, text);
}

static void advance_localization()
{
	std::lock_guard lock(s_locale.mutex);
	for (auto& [id, language, text] : s_locale.queued)
		if (std::ranges::find(Languages, language) != std::end(Languages))
			s_locale.strings[language][id] = std::move(text);
	s_locale.queued.clear();
}

// --- Fonts ---

// Nexus's font manager. A font keeps its receivers, and every atlas rebuild calls them with nullptr and then with the new
// font. Rebuilds happen before the next frame. Sizes are unscaled, since Nexus scales with io.FontGlobalScale.
struct MockFont
{
	std::string id;
	float size = 0.f;
	std::vector<unsigned char> data; // empty for ImGui's default font
	ImFontConfig config;
	ImFont* pointer = nullptr;
	std::vector<FONTS_RECEIVECALLBACK> receivers;
	bool nexus = false; // one of the fonts Nexus provides, which stay
};

static struct
{
	std::mutex mutex;
	std::vector<MockFont> fonts; // in registration order, which merged fonts rely on
	bool rebuild = true;
} s_fonts;

// Latin with the extensions A and B, which Nexus builds every font with
static const ImWchar GlyphRanges[] = { 0x0020, 0x024F, 0 };

// The formats stb_truetype reads. ImGui asserts on anything else.
static bool is_font(const std::vector<unsigned char>& data)
{
	if (data.size() < 12)
		return false;
	const std::string_view tag(reinterpret_cast<const char*>(data.data()), 4);
	using namespace std::string_view_literals;
	return tag == "1\0\0\0"sv || tag == "\0\1\0\0"sv || tag == "true" || tag == "typ1" || tag == "OTTO" || tag == "ttcf";
}

static decltype(s_fonts.fonts)::iterator find_font(std::string_view id) { return std::ranges::find(s_fonts.fonts, id, &MockFont::id); }

// Registers a font, or only adds the receiver when the identifier exists, like Nexus.
static void add_font(const char* id, float size, std::vector<unsigned char> data, FONTS_RECEIVECALLBACK callback, void* config)
{
	if (!id || size < 1.f)
		return;
	if (!is_font(data))
	{
		printf("[MOCK] WARN: Font %s has no TrueType or OpenType data.\n", id);
		return;
	}
	std::lock_guard lock(s_fonts.mutex);
	if (const auto found = find_font(id); found != s_fonts.fonts.end())
	{
		if (callback)
			found->receivers.push_back(callback);
		return;
	}
	auto& font = s_fonts.fonts.emplace_back();
	font.id = id;
	font.size = size;
	font.data = std::move(data);
	// the addon's ImGui 1.80 config
	if (config)
		font.config = *static_cast<const ImFontConfig*>(config);
	if (callback)
		font.receivers.push_back(callback);
	s_fonts.rebuild = true;
}

static void mock_fonts_get(const char* id, FONTS_RECEIVECALLBACK callback)
{
	if (!id || !callback)
		return;
	ImFont* pointer = nullptr;
	{
		std::lock_guard lock(s_fonts.mutex);
		if (const auto found = find_font(id); found != s_fonts.fonts.end())
		{
			found->receivers.push_back(callback);
			pointer = found->pointer;
		}
	}
	callback(id, pointer);
}

// a font without receivers is dropped
static void mock_fonts_release(const char* id, FONTS_RECEIVECALLBACK callback)
{
	if (!id)
		return;
	std::lock_guard lock(s_fonts.mutex);
	const auto found = find_font(id);
	if (found == s_fonts.fonts.end())
		return;
	std::erase(found->receivers, callback);
	if (found->receivers.empty() && !found->nexus)
	{
		s_fonts.fonts.erase(found);
		s_fonts.rebuild = true;
	}
}

static void mock_fonts_add_from_file(const char* id, float size, const char* filename, FONTS_RECEIVECALLBACK callback, void* config) { add_font(id, size, filename ? read_file(filename) : std::vector<unsigned char>(), callback, config); }

// Nexus only finds resources of the type RT_FONT
static void mock_fonts_add_from_resource(const char* id, float size, uint32_t resource_id, HMODULE module, FONTS_RECEIVECALLBACK callback, void* config)
{
	std::vector<unsigned char> data;
	if (HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_FONT))
		if (HGLOBAL handle = LoadResource(module, resource))
			if (const auto* bytes = static_cast<const unsigned char*>(LockResource(handle)))
				data.assign(bytes, bytes + SizeofResource(module, resource));
	add_font(id, size, std::move(data), callback, config);
}

static void mock_fonts_add_from_memory(const char* id, float size, void* data, uint64_t length, FONTS_RECEIVECALLBACK callback, void* config)
{
	const auto* bytes = static_cast<const unsigned char*>(data);
	add_font(id, size, bytes ? std::vector<unsigned char>(bytes, bytes + length) : std::vector<unsigned char>(), callback, config);
}

static void mock_fonts_resize(const char* id, float size)
{
	if (!id || size < 1.f)
		return;
	std::lock_guard lock(s_fonts.mutex);
	if (const auto found = find_font(id); found != s_fonts.fonts.end() && found->size != size)
	{
		found->size = size;
		s_fonts.rebuild = true;
	}
}

bool add_nexus_fonts(const std::string& body_file, const std::string& heading_file)
{
	auto load = [](const std::string& file, std::vector<unsigned char>& data) {
		if (file.empty())
			return true;
		data = read_file(file);
		if (is_font(data))
			return true;
		printf("[MOCK] ERROR: %s is no TrueType or OpenType font.\n", file.c_str());
		return false;
	};
	std::vector<unsigned char> body;
	std::vector<unsigned char> heading;
	if (!load(body_file, body) || !load(heading_file, heading))
		return false;
	if (heading.empty())
		heading = body;

	// Nexus draws FontUI with Trebuchet MS, which comes with Windows
	wchar_t windows[MAX_PATH] = {};
	GetWindowsDirectoryW(windows, MAX_PATH);
	auto ui = read_file(std::filesystem::path(windows) / "Fonts" / "trebuc.ttf");
	const bool trebuchet = is_font(ui);
	if (!trebuchet)
		ui = body;

	struct NexusFont
	{
		const char* id;
		float size;
		const std::vector<unsigned char>& data;
	};
	const NexusFont fonts[] = {
		{ "FONT_DEFAULT", 15.f, body },
		{ "MENOMONIA_S", 16.f, heading },
		{ "MENOMONIA_BIG_S", 22.f, heading },
		{ "TREBUCHET_S", 15.f, ui },
		{ "MENOMONIA_N", 18.f, heading },
		{ "MENOMONIA_BIG_N", 24.f, heading },
		{ "TREBUCHET_N", 16.f, ui },
		{ "MENOMONIA_L", 20.f, heading },
		{ "MENOMONIA_BIG_L", 26.f, heading },
		{ "TREBUCHET_L", 17.5f, ui },
		{ "MENOMONIA_XL", 22.f, heading },
		{ "MENOMONIA_BIG_XL", 28.f, heading },
		{ "TREBUCHET_XL", 19.5f, ui },
	};
	std::lock_guard lock(s_fonts.mutex);
	for (const auto& nexus_font : fonts)
	{
		auto& font = s_fonts.fonts.emplace_back();
		font.id = nexus_font.id;
		font.size = nexus_font.size;
		font.data = nexus_font.data;
		font.nexus = true;
		if (font.data.empty())
		{
			// ImGui's default font is a pixel font
			font.config.OversampleH = 1;
			font.config.OversampleV = 1;
			font.config.PixelSnapH = true;
		}
	}
	s_fonts.rebuild = true;
	auto name = [](const std::string& file, const std::string& fallback) { return file.empty() ? fallback : file; };
	const auto body_name = name(body_file, "ImGui's default font");
	const auto heading_name = name(heading_file, body_name);
	printf("[MOCK] Fonts: %s for the body, %s for the Nexus link fonts, %s for the UI font\n", body_name.c_str(), heading_name.c_str(), trebuchet ? "Trebuchet MS" : body_name.c_str());
	return true;
}

// Nexus points the link at the fonts for the GW2 UI size, and ImGui's default font at FONT_DEFAULT.
static void update_nexus_fonts()
{
	static constexpr const char* Suffixes[] = { "_S", "_N", "_L", "_XL" };
	const auto ui_size = static_cast<size_t>(s_state.identity.UISize);
	const std::string suffix = ui_size < std::size(Suffixes) ? Suffixes[ui_size] : "_N";
	std::lock_guard lock(s_fonts.mutex);
	auto pointer = [](std::string_view id) -> ImFont* {
		const auto found = find_font(id);
		return found != s_fonts.fonts.end() ? found->pointer : nullptr;
	};
	auto& link = s_state.nexus_link;
	link.Font = pointer("MENOMONIA" + suffix);
	link.FontBig = pointer("MENOMONIA_BIG" + suffix);
	link.FontUI = pointer("TREBUCHET" + suffix);
	ImGui::GetIO().FontDefault = pointer("FONT_DEFAULT");
}

static bool advance_fonts(bool& rebuilt)
{
	using Receivers = std::vector<std::tuple<std::string, ImFont*, FONTS_RECEIVECALLBACK>>;
	auto collect = [](Receivers& receivers) {
		for (const auto& font : s_fonts.fonts)
			for (auto receiver : font.receivers)
				receivers.emplace_back(font.id, font.pointer, receiver);
	};
	Receivers before;
	{
		std::lock_guard lock(s_fonts.mutex);
		rebuilt = s_fonts.rebuild;
		if (!rebuilt)
			return true;
		s_fonts.rebuild = false;
		collect(before);
	}
	for (const auto& [id, pointer, receiver] : before)
		receiver(id.c_str(), nullptr);
	auto& link = s_state.nexus_link;
	link.Font = link.FontBig = link.FontUI = nullptr;
	ImGui::GetIO().FontDefault = nullptr;

	Receivers after;
	bool built = false;
	{
		std::lock_guard lock(s_fonts.mutex);
		auto& atlas = *ImGui::GetIO().Fonts;
		atlas.Clear();
		for (auto& font : s_fonts.fonts)
		{
			auto config = font.config;
			config.FontDataOwnedByAtlas = false;
			config.SizePixels = font.size;
			config.GlyphRanges = GlyphRanges;
			strncpy_s(config.Name, font.id.c_str(), _TRUNCATE);
			font.pointer = font.data.empty() ? atlas.AddFontDefault(&config) : atlas.AddFontFromMemoryTTF(font.data.data(), static_cast<int>(font.data.size()), font.size, &config, GlyphRanges);
		}
		built = atlas.Build();
		collect(after);
	}
	if (!built)
	{
		printf("[MOCK] ERROR: ImGui could not build the font atlas.\n");
		return false;
	}
	update_nexus_fonts();
	for (const auto& [id, pointer, receiver] : after)
		receiver(id.c_str(), pointer);
	return true;
}

// --- Nexus link ---

void update_nexus_link(unsigned width, unsigned height)
{
	s_state.nexus_link.Width = width;
	s_state.nexus_link.Height = height;
}

void set_dpi_scale(float scale) { ImGui::GetIO().FontGlobalScale = scale; }

// Nexus multiplies the GW2 UI size, the DPI scale and the shrink of windows smaller than 1024x768
static void update_scaling()
{
	static constexpr float UiSizes[] = { 0.90f, 1.00f, 1.11f, 1.22f };
	auto& link = s_state.nexus_link;
	const auto ui_size = static_cast<size_t>(s_state.identity.UISize);
	const float window = (std::min)((std::min)(link.Width, 1024u) / 1024.f, (std::min)(link.Height, 768u) / 768.f);
	link.Scaling = (ui_size < std::size(UiSizes) ? UiSizes[ui_size] : 1.f) * ImGui::GetIO().FontGlobalScale * window;
}

// a guardian standing in Lion's Arch, with the camera behind and above
static void populate_mumble()
{
	auto& identity = s_state.identity;
	strcpy_s(identity.Name, "Mock Character");
	identity.Profession = Mumble::EProfession::Guardian;
	identity.Specialization = 62; // Firebrand
	identity.Race = Mumble::ERace::Human;
	identity.MapID = 50;
	identity.FOV = 1.222f;
	identity.UISize = Mumble::EUIScale::Normal;

	auto& mumble = s_state.mumble;
	mumble.UIVersion = 2;
	mumble.UITick = 1;
	wcscpy_s(mumble.Name, L"Guild Wars 2");
	mumble.AvatarFront = { 0.f, 0.f, 1.f };
	mumble.CameraPosition = { 0.f, 3.f, -8.f };
	mumble.CameraFront = { 0.f, -0.2425f, 0.9701f };
	// the game writes the identity as JSON, which Nexus parses into the identity link
	swprintf_s(mumble.Identity, LR"({"name":"%hs","profession":%u,"spec":%u,"race":%u,"map_id":%u,"world_id":%u,"team_color_id":%u,"commander":%s,"fov":%.3f,"uisz":%u})", identity.Name, static_cast<unsigned>(identity.Profession), identity.Specialization, static_cast<unsigned>(identity.Race),
	    identity.MapID, identity.WorldID, identity.TeamColorID, identity.IsCommander ? L"true" : L"false", identity.FOV, static_cast<unsigned>(identity.UISize));
	mumble.ContextLength = 48;
	mumble.Context.MapID = identity.MapID;
	mumble.Context.MapType = Mumble::EMapType::Public;
	mumble.Context.ShardID = 1;
	mumble.Context.InstanceID = 1;
	mumble.Context.IsGameFocused = 1;
	mumble.Context.ProcessID = GetCurrentProcessId();

	s_state.nexus_link.IsGameplay = true;
}

// --- Startup and shutdown ---

static void* imgui_malloc(size_t size, void*) { return ImGui::MemAlloc(size); }
static void imgui_free(void* pointer, void*) { ImGui::MemFree(pointer); }

AddonAPI_t* create_mock_api(const std::filesystem::path& game_directory, ID3D11Device* device, IDXGISwapChain* swap_chain)
{
	s_game_directory = game_directory;
	s_textures.device = device;
	s_textures.internet = InternetOpenA("NexusMock", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	s_bind_worker = std::make_unique<Worker>();
	s_download_worker = std::make_unique<Worker>();

	populate_mumble();
	s_shared_blocks[DL_MUMBLE_LINK] = { &s_state.mumble, sizeof(s_state.mumble), nullptr };
	s_shared_blocks[DL_MUMBLE_LINK_IDENTITY] = { &s_state.identity, sizeof(s_state.identity), nullptr };
	s_shared_blocks[DL_NEXUS_LINK] = { &s_state.nexus_link, sizeof(s_state.nexus_link), nullptr };

	static AddonAPI_t api = {};
	api.SwapChain = swap_chain;
	api.ImguiContext = ImGui::GetCurrentContext();
	api.ImguiMalloc = reinterpret_cast<void*>(imgui_malloc);
	api.ImguiFree = reinterpret_cast<void*>(imgui_free);
	api.GUI_Register = mock_gui_register;
	api.GUI_Deregister = mock_gui_deregister;
	api.RequestUpdate = [](uint32_t, const char*) {};
	api.Log = mock_log;
	api.GUI_SendAlert = mock_gui_send_alert;
	api.GUI_RegisterCloseOnEscape = mock_gui_register_close_on_escape;
	api.GUI_DeregisterCloseOnEscape = mock_gui_deregister_close_on_escape;
	api.Paths_GetGameDirectory = mock_paths_get_game_directory;
	api.Paths_GetAddonDirectory = mock_paths_get_addon_directory;
	api.Paths_GetCommonDirectory = mock_paths_get_common_directory;
	api.MinHook_Create = mock_minhook_create;
	api.MinHook_Remove = mock_minhook_remove;
	api.MinHook_Enable = mock_minhook_enable;
	api.MinHook_Disable = mock_minhook_disable;
	api.Events_Raise = raise_event;
	api.Events_RaiseNotification = mock_events_raise_notification;
	api.Events_RaiseTargeted = mock_events_raise_targeted;
	api.Events_RaiseNotificationTargeted = mock_events_raise_notification_targeted;
	api.Events_Subscribe = mock_events_subscribe;
	api.Events_Unsubscribe = mock_events_unsubscribe;
	api.WndProc_Register = mock_wndproc_register;
	api.WndProc_Deregister = mock_wndproc_deregister;
	api.WndProc_SendToGameOnly = mock_wndproc_send_to_game_only;
	api.InputBinds_Invoke = mock_input_binds_invoke;
	api.InputBinds_RegisterWithString = mock_input_binds_register_with_string;
	api.InputBinds_RegisterWithStruct = mock_input_binds_register_with_struct;
	api.InputBinds_Deregister = mock_input_binds_deregister;
	api.GameBinds_PressAsync = mock_gamebinds_press_async;
	api.GameBinds_ReleaseAsync = mock_gamebinds_release_async;
	api.GameBinds_InvokeAsync = mock_gamebinds_invoke_async;
	api.GameBinds_Press = mock_gamebinds_press;
	api.GameBinds_Release = mock_gamebinds_release;
	api.GameBinds_IsBound = mock_gamebinds_is_bound;
	api.DataLink_Get = mock_datalink_get;
	api.DataLink_Share = mock_datalink_share;
	api.Textures_Get = mock_textures_get;
	api.Textures_GetOrCreateFromFile = mock_textures_get_or_create_from_file;
	api.Textures_GetOrCreateFromResource = mock_textures_get_or_create_from_resource;
	api.Textures_GetOrCreateFromURL = mock_textures_get_or_create_from_url;
	api.Textures_GetOrCreateFromMemory = mock_textures_get_or_create_from_memory;
	api.Textures_LoadFromFile = mock_textures_load_from_file;
	api.Textures_LoadFromResource = mock_textures_load_from_resource;
	api.Textures_LoadFromURL = mock_textures_load_from_url;
	api.Textures_LoadFromMemory = mock_textures_load_from_memory;
	api.QuickAccess_Add = mock_quickaccess_add;
	api.QuickAccess_Remove = mock_quickaccess_remove;
	api.QuickAccess_Notify = mock_quickaccess_notify;
	api.QuickAccess_AddContextMenu = mock_quickaccess_add_context_menu;
	api.QuickAccess_RemoveContextMenu = mock_quickaccess_remove_context_menu;
	api.Localization_Translate = mock_localization_translate;
	api.Localization_TranslateTo = mock_localization_translate_to;
	api.Localization_Set = mock_localization_set;
	api.Fonts_Get = mock_fonts_get;
	api.Fonts_Release = mock_fonts_release;
	api.Fonts_AddFromFile = mock_fonts_add_from_file;
	api.Fonts_AddFromResource = mock_fonts_add_from_resource;
	api.Fonts_AddFromMemory = mock_fonts_add_from_memory;
	api.Fonts_Resize = mock_fonts_resize;
	return &api;
}

void shutdown_mock_api()
{
	// closing the session ends a download in progress
	if (s_textures.internet)
		InternetCloseHandle(s_textures.internet);
	s_textures.internet = nullptr;
	s_download_worker.reset();
	s_bind_worker.reset();
	release_textures();
}

bool advance_mock_api(bool& fonts_rebuilt)
{
	advance_textures();
	advance_localization();
	update_scaling();
	return advance_fonts(fonts_rebuilt);
}

size_t release_addon_module()
{
	auto inside = [](auto pointer) { return inside_addon(pointer); };
	size_t count = 0;
	{
		std::lock_guard lock(s_state.mutex);
		for (auto* callbacks : { &s_state.pre_render, &s_state.render, &s_state.post_render, &s_state.options_render })
			count += std::erase_if(*callbacks, inside);
		count += std::erase_if(s_state.wndprocs, inside);
		for (auto& [id, subscriptions] : s_state.events)
			count += std::erase_if(subscriptions, inside);
		std::erase_if(s_state.events, [](const auto& event) { return event.second.empty(); });
		for (auto& bind : s_state.input_binds)
			if (bind.handler && inside(bind.handler))
			{
				bind.handler = nullptr;
				bind.held = false;
				++count;
			}
		count += std::erase_if(s_state.close_on_escape, [&](const auto& window) { return inside(window.second); });
		for (auto& [id, shortcut] : s_state.shortcuts)
			count += std::erase_if(shortcut.context_menu, [&](const auto& item) { return inside(item.second); });
	}
	{
		std::lock_guard lock(s_textures.mutex);
		count += std::erase_if(s_textures.queued, [&](const auto& texture) { return inside(texture.second.callback); });
	}
	{
		std::lock_guard lock(s_fonts.mutex);
		for (auto& font : s_fonts.fonts)
			count += std::erase_if(font.receivers, inside);
		if (std::erase_if(s_fonts.fonts, [](const auto& font) { return !font.nexus && font.receivers.empty(); }))
			s_fonts.rebuild = true;
	}
	return count;
}
