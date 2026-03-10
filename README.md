# DS4 Spotify Controller

> Skip Spotify tracks with your controller. No API, no login.

---

### Download

Grab the latest `.exe` from [Releases](../../releases)

---

### Features

- DS4, DualSense, and Xbox One support
- Swap controllers without restarting
- Bindings persist across restarts
- Minimizes to system tray
- Zero performance impact

---

### Build from source

Requires vcpkg + SDL2 static + Dear ImGui in `third_party/imgui/`.

```cmd
cmake -S . -B build \
  -DSDL2_DIR=C:\path\to\vcpkg\packages\sdl2_x64-windows-static\share\sdl2 \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake \
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
```

---

### Usage

1. Launch Spotify
2. Launch the app and connect your controller
3. Click **Bind**, press a button

Bindings are saved to `%APPDATA%\DS4SpotifyController\binds.cfg` — delete to reset.
