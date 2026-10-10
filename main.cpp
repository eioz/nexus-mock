#include "mock_api.h"
#include "test_scenario.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations
static bool CreateDeviceD3D(HWND hwnd);
static void CleanupDeviceD3D();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// D3D11 globals
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target = nullptr;

static void CreateRenderTarget()
{
	ID3D11Texture2D* back_buffer = nullptr;
	g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
	if (back_buffer)
	{
		g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
		back_buffer->Release();
	}
}

static void CleanupRenderTarget()
{
	if (g_render_target)
	{
		g_render_target->Release();
		g_render_target = nullptr;
	}
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (process_window_message(hWnd, msg, wParam, lParam))
		return 0;

	switch (msg)
	{
	case WM_SETCURSOR:
		// ImGui sets its cursor shapes over its windows
		if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
			return TRUE;
		break;
	case WM_SIZE:
		if (g_device && wParam != SIZE_MINIMIZED)
		{
			// like Nexus in its ResizeBuffers hook, the link gets the size and the event fires before the buffers change
			update_nexus_link(LOWORD(lParam), HIWORD(lParam));
			raise_event(EV_WINDOW_RESIZED, nullptr);
			CleanupRenderTarget();
			g_swap_chain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;
	case WM_GETMINMAXINFO:
		// lets the size presets exceed the screen
		reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxTrackSize = { 16384, 16384 };
		return 0;
	case WM_SYSCOMMAND:
		// Alt alone would open the window menu, which stalls the render loop
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool CreateDeviceD3D(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL feature_level;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &feature_level, &g_context);

	// The debug layer may not be installed, so retry without it
	if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
	{
		flags &= ~D3D11_CREATE_DEVICE_DEBUG;
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &feature_level, &g_context);
	}

	if (FAILED(hr))
	{
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &feature_level, &g_context);
		if (FAILED(hr))
			return false;
	}

	CreateRenderTarget();
	return true;
}

static void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_swap_chain)
	{
		g_swap_chain->Release();
		g_swap_chain = nullptr;
	}
	if (g_context)
	{
		g_context->Release();
		g_context = nullptr;
	}
	if (g_device)
	{
		g_device->Release();
		g_device = nullptr;
	}
}

static bool resize_client_area(HWND hwnd, int width, int height)
{
	RECT rect{ 0, 0, width, height };
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
	const DWORD extended_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&rect, style, GetMenu(hwnd) != nullptr, extended_style))
		return false;
	if (IsIconic(hwnd) || IsZoomed(hwnd))
		ShowWindow(hwnd, SW_RESTORE);
	return SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

// --- Launch options ---

struct LaunchOptions
{
	std::filesystem::path addon;
	bool hide_controls = false;
	std::string test_script;
	std::string font_file;
	std::string heading_font_file;
	std::vector<std::string> input_binds;
};

static void print_usage()
{
	printf("Usage: nexus_mock.exe <path-to-addon.dll> [options]\n");
	printf("Options:\n");
	printf("  --invoke-keybind <id>      Press and release an input bind of the addon after it loaded.\n");
	printf("  --hide-controls            Hide the mock controls for clean screenshots.\n");
	printf("  --font-file <path>         Use a TTF or OTF font for the body text.\n");
	printf("  --heading-font-file <path> Use a TTF or OTF font for the Nexus link fonts.\n");
	printf("  --test-script <path>       Replay timed input, capture frames, and exit after the last event.\n");
}

static bool parse_launch_options(int argc, char* argv[], LaunchOptions& options)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		std::string* value = nullptr;
		if (arg == "--invoke-keybind")
			value = &options.input_binds.emplace_back();
		else if (arg == "--test-script")
			value = &options.test_script;
		else if (arg == "--font-file")
			value = &options.font_file;
		else if (arg == "--heading-font-file")
			value = &options.heading_font_file;
		else if (arg == "--hide-controls")
			options.hide_controls = true;
		else if (arg == "--help" || arg == "-h" || arg == "/?")
			return false;
		else if (arg.starts_with("--"))
		{
			printf("[MOCK] ERROR: Unknown option: %s\n", argv[i]);
			return false;
		}
		else if (options.addon.empty())
			options.addon = argv[i];
		else
		{
			printf("[MOCK] ERROR: Unexpected argument: %s\n", argv[i]);
			return false;
		}

		if (value)
		{
			if (++i == argc)
			{
				printf("[MOCK] ERROR: %s needs a value.\n", argv[i - 1]);
				return false;
			}
			*value = argv[i];
		}
	}

	if (options.addon.empty())
	{
		printf("[MOCK] ERROR: Missing addon DLL path.\n");
		return false;
	}
	return true;
}

// --- Addon ---

struct Addon
{
	HMODULE module = nullptr;
	AddonDefinition_t* definition = nullptr;
};

static Addon load_addon(const std::filesystem::path& path)
{
	Addon addon;
	addon.module = LoadLibraryW(path.c_str());
	if (!addon.module)
	{
		printf("[MOCK] ERROR: Cannot load %s (error %lu).\n", path.string().c_str(), GetLastError());
		return {};
	}

	auto get_definition = reinterpret_cast<AddonDefinition_t* (*)()>(GetProcAddress(addon.module, "GetAddonDef"));
	addon.definition = get_definition ? get_definition() : nullptr;
	const auto* definition = addon.definition;
	if (!definition)
		printf("[MOCK] ERROR: %s exports no GetAddonDef.\n", path.string().c_str());
	// the minimum requirements Nexus checks
	else if (!definition->Signature || !definition->Name || !definition->Author || !definition->Description || !definition->Load || (!(definition->Flags & AF_DisableHotloading) && !definition->Unload))
		printf("[MOCK] ERROR: The addon definition needs a signature, a name, an author, a description, Load, and Unload unless hotloading is disabled.\n");
	else if (definition->APIVersion != NEXUS_API_VERSION)
		printf("[MOCK] ERROR: The addon uses API version %u, the mock implements %d.\n", definition->APIVersion, NEXUS_API_VERSION);
	else
	{
		const auto& version = definition->Version;
		printf("[MOCK] Loaded addon: %s v%d.%d.%d.%d by %s\n", definition->Name, version.Major, version.Minor, version.Build, version.Revision, definition->Author);
		return addon;
	}
	FreeLibrary(addon.module);
	return {};
}

// --- Nexus UI ---

// callbacks run on a copy, so they can register and deregister
static std::vector<GUI_RENDER> callbacks(std::vector<GUI_RENDER> MockState::* list)
{
	auto& state = get_mock_state();
	std::lock_guard lock(state.mutex);
	return state.*list;
}

// Nexus shows one alert at a time for five seconds, then fades it out over two and a half
static void render_alerts()
{
	auto& state = get_mock_state();
	std::string message;
	float alpha = 1.f;
	{
		std::lock_guard lock(state.mutex);
		const auto now = std::chrono::steady_clock::now();
		while (!state.alerts.empty())
		{
			auto& alert = state.alerts.front();
			if (alert.shown == std::chrono::steady_clock::time_point{})
				alert.shown = now;
			const float age = std::chrono::duration<float>(now - alert.shown).count();
			if (age < 7.5f)
			{
				message = alert.message;
				alpha = age < 5.f ? 1.f : 1.f - std::pow((age - 5.f) / 2.5f, 3.f);
				break;
			}
			state.alerts.erase(state.alerts.begin());
		}
	}
	if (message.empty())
		return;

	const auto& link = state.nexus_link;
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 230.f * link.Scaling), ImGuiCond_Always, ImVec2(0.5f, 0.f));
	constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (ImGui::Begin("##Alerts", nullptr, flags))
	{
		if (link.FontBig)
			ImGui::PushFont(static_cast<ImFont*>(link.FontBig));
		// yellow text with a black outline
		const auto position = ImGui::GetCursorScreenPos();
		const auto outline = ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, alpha));
		for (const auto& offset : { ImVec2(-1.f, 0.f), ImVec2(1.f, 0.f), ImVec2(0.f, -1.f), ImVec2(0.f, 1.f) })
			ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + offset.x, position.y + offset.y), outline, message.c_str());
		ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, alpha), "%s", message.c_str());
		if (link.FontBig)
			ImGui::PopFont();
	}
	ImGui::End();
}

// Nexus shows the first options callback of an addon in its "Nexus" window
static void render_nexus_window(bool& open, const char* addon_name)
{
	if (!open)
		return;
	const auto options = callbacks(&MockState::options_render);
	const auto& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(620.f, 460.f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Nexus", &open))
	{
		ImGui::TextUnformatted(addon_name);
		if (options.empty())
			ImGui::TextDisabled("No options");
		else if (ImGui::CollapsingHeader("Options", ImGuiTreeNodeFlags_DefaultOpen))
			options.front()();
	}
	ImGui::End();
}

// Nexus sorts the shortcuts by identifier, and shows one with context menu items or an input bind with a handler
static void render_quick_access()
{
	auto& state = get_mock_state();
	std::vector<std::pair<std::string, MockShortcut>> shortcuts;
	std::map<std::string, std::string> binds; // the handled input binds, as text
	{
		std::lock_guard lock(state.mutex);
		for (const auto& bind : state.input_binds)
			if (bind.handler)
				binds[bind.id] = bind.bind.code ? bind_text(bind.bind) : "";
		for (const auto& [id, shortcut] : state.shortcuts)
			if (!shortcut.context_menu.empty() || binds.contains(shortcut.input_bind))
				shortcuts.emplace_back(id, shortcut);
	}
	if (shortcuts.empty())
		return;

	static std::string hovered; // the hover texture shows from the next frame on
	std::string hovered_now;
	ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_Always);
	constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
	if (ImGui::Begin("##QuickAccess", nullptr, flags))
	{
		for (const auto& [id, shortcut] : shortcuts)
		{
			ImGui::PushID(id.c_str());
			const ImVec2 size(32.f, 32.f);
			bool clicked = false;
			if (const auto* texture = get_texture(hovered == id ? shortcut.hover_texture : shortcut.texture))
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.15f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.25f));
				clicked = ImGui::ImageButton(texture->Resource, size, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), 0);
				ImGui::PopStyleColor(3);
			}
			else
			{
				// the tooltip's first letter stands in for a missing texture
				const char label[] = { shortcut.tooltip.empty() ? '?' : shortcut.tooltip[0], '\0' };
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.35f, 0.5f, 1.f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.7f, 1.f));
				clicked = ImGui::Button(label, size);
				ImGui::PopStyleColor(2);
			}
			if (ImGui::IsItemHovered())
			{
				hovered_now = id;
				const auto bind = binds.find(shortcut.input_bind);
				const auto tooltip = shortcut.tooltip + (bind != binds.end() && !bind->second.empty() ? " (" + bind->second + ")" : "");
				if (!shortcut.tooltip.empty())
					ImGui::SetTooltip("%s", tooltip.c_str());
			}
			if (shortcut.notified)
				ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetItemRectMax().x - 5.f, ImGui::GetItemRectMin().y + 5.f), 4.f, IM_COL32(255, 200, 0, 255));

			// a click clears the notification and invokes only the press
			if (clicked && !shortcut.input_bind.empty())
			{
				{
					std::lock_guard lock(state.mutex);
					if (const auto found = state.shortcuts.find(id); found != state.shortcuts.end())
						found->second.notified = false;
				}
				invoke_input_bind(shortcut.input_bind, false);
			}

			if (!shortcut.context_menu.empty() && ImGui::BeginPopupContextItem("ShortcutsCtxMenu"))
			{
				bool first = true;
				for (const auto& [item, callback] : shortcut.context_menu)
				{
					if (!callback)
						continue;
					if (!first)
						ImGui::Separator();
					first = false;
					callback();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
			ImGui::SameLine(0.f, 4.f);
		}
	}
	ImGui::End();
	hovered = hovered_now;
}

// --- Mock controls ---

// A resize sends WM_SIZE right away, so the size is applied after the frame.
static void render_mock_controls(SIZE& requested_size)
{
	auto& state = get_mock_state();
	std::vector<std::pair<std::string, std::string>> binds;
	std::vector<std::pair<std::string, size_t>> events;
	std::vector<std::pair<std::string, bool>> windows;
	size_t render_counts[4] = {};
	{
		std::lock_guard lock(state.mutex);
		for (const auto& bind : state.input_binds)
			if (bind.handler)
				binds.emplace_back(bind.id, bind_text(bind.bind));
		for (const auto& [id, subscriptions] : state.events)
			events.emplace_back(id, subscriptions.size());
		for (const auto& [name, visible] : state.close_on_escape)
			windows.emplace_back(name, *visible);
		size_t i = 0;
		for (const auto* list : { &state.pre_render, &state.render, &state.post_render, &state.options_render })
			render_counts[i++] = list->size();
	}

	// back to the top right after a resize, so the size presets stay in reach
	static float display_width = 0.f;
	const float width = ImGui::GetIO().DisplaySize.x;
	ImGui::SetNextWindowPos(ImVec2(width - 350.f, 10.f), display_width == 0.f || display_width == width ? ImGuiCond_FirstUseEver : ImGuiCond_Always);
	display_width = width;
	ImGui::SetNextWindowSize(ImVec2(340.f, 470.f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Mock Controls"))
	{
		ImGui::TextUnformatted("Mumble Link");
		ImGui::Separator();
		auto& context = state.mumble.Context;
		bool in_combat = context.IsInCombat;
		if (ImGui::Checkbox("In Combat", &in_combat))
			context.IsInCombat = in_combat;
		bool map_open = context.IsMapOpen;
		if (ImGui::Checkbox("Map Open", &map_open))
			context.IsMapOpen = map_open;
		ImGui::Text("UI Tick: %u", state.mumble.UITick);

		ImGui::Spacing();
		ImGui::TextUnformatted("Nexus Link");
		ImGui::Separator();
		const auto& link = state.nexus_link;
		ImGui::Text("Window: %ux%u, scaling %.2f", link.Width, link.Height, link.Scaling);
		for (const auto [width, height] : { std::pair{ 1280, 800 }, std::pair{ 1920, 1080 }, std::pair{ 860, 480 } })
		{
			const auto label = std::to_string(width) + "x" + std::to_string(height);
			if (ImGui::Button(label.c_str()))
				requested_size = { width, height };
			ImGui::SameLine();
		}
		ImGui::NewLine();

		ImGui::Spacing();
		ImGui::TextUnformatted("Input Binds");
		ImGui::Separator();
		for (const auto& [id, bind] : binds)
		{
			ImGui::PushID(id.c_str());
			if (ImGui::SmallButton("Invoke"))
			{
				invoke_input_bind(id, false);
				invoke_input_bind(id, true);
			}
			ImGui::SameLine();
			ImGui::Text("%s: %s", id.c_str(), bind.c_str());
			ImGui::PopID();
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Events");
		ImGui::Separator();
		if (events.empty())
			ImGui::TextDisabled("(none)");
		for (const auto& [id, count] : events)
			ImGui::Text("%s (%zu)", id.c_str(), count);

		ImGui::Spacing();
		ImGui::TextUnformatted("Close On Escape");
		ImGui::Separator();
		for (const auto& [name, visible] : windows)
			ImGui::Text("%s: %s", name.c_str(), visible ? "open" : "closed");

		ImGui::Spacing();
		ImGui::TextUnformatted("Render Callbacks");
		ImGui::Separator();
		ImGui::Text("Pre: %zu, render: %zu, post: %zu, options: %zu", render_counts[0], render_counts[1], render_counts[2], render_counts[3]);
	}
	ImGui::End();
}

// --- Entry point ---

static std::atomic<bool> s_toggle_menu = false;
static std::atomic<bool> s_toggle_ui = false;

// runs on the input bind worker like any handler, so the render loop applies the toggles
static void on_nexus_input_bind(const char* id, bool release)
{
	if (release)
		return;
	if (std::string_view(id) == "KB_MENU")
		s_toggle_menu = true;
	else if (std::string_view(id) == "KB_TOGGLEHIDEUI")
		s_toggle_ui = true;
}

static int run(HWND hwnd, const LaunchOptions& options, AddonAPI_t* api)
{
	auto& state = get_mock_state();

	// what Nexus registers before any addon: its menu and hide binds, its window and its quick access shortcut
	bool menu_open = false;
	bool ui_visible = true;
	api->InputBinds_RegisterWithString("KB_MENU", on_nexus_input_bind, "CTRL+O");
	api->InputBinds_RegisterWithString("KB_TOGGLEHIDEUI", on_nexus_input_bind, "CTRL+H");
	api->GUI_RegisterCloseOnEscape("Nexus", &menu_open);
	api->QuickAccess_Add(NexusShortcut, "ICON_NEXUS", "ICON_NEXUS_HOVER", "KB_MENU", "Nexus");

	const auto addon = load_addon(options.addon);
	if (!addon.module)
		return 1;

	printf("[MOCK] Calling Load...\n");
	set_addon_module(addon.module, addon.definition->Signature);
	addon.definition->Load(api);
	// Nexus raises these after every Load
	raise_event(EV_ADDON_LOADED, &addon.definition->Signature);
	raise_event(EV_MUMBLE_IDENTITY_UPDATED, &state.identity);

	TestScenario scenario;
	bool failed = !scenario.load(options.test_script);
	for (const auto& id : options.input_binds)
	{
		printf("[MOCK] Invoking input bind: %s\n", id.c_str());
		if (!invoke_input_bind(id, false) || !invoke_input_bind(id, true))
		{
			printf("[MOCK] ERROR: The addon has no input bind %s.\n", id.c_str());
			failed = true;
		}
	}
	wait_for_input_binds();

	const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.f };
	SIZE requested_size{};
	bool done = failed;
	while (!done)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			done |= msg.message == WM_QUIT;
		}
		if (done)
			break;

		if (s_toggle_menu.exchange(false))
			menu_open = !menu_open;
		if (s_toggle_ui.exchange(false))
			ui_visible = !ui_visible;

		// the game's part of the frame: Mumble ticks and the scene is drawn
		state.mumble.UITick++;
		if (g_render_target)
		{
			g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
			g_context->ClearRenderTargetView(g_render_target, clear_color);
		}

		// Nexus's part: textures, strings and fonts, then the callbacks around its ImGui frame
		bool fonts_rebuilt = false;
		if (!advance_mock_api(fonts_rebuilt))
		{
			failed = true;
			break;
		}
		if (fonts_rebuilt)
			ImGui_ImplDX11_InvalidateDeviceObjects();

		for (auto callback : callbacks(&MockState::pre_render))
			callback();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		scenario.before_frame(hwnd);
		ImGui::NewFrame();
		if (ui_visible)
		{
			for (auto callback : callbacks(&MockState::render))
				callback();
			render_alerts();
			render_nexus_window(menu_open, addon.definition->Name);
			render_quick_access();
		}
		if (!options.hide_controls)
			render_mock_controls(requested_size);
		ImGui::Render();
		if (g_render_target)
		{
			g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		}

		for (auto callback : callbacks(&MockState::post_render))
			callback();

		done = scenario.after_frame(g_device, g_context, g_swap_chain);
		// a minimized window presents without waiting for the display, which would spin the loop
		if (g_swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED)
			Sleep(10);
		if (requested_size.cx > 0)
		{
			if (!resize_client_area(hwnd, requested_size.cx, requested_size.cy))
				printf("[MOCK] ERROR: Cannot resize the client area (error %lu).\n", GetLastError());
			requested_size = {};
		}
	}

	// Nexus calls Unload, drops what the addon left registered and frees it. Quitting raises no EV_ADDON_UNLOADED.
	printf("[MOCK] Calling Unload...\n");
	wait_for_input_binds();
	if (addon.definition->Unload)
		addon.definition->Unload();
	if (const auto left = release_addon_module())
		printf("[MOCK] WARN: The addon left %zu callbacks registered, the mock removed them.\n", left);
	wait_for_input_binds();
	FreeLibrary(addon.module);
	return failed || scenario.failed ? 1 : 0;
}

int main(int argc, char* argv[])
{
	LaunchOptions options;
	if (!parse_launch_options(argc, argv, options))
	{
		print_usage();
		return 1;
	}
	std::error_code error;
	if (!std::filesystem::is_regular_file(options.addon, error))
	{
		printf("[MOCK] ERROR: File not found: %s\n", options.addon.string().c_str());
		return 1;
	}

	// The data folder stands in for the game directory, with the folders Nexus creates at startup
	const auto game_directory = std::filesystem::absolute("data");
	std::filesystem::create_directories(game_directory / "addons" / "common", error);
	if (error)
	{
		printf("[MOCK] ERROR: Cannot create %s: %s\n", (game_directory / "addons" / "common").string().c_str(), error.message().c_str());
		return 1;
	}

	printf("[MOCK] Starting Nexus Mock...\n");
	WNDCLASSEXW window_class = { sizeof(window_class), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"NexusMock", nullptr };
	RegisterClassExW(&window_class);
	const auto title = L"Nexus Mock - " + options.addon.stem().wstring();
	HWND hwnd = CreateWindowW(window_class.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, window_class.hInstance, nullptr);

	int result = 1;
	if (hwnd && resize_client_area(hwnd, 1280, 800) && CreateDeviceD3D(hwnd))
	{
		ShowWindow(hwnd, SW_SHOWDEFAULT);
		UpdateWindow(hwnd);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		// scripted runs start from the same window layout every time
		if (!options.test_script.empty())
			ImGui::GetIO().IniFilename = nullptr;
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(hwnd);
		ImGui_ImplDX11_Init(g_device, g_context);

		auto* api = create_mock_api(game_directory, g_device, g_swap_chain);
		RECT client;
		GetClientRect(hwnd, &client);
		update_nexus_link(client.right - client.left, client.bottom - client.top);
		if (add_nexus_fonts(options.font_file, options.heading_font_file))
			result = run(hwnd, options, api);
		shutdown_mock_api();

		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
	else
		printf("[MOCK] ERROR: Cannot create the window and its D3D11 device.\n");

	CleanupDeviceD3D();
	if (hwnd)
		DestroyWindow(hwnd);
	UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
	return result;
}
