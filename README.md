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

## macOS native addon mock

When this repository is used as the `nexus-mock` submodule of `nexus-log-uploader`, it can build a native macOS host for UI and basic workflow development. This does not produce a Windows addon DLL.

```sh
brew install cmake ninja
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake --preset mac-mock-debug
cmake --build --preset mac-mock-debug
../build/mac-mock-debug/nexus_log_uploader_mac_mock
```

The mac host links the addon sources directly, uses local mock parser/upload results, and never uploads to dps.report or Wingman.
