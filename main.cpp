#include "mock_api.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <d3d11.h>
#include <filesystem>
#include <fstream>

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
	// Forward to registered addon WndProc callbacks before ImGui or game processing
	{
		auto cbs = get_mock_state().wndproc_callbacks;
		for (auto& cb : cbs)
			if (cb(hWnd, msg, wParam, lParam) == 0)
				return 0;
	}

	// Process keybinds before ImGui consumes keyboard messages
	process_keybind_message(msg, wParam, lParam);

	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (g_device && wParam != SIZE_MINIMIZED)
		{
			CleanupRenderTarget();
			g_swap_chain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
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

	// Debug layer may not be installed — retry without it
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

// --- DLL loading ---

using GetAddonDefFn = AddonDefinition* (*)();

struct LoadedAddon
{
	HMODULE handle = nullptr;
	AddonDefinition* definition = nullptr;
};

static LoadedAddon load_addon_dll(const char* path)
{
	LoadedAddon addon;

	addon.handle = LoadLibraryA(path);
	if (!addon.handle)
	{
		printf("[MOCK] ERROR: Failed to load DLL: %s (error %lu)\n", path, GetLastError());
		return addon;
	}

	auto get_def = reinterpret_cast<GetAddonDefFn>(GetProcAddress(addon.handle, "GetAddonDef"));
	if (!get_def)
	{
		printf("[MOCK] ERROR: DLL does not export GetAddonDef: %s\n", path);
		FreeLibrary(addon.handle);
		addon.handle = nullptr;
		return addon;
	}

	addon.definition = get_def();
	if (!addon.definition)
	{
		printf("[MOCK] ERROR: GetAddonDef returned null: %s\n", path);
		FreeLibrary(addon.handle);
		addon.handle = nullptr;
		return addon;
	}

	printf("[MOCK] Loaded addon: %s v%d.%d.%d.%d by %s (API v%u)\n", addon.definition->Name, addon.definition->Version.Major, addon.definition->Version.Minor, addon.definition->Version.Build, addon.definition->Version.Revision, addon.definition->Author, addon.definition->APIVersion);

	if (addon.definition->APIVersion != NEXUS_API_VERSION)
	{
		printf("[MOCK] ERROR: Addon API version %u does not match mock API version %u\n", addon.definition->APIVersion, NEXUS_API_VERSION);
		FreeLibrary(addon.handle);
		addon.handle = nullptr;
		addon.definition = nullptr;
		return addon;
	}

	return addon;
}

// --- Quick Access bar ---

static void render_quick_access()
{
	auto quick_access = get_mock_state().quick_access;
	if (quick_access.empty())
		return;

	ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.75f);
	constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

	if (ImGui::Begin("##QuickAccess", nullptr, flags))
	{
		constexpr float icon_size = 32.0f;

		for (auto& entry : quick_access)
		{
			Texture* tex = mock_textures_get(entry.texture_id.c_str());

			bool clicked = false;
			if (tex && tex->Resource)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.25f));
				clicked = ImGui::ImageButton((ImTextureID)tex->Resource, ImVec2(icon_size, icon_size));
				ImGui::PopStyleColor(3);
			}
			else
			{
				// Placeholder: colored square with first letter of tooltip
				char label[4] = { '?', '\0' };
				if (!entry.tooltip.empty())
					label[0] = entry.tooltip[0];

				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.35f, 0.5f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
				clicked = ImGui::Button(label, ImVec2(icon_size, icon_size));
				ImGui::PopStyleColor(2);
			}

			if (clicked && !entry.keybind_id.empty())
			{
				invoke_keybind(entry.keybind_id.c_str(), false);
				invoke_keybind(entry.keybind_id.c_str(), true);
			}

			if (!entry.context_menus.empty())
			{
				auto popup_id = "##ctx_" + entry.id;
				if (ImGui::BeginPopupContextItem(popup_id.c_str()))
				{
					for (auto& [menu_id, cb] : entry.context_menus)
						cb();
					ImGui::EndPopup();
				}
			}

			if (ImGui::IsItemHovered() && !entry.tooltip.empty())
				ImGui::SetTooltip("%s", entry.tooltip.c_str());

			ImGui::SameLine(0.0f, 4.0f);
		}
	}
	ImGui::End();
}

// --- Alert overlay ---

static void render_alerts()
{
	auto& state = get_mock_state();
	auto now = std::chrono::steady_clock::now();
	constexpr float display_seconds = 3.0f;

	std::lock_guard<std::mutex> lock(state.alerts_mutex);
	state.alerts.erase(std::remove_if(state.alerts.begin(), state.alerts.end(), [&](const MockAlert& a) { return std::chrono::duration<float>(now - a.created).count() > display_seconds; }), state.alerts.end());

	if (state.alerts.empty())
		return;

	const auto& latest = state.alerts.back();
	float age = std::chrono::duration<float>(now - latest.created).count();
	float alpha = 1.0f - (age / display_seconds);

	ImGuiIO& io = ImGui::GetIO();
	ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.15f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(alpha * 0.85f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 8));

	constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs;

	if (ImGui::Begin("##Alert", nullptr, flags))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, alpha));
		ImGui::Text("%s", latest.message.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

// --- Mock control panel ---

static void render_mock_controls()
{
	auto& state = get_mock_state();

	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Mock Controls"))
	{
		ImGui::Text("Mumble Link");
		ImGui::Separator();

		bool in_combat = state.mumble_data.Context.IsInCombat;
		if (ImGui::Checkbox("In Combat", &in_combat))
			state.mumble_data.Context.IsInCombat = in_combat ? 1 : 0;

		ImGui::Spacing();
		ImGui::Text("Nexus Link");
		ImGui::Separator();
		ImGui::Text("Window: %ux%u", state.nexus_link_data.Width, state.nexus_link_data.Height);
		ImGui::Text("Scaling: %.1f", state.nexus_link_data.Scaling);

		ImGui::Spacing();
		ImGui::Text("Keybinds");
		ImGui::Separator();

		auto keybinds = state.keybinds;
		for (auto& entry : keybinds)
		{
			ImGui::Text("%s (%s)", entry.id.c_str(), entry.keybind_string.c_str());
			ImGui::SameLine();
			auto btn_label = "Invoke##" + entry.id;
			if (ImGui::SmallButton(btn_label.c_str()))
			{
				invoke_keybind(entry.id.c_str(), false);
				invoke_keybind(entry.id.c_str(), true);
			}
		}

		ImGui::Spacing();
		ImGui::Text("Events");
		ImGui::Separator();
		if (state.events_subscribers.empty())
		{
			ImGui::TextDisabled("(none)");
		}
		else
		{
			for (auto& [event_name, subs] : state.events_subscribers)
				ImGui::Text("%s (%zu)", event_name.c_str(), subs.size());
		}

		ImGui::Spacing();
		ImGui::Text("Close On Escape");
		ImGui::Separator();
		if (state.close_on_escape.empty())
		{
			ImGui::TextDisabled("(none)");
		}
		else
		{
			for (auto& [id, pBool] : state.close_on_escape)
				ImGui::Text("%s: %s", id.c_str(), (pBool && *pBool) ? "open" : "closed");
		}

		ImGui::Spacing();
		ImGui::Text("Callbacks");
		ImGui::Separator();
		ImGui::Text("Pre-render: %zu", state.pre_render_callbacks.size());
		ImGui::Text("Render: %zu", state.render_callbacks.size());
		ImGui::Text("Post-render: %zu", state.post_render_callbacks.size());
		ImGui::Text("Options: %zu", state.options_render_callbacks.size());
	}
	ImGui::End();

	auto options_cbs = state.options_render_callbacks;
	if (!options_cbs.empty())
	{
		ImGui::SetNextWindowPos(ImVec2(10, 270), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Addon Options"))
		{
			for (auto& cb : options_cbs)
				cb();
		}
		ImGui::End();
	}
}

// --- Entry point ---

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		printf("Usage: nexus_mock.exe <path-to-addon.dll>\n");
		return 1;
	}

	const char* dll_path = argv[1];

	if (!std::filesystem::exists(dll_path))
	{
		printf("[MOCK] ERROR: File not found: %s\n", dll_path);
		return 1;
	}

	printf("[MOCK] Starting Nexus Mock...\n");

	// Create data directory next to executable
	std::filesystem::create_directories("data");

	// Prepare arcdps directories for monitor startup.
	{
		auto arcdps_path = std::filesystem::absolute("data/arcdps");
		std::filesystem::create_directories(arcdps_path);
		std::filesystem::create_directories(arcdps_path / "arcdps.cbtlogs");

		auto ini_path = arcdps_path / "arcdps.ini";
		if (!std::filesystem::exists(ini_path))
		{
			auto path_str = arcdps_path.string();
			std::string ini_content = "[session]\nboss_encounter_path=" + path_str + "\n\n[main]\nboss_encounter_path=" + path_str + "\n";
			if (std::ofstream f(ini_path); f.is_open())
				f << ini_content;
		}

		auto log_directory = (arcdps_path / "arcdps.cbtlogs").string();
		printf("[MOCK] Log directory: %s\n", log_directory.c_str());
	}

	// Create window
	WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"NexusMock", nullptr };
	RegisterClassEx(&wc);

	// Build window title from DLL name
	auto dll_name = std::filesystem::path(dll_path).stem().string();
	auto title = "Nexus Mock - " + dll_name;
	std::wstring wide_title(title.begin(), title.end());
	const wchar_t* wide_title_text = wide_title.c_str();

	HWND hwnd = CreateWindow(wc.lpszClassName, wide_title_text, WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

	if (!CreateDeviceD3D(hwnd))
	{
		printf("[MOCK] ERROR: Failed to create D3D11 device.\n");
		CleanupDeviceD3D();
		DestroyWindow(hwnd);
		UnregisterClass(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	// Setup ImGui
	IMGUI_CHECKVERSION();
	ImGuiContext* ctx = ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	// Populate mock state
	auto& state = get_mock_state();

	RECT client_rect;
	GetClientRect(hwnd, &client_rect);
	state.nexus_link_data.Width = client_rect.right - client_rect.left;
	state.nexus_link_data.Height = client_rect.bottom - client_rect.top;
	state.nexus_link_data.Scaling = 1.0f;
	state.nexus_link_data.IsGameplay = true;

	// Populate font pointers from ImGui defaults (build atlas first)
	io.Fonts->AddFontDefault();
	io.Fonts->Build();
	ImFont* default_font = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0] : nullptr;
	state.nexus_link_data.Font = default_font;
	state.nexus_link_data.FontBig = default_font;
	state.nexus_link_data.FontUI = default_font;

	// Create mock API and inject D3D11/ImGui context
	set_mock_device(g_device);
	AddonAPI* mock_api = create_mock_api();
	mock_api->SwapChain = g_swap_chain;
	mock_api->ImguiContext = ctx;

	static auto imgui_malloc = [](size_t sz, void*) -> void* { return malloc(sz); };
	static auto imgui_free = [](void* ptr, void*) { free(ptr); };
	mock_api->ImguiMalloc = reinterpret_cast<void*>(+imgui_malloc);
	mock_api->ImguiFree = reinterpret_cast<void*>(+imgui_free);

	// Load the addon DLL
	auto addon = load_addon_dll(dll_path);
	if (!addon.handle || !addon.definition || !addon.definition->Load)
	{
		printf("[MOCK] ERROR: Failed to load addon.\n");
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		CleanupDeviceD3D();
		DestroyWindow(hwnd);
		UnregisterClass(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	printf("[MOCK] Initializing addon...\n");
	addon.definition->Load(mock_api);
	printf("[MOCK] Addon initialized. Starting render loop.\n");

	// Render loop
	const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
	MSG msg;
	ZeroMemory(&msg, sizeof(msg));
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}

		GetClientRect(hwnd, &client_rect);
		state.nexus_link_data.Width = client_rect.right - client_rect.left;
		state.nexus_link_data.Height = client_rect.bottom - client_rect.top;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		flush_pending_texture_uploads();

		auto pre_cbs = state.pre_render_callbacks;
		auto render_cbs = state.render_callbacks;
		auto post_cbs = state.post_render_callbacks;

		for (auto& cb : pre_cbs)
			cb();

		render_mock_controls();

		for (auto& cb : render_cbs)
			cb();

		render_quick_access();
		render_alerts();

		for (auto& cb : post_cbs)
			cb();

		ImGui::Render();

		if (g_render_target)
		{
			g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
			g_context->ClearRenderTargetView(g_render_target, clear_color);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		}

		g_swap_chain->Present(1, 0);
	}

	// Cleanup
	printf("[MOCK] Unloading addon...\n");
	if (addon.definition->Unload)
		addon.definition->Unload();
	FreeLibrary(addon.handle);

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	DestroyWindow(hwnd);
	UnregisterClass(wc.lpszClassName, wc.hInstance);

	return 0;
}
