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

The mock creates a `data\` directory next to the executable for simulated game and addon paths.

## Testing log loading

On startup the mock bootstraps a stub `data\arcdps\arcdps.ini` pointing at `data\` and creates `data\arcdps.cbtlogs\`. This is the directory the addon's directory monitor watches.

To trigger log loading, rename or copy a `.zevtc` or `.evtc` file into `data\arcdps.cbtlogs\`. The monitor detects the rename and hands the file to the log manager for parsing and display.

If `data\arcdps\arcdps.ini` already exists when the mock starts it is left untouched, so you can point it at a real arcdps log directory instead.
