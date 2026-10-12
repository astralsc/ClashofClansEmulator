$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $root "build-windows" }
$buildType = if ($env:BUILD_TYPE) { $env:BUILD_TYPE } else { "Release" }
$buildJobs = if ($env:BUILD_JOBS) { $env:BUILD_JOBS } else { [Environment]::ProcessorCount }

foreach ($tool in @("cmake", "cl")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$tool' was not found. Run this script from a Visual Studio Developer PowerShell with CMake installed."
    }
}

$requiredFiles = @(
    (Join-Path $root "extern\dynarmic\CMakeLists.txt"),
    (Join-Path $root "extern\sqlite3\sqlite3.c"),
    (Join-Path $root "extern\stb\stb_truetype.h")
)
foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file -PathType Leaf)) {
        throw "Required dependency is missing: $file. Prepare dependencies as described in the README, then rerun this script."
    }
}

if ($buildJobs -notmatch '^[1-9][0-9]*$') {
    throw "BUILD_JOBS must be a positive integer; received '$buildJobs'."
}

$cmakeArgs = @()
if ($env:VCPKG_ROOT) {
    $toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain -PathType Leaf)) {
        throw "VCPKG_ROOT is set but its CMake toolchain file was not found: $toolchain"
    }
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    $boostInclude = Join-Path $env:VCPKG_ROOT "installed\x64-windows\include"
    if (Test-Path $boostInclude -PathType Container) {
        $cmakeArgs += "-DBoost_INCLUDE_DIR=$boostInclude"
    }
}

cmake -S $root -B $buildDir "-DCMAKE_BUILD_TYPE=$buildType" @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

cmake --build $buildDir --config $buildType --parallel $buildJobs
if ($LASTEXITCODE -ne 0) {
    throw "Windows build failed with exit code $LASTEXITCODE."
}

$executable = Join-Path $buildDir "clashofclans_loader.exe"
if (-not (Test-Path $executable -PathType Leaf)) {
    $executable = Join-Path (Join-Path $buildDir $buildType) "clashofclans_loader.exe"
}
if (-not (Test-Path $executable -PathType Leaf)) {
    throw "Build completed, but the executable was not found under '$buildDir'."
}

Write-Output "Windows executable: $executable"
