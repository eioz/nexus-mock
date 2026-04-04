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
