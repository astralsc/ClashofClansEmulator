# Clash of Clans Emulator

[![Android Build](https://github.com/VitalikObject/SpookyPopEmulator/actions/workflows/android.yml/badge.svg)](https://github.com/VitalikObject/SpookyPopEmulator/actions/workflows/android.yml)

An open-source emulator for the iOS version of "Clash of Clans" (a game by Supercell), allowing it to run natively on Android, macOS, Linux, and Windows.

This emulator translates iOS instructions and shims iOS frameworks (like UIKit, Foundation, CoreGraphics, etc.) to allow the game to run on modern platforms. It uses [Dynarmic](https://github.com/lioncash/dynarmic.git) for dynamic binary translation.

## Features

* **Cross-Platform**: Designed to run on Android, macOS, Linux, and Windows.
* **JIT Compilation**: Fast execution utilizing the Dynarmic JIT engine.
* **iOS Framework Shimming**: Emulates a subset of iOS APIs required by the game.

## Requirements

* CMake 3.21+
* C++20 compatible compiler
* Android NDK (for Android builds)
* macOS with Xcode Command Line Tools and Homebrew (for macOS builds)
* Linux with build tools (for Linux builds)
* Windows 10/11, Visual Studio 2022 C++ tools, CMake, and vcpkg (for Windows builds)

## Game Assets

Before building or running the emulator, you must provide the original game files. Extract the decrypted iOS application and place the `Clash of Clans` executable and the `res/` folder directly into the `external/` directory (next to the `.pastehere` file).

## Build Instructions

### macOS Installation

Install Xcode Command Line Tools and Homebrew, then install CMake and Boost:

```bash
xcode-select --install
brew install cmake boost

# From the repository root, fetch Dynarmic and its submodules
git submodule update --init --recursive

# Download SQLite amalgamation and stb_truetype.h if not already present
mkdir -p extern/sqlite3 extern/stb
if [ ! -f extern/sqlite3/sqlite3.c ]; then
  curl -L https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip -o /tmp/clashofclans-sqlite.zip
  unzip -oq /tmp/clashofclans-sqlite.zip -d /tmp/clashofclans-sqlite
  cp /tmp/clashofclans-sqlite/sqlite-amalgamation-3500400/sqlite3.c \
    /tmp/clashofclans-sqlite/sqlite-amalgamation-3500400/sqlite3.h \
    /tmp/clashofclans-sqlite/sqlite-amalgamation-3500400/sqlite3ext.h extern/sqlite3/
  rm -rf /tmp/clashofclans-sqlite /tmp/clashofclans-sqlite.zip
fi
if [ ! -f extern/stb/stb_truetype.h ]; then
  curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h \
    -o extern/stb/stb_truetype.h
fi
```

### Linux Installation

```bash
# For Ubuntu: sudo apt install build-essential cmake git curl unzip zlib1g-dev libboost-all-dev libegl1-mesa-dev libgles2-mesa-dev libx11-dev
# For Fedora: sudo dnf install gcc gcc-c++ make cmake git curl unzip zlib-devel boost-devel mesa-libEGL-devel mesa-libGLES-devel libX11-devel

# Clone Dynarmic
rm -rf extern/dynarmic
git clone https://github.com/lioncash/dynarmic.git extern/dynarmic

# Download SQLite 3.50.4
rm -rf extern/sqlite3
mkdir -p extern/sqlite3
cd extern/sqlite3

curl -L https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip -o sqlite.zip
unzip sqlite.zip

mv sqlite-amalgamation-3500400/sqlite3.c .
mv sqlite-amalgamation-3500400/sqlite3.h .
mv sqlite-amalgamation-3500400/sqlite3ext.h .

rm -rf sqlite-amalgamation-3500400 sqlite.zip

cd ../..
rm -rf extern/stb
git clone https://github.com/nothings/stb.git extern/stb

# Build
cmake -S . -B build
cd build
cmake ..
cmake --build . -j$(nproc)
```

### Windows Installation

Install Visual Studio 2022 with the **Desktop development with C++** workload,
Git, CMake 3.21 or newer, and vcpkg. In a Visual Studio Developer PowerShell,
install the required third-party packages:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

$env:VCPKG_ROOT = "C:\vcpkg" # Set this to your vcpkg installation directory
& "$env:VCPKG_ROOT\vcpkg.exe" install boost:x64-windows zlib:x64-windows

# From the repository root, fetch Dynarmic and its submodules
if (Test-Path .git) {
    git submodule update --init --recursive
} elseif (-not (Test-Path extern\dynarmic\CMakeLists.txt)) {
    New-Item -ItemType Directory -Force extern | Out-Null
    git clone --recursive https://github.com/lioncash/dynarmic.git extern\dynarmic
}

# Download SQLite amalgamation if it is not already present
$sqliteZip = "$env:TEMP\sqlite-amalgamation.zip"
Invoke-WebRequest "https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip" -OutFile $sqliteZip
Expand-Archive $sqliteZip -DestinationPath "$env:TEMP\clashofclans-sqlite" -Force
New-Item -ItemType Directory -Force extern\sqlite3 | Out-Null
Copy-Item "$env:TEMP\clashofclans-sqlite\sqlite-amalgamation-3500400\sqlite3.*" extern\sqlite3\
Remove-Item $sqliteZip
Remove-Item "$env:TEMP\clashofclans-sqlite" -Recurse -Force

# Fetch stb_truetype.h
New-Item -ItemType Directory -Force extern\stb | Out-Null
Invoke-WebRequest "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" -OutFile extern\stb\stb_truetype.h
```

### App Building

#### macOS

Build a native executable for the current Mac architecture (Apple Silicon or
Intel) from the repository root:

```bash
bash ./scripts/build_macos.sh
./build-macos/clashofclans_loader --binary "external/Clash of Clans" --external external
```

The macOS backend uses Cocoa and the system OpenGL framework.
The script accepts `BUILD_DIR`, `BUILD_TYPE`, and `BUILD_JOBS` environment
variables to customize the build directory, CMake configuration, and parallel
job count.

#### Android

An automated build script is provided to generate the APK. The following
commands install the Android SDK command-line tools and required SDK
components, then build the Android ARM64 library and APK.

```bash
# Android SDK
mkdir -p ~/Android/Sdk/cmdline-tools
cd ~/Android/Sdk

curl -L https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip -o cmdline-tools.zip
unzip -o cmdline-tools.zip

mkdir -p cmdline-tools/latest
mv cmdline-tools/bin cmdline-tools/lib cmdline-tools/latest/ 2>/dev/null || true
mv cmdline-tools/NOTICE.txt cmdline-tools/source.properties cmdline-tools/latest/ 2>/dev/null || true
rm -f cmdline-tools.zip

# Android environment
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export PATH="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:$ANDROID_SDK_ROOT/platform-tools:$PATH"

sdkmanager "ndk;27.2.12479018"

export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.2.12479018"

cd ~/ClashofClansEmulator
./scripts/build_android_apk.sh
```

#### Linux

Run the build script from the repository root after preparing the dependencies
above. It builds a native Linux executable for the current machine architecture
(including x86-64 or ARM64).

```bash
sed -i 's/\r$//' scripts/build_linux.sh
bash ./scripts/build_linux.sh
./build-linux/clashofclans_loader --binary "external/Clash of Clans" --external external
```

#### Windows

Run the build script from a Visual Studio Developer PowerShell after preparing
the dependencies above. It builds a native Windows executable. The WGL graphics
backend opens a desktop window and uses the system OpenGL implementation.

```powershell
.\scripts\build_windows.ps1
.\build-windows\Release\clashofclans_loader.exe --binary "external\Clash of Clans" --external external
```

If you changed `BUILD_TYPE`, replace `Release` in the executable path with that
configuration. The build script also prints the exact executable path when it
finishes.

The Windows build script accepts `BUILD_DIR`, `BUILD_TYPE`, and `BUILD_JOBS`
environment variables to customize the build directory, CMake configuration,
and parallel job count.

Linux graphics use EGL, OpenGL ES 2, and an X11 display (XWayland works for
Wayland desktops). On Debian/Ubuntu, install `libegl1-mesa-dev`,
`libgles2-mesa-dev`, and `libx11-dev` in addition to the build dependencies.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).

## Disclaimer

This project is not affiliated with, endorsed, sponsored, or specifically approved by Supercell Oy. "Clash of Clans" and other related trademarks are the property of Supercell Oy. This is a non-commercial, educational fan project created for preservation purposes.
