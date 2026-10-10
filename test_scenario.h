#pragma once

#include "mock_api.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Optional, deterministic input and capture script for addon tests. The README describes the format.
class TestScenario
{
public:
	bool failed = false;

	// An empty path disables scripting.
	bool load(const std::string& path)
	{
		if (path.empty())
			return true;
		std::ifstream input(path);
		if (!input)
		{
			printf("[MOCK] ERROR: Cannot open the test script: %s\n", path.c_str());
			return false;
		}
		directory = std::filesystem::absolute(path).parent_path();
		std::string line;
		for (int number = 1; std::getline(input, line); ++number)
		{
			if (number == 1 && line.starts_with("\xEF\xBB\xBF"))
				line.erase(0, 3);
			std::istringstream stream(line);
			stream >> std::ws;
			if (stream.eof() || stream.peek() == '#')
				continue;
			Event event;
			auto error = parse(stream, event);
			if (error.empty() && !events.empty() && event.time < events.back().time)
				error = "times must not decrease";
			if (!error.empty())
			{
				printf("[MOCK] ERROR: Test script line %d: %s\n", number, error.c_str());
				return false;
			}
			events.push_back(std::move(event));
		}
		enabled = true;
		started = std::chrono::steady_clock::now();
		return true;
	}

	// Applies the events that are due. Runs after the platform backend's NewFrame, which reads the real mouse. Buttons, the
	// wheel and Escape go through the window procedure like real input.
	void before_frame(HWND hwnd)
	{
		if (!enabled)
			return;
		auto& io = ImGui::GetIO();
		unsigned changed = 0;
		if (escape_down)
		{
			send_key(hwnd, VK_ESCAPE, false);
			escape_down = false;
			changed |= input_of(Action::Escape);
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		while (next < events.size() && events[next].time <= elapsed && !quit_requested)
		{
			const auto& event = events[next];
			// ImGui 1.80 reads its input once per frame, so a second change of the same input waits for the next frame. A
			// button or the wheel also waits after a move, until ImGui knows whether the mouse is over one of its windows.
			const auto input = input_of(event.action, event.button);
			const bool mouse_message = event.action == Action::Down || event.action == Action::Up || event.action == Action::Wheel || event.action == Action::WheelX;
			if ((changed & input) || (mouse_message && (changed & input_of(Action::Move))))
				break;
			changed |= input;
			++next;
			switch (event.action)
			{
			case Action::Move:
				mouse = event.position;
				send_mouse(hwnd, WM_MOUSEMOVE);
				break;
			case Action::Down:
			case Action::Up:
			{
				const bool down = event.action == Action::Down;
				static constexpr UINT DownMessages[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN };
				static constexpr UINT UpMessages[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP };
				buttons[event.button] = down;
				send_mouse(hwnd, (down ? DownMessages : UpMessages)[event.button], event.button == 3 ? XBUTTON1 : event.button == 4 ? XBUTTON2 : 0);
				break;
			}
			case Action::Wheel:
			case Action::WheelX:
				send_mouse(hwnd, event.action == Action::Wheel ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL, static_cast<WORD>(std::lround(event.value * WHEEL_DELTA)));
				break;
			case Action::Escape:
				// through the window procedure, so close on escape and input binds see it like a real key press
				send_key(hwnd, VK_ESCAPE, true);
				escape_down = true;
				break;
			case Action::Scale:
				set_dpi_scale(event.value);
				break;
			case Action::Theme:
				if (event.text == "dark")
					ImGui::StyleColorsDark();
				else if (event.text == "light")
					ImGui::StyleColorsLight();
				else
					ImGui::StyleColorsClassic();
				break;
			case Action::Color:
				ImGui::GetStyle().Colors[event.style_color] = event.color;
				break;
			case Action::Texture:
				if (!preload_texture(event.text, directory / event.file))
					failed = quit_requested = true;
				break;
			case Action::Capture:
				captures.push_back(directory / event.file);
				break;
			case Action::Quit:
				quit_requested = true;
				break;
			}
		}
		io.MousePos = mouse;
	}

	// Writes the captures of the rendered frame. Returns true when the run is over.
	bool after_frame(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swap_chain)
	{
		if (!enabled)
			return false;
		for (const auto& path : captures)
		{
			if (!capture(device, context, swap_chain, path))
			{
				printf("[MOCK] ERROR: Screenshot capture failed: %s\n", reinterpret_cast<const char*>(path.u8string().c_str()));
				failed = quit_requested = true;
			}
			const auto& draw_data = *ImGui::GetDrawData();
			write_text(std::filesystem::path(path) += ".ui.txt", draw_data);
			write_textures(std::filesystem::path(path) += ".textures.txt", draw_data);
			write_windows(std::filesystem::path(path) += ".windows.txt");
		}
		captures.clear();
		return quit_requested || next == events.size();
	}

private:
	enum class Action
	{
		Move,
		Down,
		Up,
		Wheel,
		WheelX,
		Escape,
		Scale,
		Theme,
		Color,
		Texture,
		Capture,
		Quit
	};

	struct Event
	{
		long long time = 0;
		Action action = Action::Quit;
		ImVec2 position;
		int button = 0;
		float value = 0.f;
		int style_color = 0;
		ImVec4 color;
		std::string text; // theme preset or texture identifier
		std::filesystem::path file;
	};

	static constexpr std::pair<std::string_view, Action> Actions[] = { { "move", Action::Move }, { "down", Action::Down }, { "up", Action::Up }, { "wheel", Action::Wheel }, { "wheel_x", Action::WheelX }, { "escape", Action::Escape }, { "scale", Action::Scale }, { "theme", Action::Theme },
		{ "color", Action::Color }, { "texture", Action::Texture }, { "capture", Action::Capture }, { "quit", Action::Quit } };

	std::vector<Event> events;
	size_t next = 0;
	std::chrono::steady_clock::time_point started;
	std::filesystem::path directory;
	std::vector<std::filesystem::path> captures;
	ImVec2 mouse = ImVec2(-FLT_MAX, -FLT_MAX);
	bool buttons[5]{};
	bool escape_down = false;
	bool enabled = false;
	bool quit_requested = false;

	// returns an error, or an empty string
	static std::string parse(std::istringstream& stream, Event& event)
	{
		std::string name;
		if (!(stream >> event.time >> name) || event.time < 0)
			return "expected milliseconds and an action";
		const auto* found = std::ranges::find(Actions, name, &std::pair<std::string_view, Action>::first);
		if (found == std::end(Actions))
			return "unknown action " + name;
		event.action = found->second;

		auto finite = [](float value) { return std::isfinite(value); };
		switch (event.action)
		{
		case Action::Move:
			if (!(stream >> event.position.x >> event.position.y) || !finite(event.position.x) || !finite(event.position.y))
				return "move needs x and y";
			break;
		case Action::Down:
		case Action::Up:
			if (!(stream >> event.button) || event.button < 0 || event.button >= IM_ARRAYSIZE(buttons))
				return "the mouse button must be 0 to 4";
			break;
		case Action::Wheel:
		case Action::WheelX:
			if (!(stream >> event.value) || !(std::abs(event.value) <= 100.f))
				return "the wheel needs a delta from -100 to 100";
			break;
		case Action::Scale:
			if (!(stream >> event.value) || !finite(event.value) || event.value <= 0.f)
				return "the scale must be positive";
			break;
		case Action::Theme:
			if (!(stream >> event.text) || (event.text != "dark" && event.text != "light" && event.text != "classic"))
				return "the theme must be dark, light or classic";
			break;
		case Action::Color:
		{
			auto& color = event.color;
			auto component = [](float value) { return value >= 0.f && value <= 1.f; };
			if (!(stream >> event.text >> color.x >> color.y >> color.z >> color.w) || !component(color.x) || !component(color.y) || !component(color.z) || !component(color.w))
				return "color needs a style color name and four components from 0 to 1";
			while (event.style_color < ImGuiCol_COUNT && event.text != ImGui::GetStyleColorName(event.style_color))
				++event.style_color;
			if (event.style_color == ImGuiCol_COUNT)
				return "unknown style color " + event.text;
			break;
		}
		case Action::Texture:
		{
			// no escape character, so Windows paths keep their backslashes
			std::string file;
			if (!(stream >> std::quoted(event.text, '"', '\n') >> std::quoted(file, '"', '\n')) || event.text.empty() || file.empty())
				return "texture needs a quoted identifier and file";
			event.file = std::u8string(file.begin(), file.end());
			break;
		}
		case Action::Capture:
		{
			std::string file;
			std::getline(stream >> std::ws, file);
			if (file.empty())
				return "capture needs a file name";
			event.file = std::u8string(file.begin(), file.end());
			break;
		}
		case Action::Escape:
		case Action::Quit:
			break;
		}

		if (event.action != Action::Capture && !(stream >> std::ws).eof())
			return "unexpected text after " + name;
		return {};
	}

	// the input an action changes, as a bit
	static unsigned input_of(Action action, int button = 0)
	{
		switch (action)
		{
		case Action::Move:
			return 1u << 0;
		case Action::Wheel:
			return 1u << 1;
		case Action::WheelX:
			return 1u << 2;
		case Action::Escape:
			return 1u << 3;
		case Action::Down:
		case Action::Up:
			return 1u << (4 + button);
		default:
			return 0;
		}
	}

	static void send_key(HWND hwnd, WPARAM key, bool down)
	{
		const LPARAM scan_code = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC)) << 16;
		const LPARAM release = (1ll << 30) | (1ll << 31);
		SendMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, key, 1 | scan_code | (down ? 0 : release));
	}

	void send_mouse(HWND hwnd, UINT msg, WORD data = 0) const
	{
		static constexpr WORD Flags[] = { MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2 };
		WORD keys = 0;
		for (int i = 0; i < IM_ARRAYSIZE(buttons); ++i)
			if (buttons[i])
				keys |= Flags[i];
		// the position before the first move is outside the window
		POINT point{ std::lround(std::clamp(mouse.x, -32768.f, 32767.f)), std::lround(std::clamp(mouse.y, -32768.f, 32767.f)) };
		if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
			ClientToScreen(hwnd, &point);
		SendMessageW(hwnd, msg, MAKEWPARAM(keys, data), MAKELPARAM(point.x, point.y));
	}

	static std::string to_utf8(const std::wstring& text)
	{
		std::string result(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()), nullptr, nullptr);
		return result;
	}

	// Reads each draw list's text back from its glyph quads, so capturing changes nothing in the frame.
	static void write_text(const std::filesystem::path& path, const ImDrawData& draw_data)
	{
		const auto& atlas = *ImGui::GetIO().Fonts;
		struct Glyph
		{
			const ImFont* font;
			const ImFontGlyph* glyph;
		};
		std::map<std::pair<float, float>, Glyph> glyphs; // by the top left texture coordinate
		for (const auto* font : atlas.Fonts)
			for (const auto& glyph : font->Glyphs)
				if (glyph.Visible)
					glyphs.try_emplace({ glyph.U0, glyph.V0 }, Glyph{ font, &glyph });

		struct Character
		{
			float top; // of the line
			float pen;
			float advance;
			float space;
			wchar_t codepoint;
		};
		// characters drawn one after another on a line, which a jump back or a wide gap ends
		struct Run
		{
			float top;
			float left;
			float end;
			std::wstring text;
		};
		std::ofstream output(path);
		for (int list_index = 0; list_index < draw_data.CmdListsCount; ++list_index)
		{
			const auto& list = *draw_data.CmdLists[list_index];
			std::vector<Character> characters;
			for (const auto& command : list.CmdBuffer)
			{
				if (command.UserCallback || command.TextureId != atlas.TexID)
					continue;
				const auto* indices = list.IdxBuffer.Data + command.IdxOffset;
				const auto* vertices = list.VtxBuffer.Data + command.VtxOffset;
				for (unsigned i = 0; i + 6 <= command.ElemCount; i += 3)
				{
					// ImFont::RenderText draws each glyph as the triangles a, a+1, a+2 and a, a+2, a+3
					const unsigned a = indices[i];
					if (indices[i + 1] != a + 1 || indices[i + 2] != a + 2 || indices[i + 3] != a || indices[i + 4] != a + 2 || indices[i + 5] != a + 3)
						continue;
					const auto& top_left = vertices[a];
					const auto& bottom_right = vertices[a + 2];
					const auto found = glyphs.find({ top_left.uv.x, top_left.uv.y });
					if (found == glyphs.end() || bottom_right.uv.x != found->second.glyph->U1 || bottom_right.uv.y != found->second.glyph->V1)
						continue;
					i += 3;

					const auto& [font, glyph] = found->second;
					const float scale = (bottom_right.pos.y - top_left.pos.y) / (glyph->Y1 - glyph->Y0);
					const Character character{ top_left.pos.y - glyph->Y0 * scale, top_left.pos.x - glyph->X0 * scale, glyph->AdvanceX * scale, font->GetCharAdvance(L' ') * scale, static_cast<wchar_t>(glyph->Codepoint) };
					// outlines and shadows draw a character again next to itself, and the last copy is the text
					const auto copy = std::find_if(characters.rbegin(), characters.rend(), [&](const Character& other) { return other.codepoint == character.codepoint && std::abs(other.top - character.top) <= 2.f && std::abs(other.pen - character.pen) <= 2.f; });
					if (copy != characters.rend())
						*copy = character;
					else
						characters.push_back(character);
				}
			}

			std::vector<Run> runs;
			for (const auto& character : characters)
			{
				if (runs.empty() || std::abs(runs.back().top - character.top) > 0.5f || character.pen < runs.back().end - 0.5f || character.pen > runs.back().end + character.space * 3.f)
					runs.push_back({ character.top, character.pen, character.pen, {} });
				auto& run = runs.back();
				if (character.pen - run.end > character.space * 0.5f)
					run.text += L' ';
				run.text += character.codepoint;
				run.end = character.pen + character.advance;
			}
			if (runs.empty())
				continue;

			// lines from top to bottom, with their runs from left to right
			std::ranges::sort(runs, {}, [](const Run& run) { return std::pair(std::lround(run.top), run.left); });
			output << '[' << list._OwnerName << ']';
			long line = LONG_MIN;
			for (const auto& run : runs)
			{
				output << (std::lround(run.top) == line ? " " : "\n") << to_utf8(run.text);
				line = std::lround(run.top);
			}
			output << '\n';
		}
	}

	// the loaded textures used by draw commands
	static void write_textures(const std::filesystem::path& path, const ImDrawData& draw_data)
	{
		const auto textures = texture_resources();
		std::ofstream output(path);
		for (int list = 0; list < draw_data.CmdListsCount; ++list)
			for (const auto& command : draw_data.CmdLists[list]->CmdBuffer)
				if (command.ElemCount > 0 && !command.UserCallback)
					for (const auto& [id, resource] : textures)
						if (resource == command.TextureId)
							output << id << '\t' << command.ElemCount << '\t' << command.ClipRect.x << ',' << command.ClipRect.y << ',' << command.ClipRect.z << ',' << command.ClipRect.w << '\n';
	}

	// the mouse, the hovered window and each visible window's geometry
	static void write_windows(const std::filesystem::path& path)
	{
		const auto& context = *ImGui::GetCurrentContext();
		std::ofstream output(path);
		output << "mouse\t" << context.IO.MousePos.x << '\t' << context.IO.MousePos.y << '\n';
		output << "hovered\t" << (context.HoveredWindow ? context.HoveredWindow->Name : "none") << '\n';
		for (const auto* window : context.Windows)
			if (window->Active && !window->Hidden)
				output << window->Name << '\t' << window->Pos.x << '\t' << window->Pos.y << '\t' << window->Size.x << '\t' << window->Size.y << '\t' << window->Scroll.x << '\t' << window->Scroll.y << '\t' << window->ScrollMax.x << '\t' << window->ScrollMax.y << '\n';
	}

	static bool capture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swap_chain, const std::filesystem::path& path)
	{
		ID3D11Texture2D* source = nullptr;
		if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&source))))
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		ID3D11Texture2D* staging = nullptr;
		const auto created = device->CreateTexture2D(&desc, nullptr, &staging);
		if (FAILED(created))
		{
			source->Release();
			return false;
		}
		context->CopyResource(staging, source);
		source->Release();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
		{
			staging->Release();
			return false;
		}
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
