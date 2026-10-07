# Nexus Mock

## Building

```powershell
.\build.ps1                  # Debug x64 (default)
.\build.ps1 -Config Release  # Release x64
```

Output: `build\x64\Debug\nexus_mock.exe`

## Running

```shell
nexus_mock.exe <path-to-addon.dll>
```

The mock creates a `data\` directory in its working directory for simulated game and addon paths. Use a separate working directory to isolate test settings and saved data.

Useful launch flags:

```shell
nexus_mock.exe <path-to-addon.dll> --invoke-keybind "<keybind id>" --hide-controls
```

`--invoke-keybind` invokes a keybind the addon registered once it is loaded, for example to open its window. `--hide-controls` hides the mock control panel for clean screenshots.

For font previews, `--font-file <path.ttf>` loads a 15-pixel body font and a 24-pixel heading font. `--heading-font-file <path.ttf>` optionally selects a separate heading face. These populate the shared Nexus font pointers; they do not change the addon or game settings. Without either flag, the body keeps ImGui's default font and `FontBig` uses a separate 24-pixel font.

Current Nexus uses Inter for its default body and Menomonia for its shared large heading. To preview those faces, supply local copies of the [upstream Nexus fonts](https://github.com/RaidcoreGG/Nexus/tree/main/Nexus/res/Fonts):

```shell
nexus_mock.exe <path-to-addon.dll> --font-file "fonts/Inter.ttf" --heading-font-file "fonts/Menomonia.ttf"
```

The mock keeps `Font` and `FontUI` equal to the chosen body font. Nexus itself uses separate Menomonia and Fira Sans fonts for those fields, so this preview matches the default body and heading rather than every host font configuration. Font files are optional and are not downloaded automatically.

## Scripted verification

`--test-script <path>` replays timed input and captures the rendered DirectX frames. It is intended for repeatable addon tests; normal launches do not synthesize input.

Each line starts with milliseconds since addon load, followed by an action. Times must be ordered. Mouse coordinates are relative to the window client area; buttons are 0 (left), 1 (right), or 2 (middle).

```text
0 texture "example-icon" "cached-icon.png"
1000 move 200 100
1100 down 1
1200 up 1
1800 capture context-menu.bmp
2200 escape
2500 scale 1.5
3000 capture scaled.bmp
3500 quit
```

Actions: `texture "identifier" "local-file"`, `move x y`, `down button`, `up button`, `wheel delta`, `wheel_x delta`, `scale factor`, `theme dark|light|classic`, `color native-name r g b a`, `escape`, `capture filename.bmp`, and `quit`. Captures are saved next to the script, with `.ui.txt` rendered text and `.windows.txt` geometry sidecars, plus `.textures.txt` listing registered textures used in draw commands. Texture preloads read local image files relative to the script; they do not download images. The text capture intercepts the frame's clipboard logging callback without changing the system clipboard. `quit` unloads the addon normally so persistence and shutdown can be checked.

Blank lines and comments beginning with `#` are ignored. Invalid numeric input, nonpositive scaling, and out-of-range mouse buttons terminate the scenario with a nonzero exit code.

Theme events use native ImGui presets. Color events override one native style color (for example, `color PopupBg 0.13 0.07 0.22 1`); each component must be between 0 and 1. These changes affect only the current mock process and let the same scenario check addon behavior under different host themes.
