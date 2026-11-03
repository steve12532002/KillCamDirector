# KillCam Director

SKSE plugin that films every killmove the player takes part in (as killer or victim) like a movie:
camera angles that move and cut, adjustable letterbox bars, optional slow motion, hotkey on/off,
and presets you can edit as text files. Works with any killmove mod because the camera follows the
actors' real skeleton, not the animation's start point.

## 中文說明

- **熱鍵**：`Insert` 開關、`Page Up` 切換 preset（可在 `KillCamDirector.ini` 改）。
- **遊戲內設定**：裝了 SKSE Menu Framework 就會有「KillCam Director」頁面，可開關、選 preset、調黑邊、慢動作。黑邊需要 SKSE Menu Framework 才會顯示。
- **Preset**：`SKSE/Plugins/KillCamDirector/Presets/*.ini`，每個檔案是一組鏡位（Shot1、Shot2…），數值說明寫在檔案註解裡，可以直接複製一份改。
- **慢動作**：建議把 VioLens 的 Camera Setting 設成「3P SlowMo Off」，避免兩邊同時改時間速度。
- **鏡頭方向不對**（例如看向天空或背對角色）：把 ini 的 `iAxisMode` 改成 0 或 1 試試，並回報 `Documents/My Games/Skyrim Special Edition/SKSE/KillCamDirector.log`。

## Requirements

SKSE64, Address Library for SKSE Plugins. Optional: SKSE Menu Framework (settings page + letterbox).

## Building

Built by GitHub Actions (`.github/workflows/build.yml`) on windows-latest with MSVC, vcpkg and
CommonLibSSE-NG (alandtse fork, v14.0.1). Locally: VS 2022, CMake 3.24+, vcpkg, then

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build
```

## License

GPL-3.0-or-later (CommonLibSSE-NG is GPL-3.0 with modding exceptions). `include/SKSEMenuFramework.h`
is from SKSE Menu Framework (MIT, see `include/SKSEMenuFramework.LICENSE`).
