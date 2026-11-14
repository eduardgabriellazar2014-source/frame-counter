# Frame Counter (Geode mod for Geometry Dash)

Shows the current frame of the level while playing.

## Before building
1. Open `mod.json` and change `"id"` (`yourname.frame-counter`) and `"developer"` to your own name.
2. Install the Geode SDK + CLI: https://docs.geode-sdk.org/getting-started/

## Build locally
```
geode sdk install        # first time only
geode sdk install-binaries
geode build
```
The resulting `.geode` file is placed in `build/`. Drop it into your GD `geode/mods` folder (or run `geode package install <file>`).

## Build with GitHub Actions
Push this folder to a GitHub repo. The workflow in `.github/workflows` builds for Windows, macOS and Android and uploads the `.geode` files as an artifact.

## Files
- `mod.json` - mod metadata and settings
- `CMakeLists.txt` - build config
- `src/main.cpp` - the mod code (hooks `PlayLayer`)
- `logo.png` - mod icon
- `about.md`, `changelog.md` - shown in the in-game mod menu
