#pragma once

#include "imgui.h"
#include "Nexus.h"
#include "imgui_internal.h"
#include <d3d11.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Optional, deterministic input/capture script for addon smoke tests.
// Each line is: milliseconds move x y | down button | up button |
// capture filename.bmp | wheel delta | wheel_x delta | scale factor | escape | quit |
// theme dark|light|classic | color native-name r g b a |
// texture "identifier" "local-file" (paths are relative to the script).
// Coordinates are client-relative.
class TestScenario
{
    struct Event { long long time; std::string action; std::string arguments; };
    std::vector<Event> events;
    size_t next = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::filesystem::path directory;
    std::vector<std::filesystem::path> captures;
    bool logging = false;
    std::string captured_text;
    void (*clipboard_set)(void*, const char*) = nullptr;
    void* clipboard_user_data = nullptr;
    ImVec2 mouse = ImVec2(-FLT_MAX, -FLT_MAX);
    bool buttons[5]{};
    bool enabled = false;
    bool quit_requested = false;

public:
    bool load(const std::string& path)
    {
        if (path.empty()) return true;
        std::ifstream input(path);
        if (!input) return false;
        directory = std::filesystem::absolute(path).parent_path();
        std::string line;
        long long previous = 0;
        while (std::getline(input, line))
        {
            std::istringstream stream(line);
            stream >> std::ws;
            if (stream.eof() || stream.peek() == '#') continue;
            Event event{};
            if (!(stream >> event.time >> event.action) || event.time < previous) return false;
            std::getline(stream >> std::ws, event.arguments);
            if (event.action != "move" && event.action != "down" && event.action != "up" &&
                event.action != "capture" && event.action != "escape" && event.action != "quit" &&
                event.action != "wheel" && event.action != "wheel_x" && event.action != "scale" && event.action != "texture" &&
                event.action != "theme" && event.action != "color") return false;
            previous = event.time;
            events.push_back(std::move(event));
        }
        enabled = true;
        started = std::chrono::steady_clock::now();
        return true;
    }

    void before_frame(TEXTURES_GETORCREATEFROMFILE load_texture)
    {
        if (!enabled) return;
        auto& io = ImGui::GetIO();
        io.KeysDown[VK_ESCAPE] = false;
        io.MouseWheel = 0;
        io.MouseWheelH = 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        while (next < events.size() && events[next].time <= elapsed)
        {
            const auto& event = events[next++];
            std::istringstream values(event.arguments);
            bool valid = true;
            if (event.action == "move")
                valid = static_cast<bool>(values >> mouse.x >> mouse.y) && std::isfinite(mouse.x) && std::isfinite(mouse.y);
            else if (event.action == "down" || event.action == "up")
            {
                int button = 0;
                valid = static_cast<bool>(values >> button) && button >= 0 && button < 5;
                if (valid) buttons[button] = event.action == "down";
            }
            else if (event.action == "texture")
            {
                std::string id, path;
                if (!(values >> std::quoted(id) >> std::quoted(path)) || id.empty() || path.empty() || !load_texture)
                {
                    printf("[MOCK] Invalid texture preload.\n");
                    failed = quit_requested = true;
                    break;
                }
                const auto source = directory / path;
                load_texture(id.c_str(), source.string().c_str());
            }
            else if (event.action == "theme")
            {
                std::string preset;
                values >> preset;
                if (preset == "dark") ImGui::StyleColorsDark();
                else if (preset == "light") ImGui::StyleColorsLight();
                else if (preset == "classic") ImGui::StyleColorsClassic();
                else
                {
                    printf("[MOCK] Invalid theme preset.\n");
                    failed = quit_requested = true;
                    break;
                }
            }
            else if (event.action == "color")
            {
                std::string name;
                ImVec4 color;
                bool valid = static_cast<bool>(values >> name >> color.x >> color.y >> color.z >> color.w);
                valid = valid && color.x >= 0.f && color.x <= 1.f && color.y >= 0.f && color.y <= 1.f &&
                    color.z >= 0.f && color.z <= 1.f && color.w >= 0.f && color.w <= 1.f;
                int index = 0;
                while (index < ImGuiCol_COUNT && name != ImGui::GetStyleColorName(index)) ++index;
                if (!valid || index == ImGuiCol_COUNT)
                {
                    printf("[MOCK] Invalid style color.\n");
                    failed = quit_requested = true;
                    break;
                }
                ImGui::GetStyle().Colors[index] = color;
            }
            else if (event.action == "wheel" || event.action == "wheel_x")
            {
                float delta = 0.f;
                valid = static_cast<bool>(values >> delta) && std::isfinite(delta);
                if (valid) (event.action == "wheel" ? io.MouseWheel : io.MouseWheelH) = delta;
            }
            else if (event.action == "scale")
            {
                float scale = 1.f;
                valid = static_cast<bool>(values >> scale) && std::isfinite(scale) && scale > 0.f;
                if (valid) io.FontGlobalScale = scale;
            }
            else if (event.action == "escape") io.KeysDown[VK_ESCAPE] = true;
            else if (event.action == "capture")
            {
                valid = !event.arguments.empty();
                if (valid) captures.push_back(directory / event.arguments);
            }
            else if (event.action == "quit") quit_requested = true;
            if (!valid)
            {
                printf("[MOCK] Invalid arguments for %s.\n", event.action.c_str());
                failed = quit_requested = true;
                break;
            }
        }
        io.MousePos = mouse;
        for (int i = 0; i < 5; ++i) io.MouseDown[i] = buttons[i];
    }

    void begin_frame()
    {
        if (!captures.empty())
        {
            auto& io = ImGui::GetIO();
            captured_text.clear();
            clipboard_set = io.SetClipboardTextFn;
            clipboard_user_data = io.ClipboardUserData;
            io.ClipboardUserData = this;
            io.SetClipboardTextFn = [](void* self, const char* text) {
                static_cast<TestScenario*>(self)->captured_text += text;
            };
            ImGui::LogToClipboard(-1);
            logging = true;
        }
    }

    bool after_frame(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swap_chain,
        const std::map<std::string, Texture*>& textures)
    {
        if (logging)
        {
            ImGui::LogFinish();
            auto& io = ImGui::GetIO();
            io.SetClipboardTextFn = clipboard_set;
            io.ClipboardUserData = clipboard_user_data;
            logging = false;
        }
        for (const auto& path : captures)
        {
            std::ofstream(path.string() + ".ui.txt") << captured_text;
            if (!capture(device, context, swap_chain, path))
            {
                printf("[MOCK] Screenshot capture failed.\n");
                failed = true;
                quit_requested = true;
            }
            // Only record registered textures referenced by nonempty draw commands.
            std::ofstream drawn_textures(path.string() + ".textures.txt");
            const auto* draw_data = ImGui::GetDrawData();
            for (int list = 0; list < draw_data->CmdListsCount; ++list)
                for (const auto& command : draw_data->CmdLists[list]->CmdBuffer)
                    if (command.ElemCount > 0 && !command.UserCallback)
                        for (const auto& [id, texture] : textures)
                            if (texture && texture->Resource == command.TextureId)
                                drawn_textures << id << '\t' << command.ElemCount << '\t'
                                    << command.ClipRect.x << ',' << command.ClipRect.y << ','
                                    << command.ClipRect.z << ',' << command.ClipRect.w << '\n';
            // Record rendered windows so restart tests can assert runtime state.
            std::ofstream windows(path.string() + ".windows.txt");
            const auto& state = *ImGui::GetCurrentContext();
            windows << "Mouse\t" << state.IO.MousePos.x << '\t' << state.IO.MousePos.y << '\n';
            windows << "HoveredWindow\t" << (state.HoveredWindow ? state.HoveredWindow->Name : "none") << '\n';
            windows << "HoveredRootWindow\t" << (state.HoveredRootWindow ? state.HoveredRootWindow->Name : "none") << '\n';
            windows << "ActiveId\t" << state.ActiveId << "\tHoveredId\t" << state.HoveredId << '\n';
            windows << "ConfigFlags\t" << state.IO.ConfigFlags;
            for (int button = 0; button < 5; ++button)
                windows << "\tMouseButton" << button << ':' << state.IO.MouseDown[button] << ':' << state.IO.MouseDownOwned[button];
            windows << '\n';
            for (const auto* window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && !window->Hidden)
                    windows << window->Name << '\t' << window->Pos.x << '\t' << window->Pos.y
                            << '\t' << window->Size.x << '\t' << window->Size.y
                            << "\tflags=" << window->Flags << "\tactive=" << window->Active << "\thidden=" << window->Hidden << "\twas_active=" << window->WasActive
                            << "\touter=" << window->OuterRectClipped.Min.x << ',' << window->OuterRectClipped.Min.y << ',' << window->OuterRectClipped.Max.x << ',' << window->OuterRectClipped.Max.y
                            << "\tscroll=" << window->Scroll.x << ',' << window->Scroll.y
                            << "\tscroll_max=" << window->ScrollMax.x << ',' << window->ScrollMax.y << '\n';
        }
        captures.clear();
        return quit_requested;
    }

    bool failed = false;

private:
    static bool capture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swap_chain,
        const std::filesystem::path& path)
    {
        ID3D11Texture2D* source = nullptr;
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&source)))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        const auto created = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(created)) { source->Release(); return false; }
        context->CopyResource(staging, source);
        source->Release();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) { staging->Release(); return false; }
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
        info.biSize = sizeof(info);
        info.biWidth = desc.Width;
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info), sizeof(info));
        std::vector<unsigned char> row(desc.Width * 4);
        for (UINT y = 0; y < desc.Height; ++y)
        {
            const auto* pixels = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x)
            {
                row[x * 4] = pixels[x * 4 + 2];
                row[x * 4 + 1] = pixels[x * 4 + 1];
                row[x * 4 + 2] = pixels[x * 4];
                row[x * 4 + 3] = 255;
            }
            output.write(reinterpret_cast<const char*>(row.data()), row.size());
        }
        context->Unmap(staging, 0);
        staging->Release();
        output.close();
        return static_cast<bool>(output);
    }
};
