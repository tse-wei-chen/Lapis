#requires -Version 5.1
<#
.SYNOPSIS
Builds the Lapis Windows player with real libbluray-backed BDMV support.
.DESCRIPTION
Uses an isolated build/native-bdmv CMake tree and publishes an isolated
build/Lapis.BdmvPlayer desktop folder. Nothing is copied into System32 and
the ordinary FFmpeg-only build/native tree is left untouched.
This is NOT an AACS/BD+ decoder and does NOT enable BD-J menus.
.EXAMPLE
.\scripts\build-bdmv-player.ps1
.EXAMPLE
.\scripts\build-bdmv-player.ps1 -VcpkgRoot C:\vcpkg -Run
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SkipTests,
    [switch]$Run
)

$ErrorActionPreference = "Stop"
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "This BDMV build script currently targets Windows x64."
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $command = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $VcpkgRoot = Split-Path -Parent $command.Source
    }
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $nativeCache = Join-Path $root "build/native/CMakeCache.txt"
    if (Test-Path -LiteralPath $nativeCache -PathType Leaf) {
        $cachedRoot = Select-String -LiteralPath $nativeCache `
            -Pattern "^Z_VCPKG_ROOT_DIR:INTERNAL=(.+)$" |
            Select-Object -First 1
        if ($null -ne $cachedRoot) {
            $candidateRoot = $cachedRoot.Matches[0].Groups[1].Value.Trim()
            $candidateToolchain = Join-Path $candidateRoot "scripts/buildsystems/vcpkg.cmake"
            if (Test-Path -LiteralPath $candidateToolchain -PathType Leaf) {
                $VcpkgRoot = $candidateRoot
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Could not locate vcpkg. Set VCPKG_ROOT or provide -VcpkgRoot <path>."
}
$toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "Could not find the vcpkg CMake toolchain at: $toolchain"
}
$nativeBuild = Join-Path $root "build/native-bdmv"
$nativeOutput = Join-Path $nativeBuild "Release"
$nativeDependencies = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "lapis-vcpkg-installed-bdmv"
$published = Join-Path $root "build/Lapis.BdmvPlayer"
$triplets = Join-Path $root "native/MediaRuntime/vcpkg-triplets"
$nativeSource = Join-Path $root "native/MediaRuntime"
$appProject = Join-Path $root "src/Lapis.Desktop.Avalonia/Lapis.Desktop.Avalonia.csproj"

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable exited with code $LASTEXITCODE."
    }
}

Write-Host "Configuring the opt-in unprotected BDMV adapter..."
Invoke-Checked "cmake" @(
    "-S", $nativeSource, "-B", $nativeBuild, "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DVCPKG_INSTALLED_DIR=$nativeDependencies",
    "-DVCPKG_OVERLAY_TRIPLETS=$triplets",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-release",
    "-DVCPKG_MANIFEST_FEATURES=bluray",
    "-DLAPIS_ENABLE_LIBBLURAY=ON"
)
$cache = Join-Path $nativeBuild "CMakeCache.txt"
if (-not (Select-String -LiteralPath $cache -Pattern "^LAPIS_ENABLE_LIBBLURAY:BOOL=ON$" -Quiet)) {
    throw "The Native CMake cache did not enable LAPIS_ENABLE_LIBBLURAY."
}

Invoke-Checked "cmake" @(
    "--build", $nativeBuild, "--config", "Release",
    "--target", "Lapis.MediaRuntime", "Lapis.MediaRuntime.BluraySourceTests",
    "Lapis.MediaRuntime.SourceFeatureTests"
)
if (-not $SkipTests) {
    Invoke-Checked "ctest" @(
        "--test-dir", $nativeBuild, "-C", "Release",
        "-R", "^(bluray_source_contract|source_features_contract)$",
        "--output-on-failure"
    )
}

$runtimeDll = Join-Path $nativeOutput "Lapis.MediaRuntime.dll"
if (-not (Test-Path -LiteralPath $runtimeDll -PathType Leaf)) {
    throw "Native Runtime DLL was not generated at $runtimeDll"
}
$nativeDlls = @(Get-ChildItem -LiteralPath $nativeOutput -File -Filter "*.dll")
if (-not @($nativeDlls | Where-Object { $_.Name -match "bluray" }).Count) {
    throw "The opt-in Native output has no app-local libbluray DLL."
}

# Use a dedicated output directory to prevent stale FFmpeg-only DLLs from
# being picked up, even when timestamps differ between source variants.
if (Test-Path -LiteralPath $published) {
    Remove-Item -LiteralPath $published -Recurse -Force
}
Invoke-Checked "dotnet" @(
    "publish", $appProject, "-c", "Release", "-r", "win-x64",
    "--self-contained", "false", "-o", $published,
    "-p:LapisNativeRuntimeDir=$nativeOutput"
)
foreach ($dll in $nativeDlls) {
    if (-not (Test-Path -LiteralPath (Join-Path $published $dll.Name) -PathType Leaf)) {
        throw "Missing published Native dependency: $($dll.Name)"
    }
}
$player = Join-Path $published "Lapis.Desktop.Avalonia.exe"
if (-not (Test-Path -LiteralPath $player -PathType Leaf)) {
    throw "Published player was not found: $player"
}
Write-Host ""
Write-Host "BDMV-enabled Lapis player published to: $player"
Write-Host "Import the OUTER disc folder, or its BDMV child, as one library item."
Write-Host "Do not expect protected AACS/BD+ content or BD-J menus to work."
if ($Run) {
    & $player
}
