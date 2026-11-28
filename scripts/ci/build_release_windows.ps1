<#
.SYNOPSIS
    Builds the Windows release of Scenery Editor X: the Release editor, its NSIS setup, and the setup's SHA-256.

.DESCRIPTION
    What the release workflow (.github/workflows/release.yml) runs on its Windows runner; it runs the same way on a
    developer machine with Visual Studio, CMake, NSIS and the Vulkan SDK (VULKAN_SDK set).

      1. Installs the vcpkg ports (vcpkg.json) into <BuildDir>\vcpkg_installed, unless -SkipBuild.
      2. Configures <BuildDir> (Visual Studio generator, x64-windows, automatic version increments off, so the editor
         carries exactly the version in source\resource.h) and builds the SceneryEditorX target in Release.
      3. Checks that bin\SceneryEditorX.exe has the version source\resource.h gives.
      4. Packages it with installer\windows\Build-Installer.ps1 (LunarG's Vulkan Runtime included when the SDK has it).
      5. Starts the staged editor with --check-vulkan: any of its answers (Vulkan present, missing, too old) shows the
         staged files load; a missing or broken DLL stops the build.
      6. Writes <setup>.sha256 and SHA256SUMS.txt (sha256sum format) beside the setup. The editor's update check
         verifies its download against them (or GitHub's own digest of the file).

    Under GitHub Actions the setup's path and the version go to $GITHUB_OUTPUT (installer, version).

.PARAMETER BuildDir
    The CMake build folder, relative to the repository root. Default: build.

.PARAMETER OutputDir
    Where the stage folder and the setup go, relative to the repository root. Default: build\installer.

.PARAMETER Channel
    The release channel in the setup's name and pages. Default: Alpha.

.PARAMETER Generator
    The CMake generator for a new build folder. Default: the newest Visual Studio vswhere finds. An existing build
    folder keeps its own.

.PARAMETER VcpkgBuildtrees
    A short folder for vcpkg's build trees (port builds can pass Windows' 260-character path limit under a long
    checkout path). Default: vcpkg's own.

.PARAMETER VcpkgInstalledDir
    Where the vcpkg ports are installed. Default: <BuildDir>\vcpkg_installed. A worktree can point at the main
    checkout's (with -SkipVcpkg) instead of building the ports again.

.PARAMETER VcpkgOnly
    Only installs the vcpkg ports (step 1), so CI can cache them before the long build.

.PARAMETER SkipVcpkg
    Starts at the configure: the ports are installed already (a -VcpkgOnly run).

.PARAMETER SkipBuild
    Packages the Release editor already in bin\ (no vcpkg, configure or build).

.EXAMPLE
    pwsh scripts\ci\build_release_windows.ps1 -VcpkgBuildtrees C:\vb
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$OutputDir = 'build\installer',
    [string]$Channel = 'Alpha',
    [string]$Generator = '',
    [string]$VcpkgBuildtrees = '',
    [string]$VcpkgInstalledDir = '',
    [switch]$VcpkgOnly,
    [switch]$SkipVcpkg,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$BuildPath = Join-Path $Root $BuildDir
$OutputPath = Join-Path $Root $OutputDir
$Triplet = 'x64-windows'
$VcpkgInstalled = if ($VcpkgInstalledDir) { $VcpkgInstalledDir } else { Join-Path $BuildPath 'vcpkg_installed' }

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-CiWarning([string]$Text) {
    if ($env:GITHUB_ACTIONS -eq 'true') { Write-Host "::warning title=Windows release::$Text" } else { Write-Warning $Text }
}
function Invoke-Checked([string]$What, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)." }
}

# The version ---------------------------------------------------

$Header = Get-Content (Join-Path $Root 'source\resource.h') -Raw
if ($Header -notmatch '#define\s+SEDX_VERSION_STRING\s+"(\d+\.\d+\.\d+\.\d+)"') { throw 'source\resource.h has no SEDX_VERSION_STRING.' }
$Version = $Matches[1]
Write-Step "Scenery Editor X $Version ($Channel)"

if (-not $SkipBuild -and -not $SkipVcpkg) {
    # vcpkg ------------------------------------------------------

    $VcpkgRoot = Join-Path $Root 'dependency\vcpkg'
    $Vcpkg = Join-Path $VcpkgRoot 'vcpkg.exe'
    if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) { throw 'dependency\vcpkg is empty: fetch the submodules first.' }
    if (-not (Test-Path $Vcpkg)) {
        Write-Step 'Bootstrapping vcpkg'
        Invoke-Checked 'bootstrap-vcpkg' { & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics }
    }
    $env:VCPKG_DISABLE_METRICS = '1'
    $VcpkgArgs = @('install', "--triplet=$Triplet", "--x-manifest-root=$Root", "--x-install-root=$VcpkgInstalled", '--no-print-usage')
    if ($VcpkgBuildtrees) {
        New-Item -ItemType Directory -Path $VcpkgBuildtrees -Force | Out-Null
        $VcpkgArgs += "--x-buildtrees-root=$(Join-Path $VcpkgBuildtrees 'bt')"
        $VcpkgArgs += "--x-packages-root=$(Join-Path $VcpkgBuildtrees 'pk')"
    }
    Write-Step "Installing the vcpkg ports into $VcpkgInstalled"
    Invoke-Checked 'vcpkg install' { & $Vcpkg @VcpkgArgs }
    if ($VcpkgOnly) { return }
}

# The vcpkg ports don't need the Vulkan SDK (-VcpkgOnly runs without it); the editor and its setup do
if (-not $env:VULKAN_SDK -or -not (Test-Path $env:VULKAN_SDK)) { throw 'VULKAN_SDK is not set to an installed Vulkan SDK.' }
if (-not (Test-Path (Join-Path $env:VULKAN_SDK 'Lib\slang.lib'))) { throw "The Vulkan SDK at $env:VULKAN_SDK has no Lib\slang.lib (the editor links Slang)." }
# Optional SDK components the editor compiles against (GLM, Volk, Vulkan Memory Allocator): an SDK installed with only
# its defaults fails much later, at the first source that includes one
foreach ($header in 'glm\glm.hpp', 'Volk\volk.h', 'vma\vk_mem_alloc.h') {
    if (-not (Test-Path (Join-Path $env:VULKAN_SDK "Include\$header"))) {
        throw "The Vulkan SDK at $env:VULKAN_SDK has no Include\${header}: install its GLM, Volk and Vulkan Memory Allocator components (the SDK's maintenance tool, or the installer's ``install com.lunarg.vulkan.glm com.lunarg.vulkan.volk com.lunarg.vulkan.vma``)."
    }
}
Write-Host "    Vulkan SDK $env:VULKAN_SDK"

if (-not $SkipBuild) {
    # Configure and build -------------------------------------------

    $ConfigureArgs = @('-S', $Root, '-B', $BuildPath)
    if (-not (Test-Path (Join-Path $BuildPath 'CMakeCache.txt'))) {
        if (-not $Generator) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (-not (Test-Path $vswhere)) { throw 'vswhere.exe was not found: install Visual Studio with the C++ workload, or pass -Generator.' }
            $major = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
            $major = "$major".Split('.')[0]
            $Generator = switch ($major) {
                '17' { 'Visual Studio 17 2022' }
                '18' { 'Visual Studio 18 2026' }
                default { throw "No Visual Studio 2022 or 2026 with the C++ tools was found (vswhere: '$major')." }
            }
        }
        $ConfigureArgs += @('-G', $Generator, '-A', 'x64')
    }
    $ConfigureArgs += @(
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $Root 'cmake\toolchains\sedx-windows.cmake')",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        '-DVCPKG_MANIFEST_INSTALL=OFF',
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalled",
        '-DSEDX_ENABLE_VERSION_INCREMENT=OFF',
        '-DSEDX_ENABLE_TESTS=OFF'
    )
    Write-Step "Configuring $BuildPath$(if ($Generator) { " ($Generator)" })"
    Invoke-Checked 'CMake configure' { & cmake @ConfigureArgs }

    Write-Step 'Building the Release editor'
    Invoke-Checked 'The Release build' { & cmake --build $BuildPath --config Release --target SceneryEditorX --parallel }
}

# The executable's version ----------------------------------------------

$Exe = Join-Path $Root 'bin\SceneryEditorX.exe'
if (-not (Test-Path $Exe)) { throw "No Release editor at $Exe." }
$Info = (Get-Item $Exe).VersionInfo
$FileVersion = '{0}.{1}.{2}.{3}' -f $Info.FileMajorPart, $Info.FileMinorPart, $Info.FileBuildPart, $Info.FilePrivatePart
if ($FileVersion -ne $Version -or $Info.ProductVersion -ne $Version) {
    throw "bin\SceneryEditorX.exe is version $FileVersion ($($Info.ProductVersion)), not ${Version}: a stale build, or the version was bumped during it."
}
Write-Step "bin\SceneryEditorX.exe is $Version"

# The setup ---------------------------------------------------------------

$InstallerArgs = @{ BuildDir = $BuildDir; Channel = $Channel; OutputDir = $OutputDir }
if (-not (Test-Path (Join-Path $env:VULKAN_SDK 'Helpers\VulkanRT.exe'))) {
    Write-CiWarning "The Vulkan SDK has no Helpers\VulkanRT.exe: the setup can't offer the Vulkan Runtime to machines without one."
    $InstallerArgs.NoVulkanRuntime = $true
}
& (Join-Path $Root 'installer\windows\Build-Installer.ps1') @InstallerArgs

$SetupName = "SceneryEditorX-$Version-$Channel-Setup.exe"
$Setup = Join-Path $OutputPath $SetupName
if (-not (Test-Path $Setup)) { throw "The installer script made no $Setup." }

# Smoke test: the staged files load ------------------------------------------------

$StagedExe = Join-Path $OutputPath 'stage\SceneryEditorX.exe'
Write-Step 'Starting the staged editor with --check-vulkan'
$Process = Start-Process -FilePath $StagedExe -ArgumentList '--check-vulkan' -WorkingDirectory (Split-Path $StagedExe) -PassThru -WindowStyle Hidden
if (-not $Process.WaitForExit(120000)) {
    $Process.Kill()
    throw 'The staged editor did not finish --check-vulkan within two minutes.'
}
$Code = $Process.ExitCode
$Answers = @{ 0 = 'Vulkan 1.3 is available'; 2 = 'no Vulkan runtime'; 3 = 'the Vulkan runtime is older than 1.3'; 4 = 'no Vulkan driver'; 5 = 'the Vulkan driver is older than 1.3' }
if (-not $Answers.ContainsKey($Code)) {
    throw ('The staged editor did not start: --check-vulkan exited with {0} (0x{1:X8}). A DLL it needs is missing from the stage or does not load.' -f $Code, $Code)
}
Write-Host "    --check-vulkan answered $Code ($($Answers[$Code])): the staged files load"

# Checksums ------------------------------------------------------------------------

$Hash = (Get-FileHash $Setup -Algorithm SHA256).Hash.ToLowerInvariant()
$Line = "$Hash  $SetupName`n"
$Utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$Setup.sha256", $Line, $Utf8)
[IO.File]::WriteAllText((Join-Path $OutputPath 'SHA256SUMS.txt'), $Line, $Utf8)
Write-Step ("{0} ({1:N1} MB), SHA-256 {2}" -f $SetupName, ((Get-Item $Setup).Length / 1MB), $Hash)

if ($env:GITHUB_OUTPUT) {
    Add-Content -Path $env:GITHUB_OUTPUT -Value "installer=$Setup" -Encoding utf8
    Add-Content -Path $env:GITHUB_OUTPUT -Value "version=$Version" -Encoding utf8
}
