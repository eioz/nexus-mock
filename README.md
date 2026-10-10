# Nexus Mock

Loads a Nexus addon into a window with a Direct3D 11 swap chain and an ImGui 1.80 context, and implements version 6 of the Nexus API the way the current Nexus release behaves. It is meant for developing and testing addons without the game, and only approximates Nexus and the game.

## Building

```powershell
.\build.ps1                  # Debug x64 (default)
.\build.ps1 -Config Release  # Release x64
```

Output: `build\x64\Debug\nexus_mock.exe`

## Running

```shell
nexus_mock.exe <path-to-addon.dll> [options]
```

The `data\` folder in the working directory stands in for the game directory, so an addon gets `data\addons\<name>` like `<Guild Wars 2>\addons\<name>` in the game. The mock creates `data\addons` and `data\addons\common` like Nexus, and ImGui saves its window layout to `imgui.ini` outside scripted runs. Use a separate working directory to isolate settings and saved data.

| Option | |
|---|---|
| `--invoke-keybind <id>` | Presses and releases an input bind of the addon after it loaded, for example to open its window. |
| `--hide-controls` | Hides the mock controls for clean screenshots. |
| `--font-file <path>` | A TTF or OTF font for `FONT_DEFAULT`, the body text. |
| `--heading-font-file <path>` | A TTF or OTF font for the Menomonia fonts behind the Nexus link's `Font` and `FontBig`. |
| `--test-script <path>` | Replays timed input, captures frames and exits after the last event. |

Without font files the mock uses ImGui's default font. `FontUI` uses Trebuchet MS from Windows, like Nexus. Nexus itself uses Inter for the body and Menomonia for the headings; local copies from the [Nexus repository](https://github.com/RaidcoreGG/Nexus/tree/main/Nexus/res/Fonts) preview them:

```shell
nexus_mock.exe <path-to-addon.dll> --font-file "fonts/Inter.ttf" --heading-font-file "fonts/Menomonia.ttf"
```

## What the mock simulates

- A guardian standing in Lion's Arch in the Mumble link and the identity link. The mock controls toggle combat and the map, and resize the window to common sizes.
- The Nexus link with the window size, the scaling and the fonts for the normal UI size.
- The Nexus frame: textures, localized strings and fonts update first, PreRender callbacks run before the ImGui frame and Render callbacks inside it, followed by the alerts, the Nexus window and the quick access, and PostRender callbacks run after it.
- Nexus's own input binds: Ctrl+O toggles the Nexus window with the addon's options, and Ctrl+H hides the UI. The N shortcut in the quick access toggles the Nexus window too, and its context menu gets the items without a target.
- Input binds match keyboard scan codes with exact modifiers, and their handlers run on a worker thread. ImGui only gets keys while a text field is active.
- Escape closes the frontmost open window that is registered for close on escape.
- `EV_ADDON_LOADED` and `EV_MUMBLE_IDENTITY_UPDATED` after Load, `EV_WINDOW_RESIZED` after a resize and `EV_INPUTBIND_UPDATED` after a bind registration.
- After Unload, the mock removes what the addon left registered and warns about it before it frees the addon.

Game binds, MinHook and updates do nothing, since there is no game.

## Scripted runs

`--test-script <path>` replays timed input and captures the rendered frames for repeatable addon tests. Normal launches do not synthesize input.

Each line starts with the milliseconds since the addon loaded, followed by an action. Times must not decrease. Mouse coordinates are relative to the client area, and buttons are 0 (left), 1 (right), 2 (middle), 3 and 4. Buttons and the wheel go through the window procedure like real input, so ImGui only gets the left and right button and the wheel over its windows, and addon callbacks and mouse input binds see them too.

```text
0 texture "example-icon" "cached-icon.png"
1000 move 200 100
1100 down 1
1100 up 1
1800 capture context-menu.bmp
2200 escape
2500 scale 1.5
3000 capture scaled.bmp
```

| Action | |
|---|---|
| `move x y` | Moves the mouse. |
| `down button`, `up button` | Presses or releases a mouse button. |
| `wheel delta`, `wheel_x delta` | Turns the mouse wheel. |
| `escape` | Presses Escape through the window procedure, so close on escape and input binds see it, and releases it on the next frame. |
| `scale factor` | Sets the DPI scale, which Nexus keeps in `io.FontGlobalScale` and in the Nexus link scaling. |
| `theme dark\|light\|classic` | Applies an ImGui color preset. |
| `color name r g b a` | Overrides one ImGui style color, for example `color PopupBg 0.13 0.07 0.22 1`, with components from 0 to 1. |
| `texture "identifier" "file"` | Loads a local image under an identifier that is not loaded or loading yet, for example one the addon would download. |
| `capture file.bmp` | Saves the frame and three text files next to it. |
| `quit` | Ends the run before the last event. |

ImGui 1.80 reads its input once per frame, so when one input changes twice in a frame, like a press and a release at the same time, the second change waits for the next frame. A button or wheel event after a move waits as well, until ImGui knows whether the mouse is over one of its windows. Files are relative to the script. Blank lines and comments beginning with `#` are ignored.

A capture writes `.ui.txt` with the text of each window, read from the drawn glyphs, `.windows.txt` with the mouse, the hovered window and the position, size and scrolling of each visible window, and `.textures.txt` with the loaded textures that draw commands use. The run ends after the last event and unloads the addon normally, so persistence and shutdown can be checked. An invalid script, a failed preload or capture, or an `--invoke-keybind` without a matching bind ends it with a nonzero exit code.
