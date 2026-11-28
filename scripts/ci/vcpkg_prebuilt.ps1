<#
.SYNOPSIS
    Prebuilt vcpkg packages for CI: fetches the newest set the vcpkg-packages workflow made, or prunes a set to the
    packages one install uses and lists them.

.DESCRIPTION
    The vcpkg-packages workflow (.github/workflows/vcpkg-packages.yml) builds Scenery Editor X's vcpkg ports once and
    keeps them as a workflow artifact: vcpkg's "files" binary cache, one zip per package, named by the SHA-256 of the
    package's vcpkg_abi_info.txt (<cache>\<first two hex digits>\<hash>.zip). Builds point VCPKG_BINARY_SOURCES at that
    folder; vcpkg restores every package whose ABI still matches (same port version, features, triplet, compiler) and
    builds only the rest. docs/RELEASING.md and spec/spec-process-cicd-vcpkg-packages.md describe the whole scheme.

    -Fetch     Downloads the newest package set into -Destination. Only sets from trusted runs count: the workflow on
               a push to dev / release / master, its weekly schedule, or a manual run, and only runs on one of those
               branches (a manual run executes the workflow file of the branch it runs on, so one on any other branch
               could upload anything). Sets from pull request runs are never used, so a pull request can't hand a
               release its own binaries. With no set to fetch it warns and leaves -Destination empty: vcpkg then builds
               everything from source.
               Needs gh with GH_TOKEN (actions: read).

    -Prune     Deletes every package in -Cache that the install in -Installed doesn't use (older ABIs pile up as ports
               and compilers change) and writes manifest.json beside them: each package's port, version, ABI and size.

    Under GitHub Actions both write a short report to the run summary, and -Fetch writes run, branch and found to
    $GITHUB_OUTPUT.

.EXAMPLE
    pwsh scripts\ci\vcpkg_prebuilt.ps1 -Fetch -Destination C:\vcpkg-cache

.EXAMPLE
    pwsh scripts\ci\vcpkg_prebuilt.ps1 -Prune -Cache C:\vcpkg-cache -Installed build\vcpkg_installed
#>
[CmdletBinding(DefaultParameterSetName = 'Fetch')]
param(
    [Parameter(ParameterSetName = 'Fetch', Mandatory)][switch]$Fetch,
    [Parameter(ParameterSetName = 'Fetch', Mandatory)][string]$Destination,
    [Parameter(ParameterSetName = 'Fetch')][string]$Workflow = 'vcpkg-packages.yml',
    [Parameter(ParameterSetName = 'Fetch')][string]$Repository = $env:GITHUB_REPOSITORY,
    [Parameter(ParameterSetName = 'Fetch')][int]$Runs = 30,

    [Parameter(ParameterSetName = 'Prune', Mandatory)][switch]$Prune,
    [Parameter(ParameterSetName = 'Prune', Mandatory)][string]$Cache,
    [Parameter(ParameterSetName = 'Prune', Mandatory)][string]$Installed,

    [string]$ArtifactName = 'vcpkg-x64-windows',
    [string]$Triplet = 'x64-windows'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Runs whose package sets may be used: they need write access to start, and they ran the workflow file of a branch
# that only takes reviewed changes
$TrustedEvents = @('push', 'schedule', 'workflow_dispatch')
$TrustedBranches = @('dev', 'release', 'master')

function Write-Summary([string[]]$Lines) {
    if ($env:GITHUB_STEP_SUMMARY) { Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value $Lines -Encoding utf8 }
}
function Set-StepOutput([string]$Name, [string]$Value) {
    if ($env:GITHUB_OUTPUT) { Add-Content -Path $env:GITHUB_OUTPUT -Value "$Name=$Value" -Encoding utf8 }
}
function Get-Megabytes([object[]]$Files) {
    $bytes = 0
    foreach ($file in $Files) { $bytes += $file.Length }		# Measure-Object -Sum has no Sum for nothing, under strict mode
    return $bytes / 1MB
}
function Write-CiWarning([string]$Text) {
    if ($env:GITHUB_ACTIONS -eq 'true') { Write-Host "::warning title=Prebuilt vcpkg packages::$Text" } else { Write-Warning $Text }
}

if ($Fetch) {
    if (-not $Repository) { throw 'No repository: pass -Repository owner/name (GITHUB_REPOSITORY is set in Actions).' }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null

    $list = & gh run list --repo $Repository --workflow $Workflow --status success --limit $Runs --json databaseId,event,headBranch,createdAt,url 2>&1
    if ($LASTEXITCODE -ne 0) {
        $reason = ($list | ForEach-Object { "$_" }) -join ' '
        Write-CiWarning "No $Workflow runs to take packages from ($reason): vcpkg builds every port from source."
        Write-Summary @('### Prebuilt vcpkg packages', '', "None found ($Workflow has no runs yet): every port is built from source.")
        Set-StepOutput 'found' 'false'
        return
    }
    $json = ($list | Where-Object { $_ -is [string] }) -join "`n"		# stdout only: gh's notices on stderr aren't JSON
    $candidates = @($json | ConvertFrom-Json |
        Where-Object { $TrustedEvents -contains $_.event -and $TrustedBranches -ccontains $_.headBranch } |
        Sort-Object createdAt -Descending)

    foreach ($run in $candidates) {
        $artifact = & gh api "repos/$Repository/actions/runs/$($run.databaseId)/artifacts" --jq ".artifacts[] | select(.name == `"$ArtifactName`" and .expired == false) | .id" 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $artifact) { continue }

        Write-Host "==> $ArtifactName from run $($run.databaseId) ($($run.event) on $($run.headBranch), $($run.createdAt))"
        & gh run download $run.databaseId --repo $Repository --name $ArtifactName --dir $Destination
        if ($LASTEXITCODE -ne 0) { throw "Downloading $ArtifactName from run $($run.databaseId) failed (exit code $LASTEXITCODE)." }

        $zips = @(Get-ChildItem $Destination -Recurse -Filter '*.zip')
        $megabytes = Get-Megabytes $zips
        Write-Host ("    {0} packages, {1:N0} MB" -f $zips.Count, $megabytes)
        Set-StepOutput 'found' 'true'
        Set-StepOutput 'run' "$($run.databaseId)"
        Set-StepOutput 'branch' "$($run.headBranch)"
        Write-Summary @('### Prebuilt vcpkg packages', '',
            ("{0} packages ({1:N0} MB) from [run {2}]({3}) ({4} on ``{5}``, {6}). vcpkg restores the ones whose ABI matches and builds the rest." -f
                $zips.Count, $megabytes, $run.databaseId, $run.url, $run.event, $run.headBranch, $run.createdAt))
        return
    }

    Write-CiWarning "No unexpired $ArtifactName from a trusted $Workflow run: vcpkg builds every port from source."
    Write-Summary @('### Prebuilt vcpkg packages', '', "None found (no unexpired ``$ArtifactName`` from a push, schedule or manual run of $Workflow): every port is built from source.")
    Set-StepOutput 'found' 'false'
    return
}

# Prune -----------------------------------------------------------

$share = Join-Path $Installed "$Triplet\share"
if (-not (Test-Path $share)) { throw "No vcpkg install at $share." }
if (-not (Test-Path $Cache)) { throw "No package cache at $Cache." }
$Cache = (Resolve-Path $Cache).Path		# Compared with full paths below

$sha = [System.Security.Cryptography.SHA256]::Create()
$packages = foreach ($info in Get-ChildItem $share -Directory | ForEach-Object { Join-Path $_.FullName 'vcpkg_abi_info.txt' } | Where-Object { Test-Path $_ }) {
    $port = Split-Path (Split-Path $info) -Leaf
    $abi = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($info)) | ForEach-Object { $_.ToString('x2') })
    $zip = Join-Path $Cache "$($abi.Substring(0, 2))\$abi.zip"
    $version = ''
    $spdx = Join-Path (Split-Path $info) 'vcpkg.spdx.json'
    if (Test-Path $spdx) {
        try { $version = ((Get-Content $spdx -Raw | ConvertFrom-Json).packages | Where-Object { $_.name -eq $port } | Select-Object -First 1).versionInfo } catch { }
    }
    [pscustomobject]@{ port = $port; version = "$version"; abi = $abi; zip = $zip; present = (Test-Path $zip) }
}
$packages = @($packages | Sort-Object port)
if ($packages.Count -eq 0) { throw "No packages under $share." }

$keep = @{}
foreach ($package in $packages) { $keep[$package.zip.ToLowerInvariant()] = $true }
$removed = 0
foreach ($zip in Get-ChildItem $Cache -Recurse -Filter '*.zip') {
    if (-not $keep.ContainsKey($zip.FullName.ToLowerInvariant())) {
        Remove-Item $zip.FullName -Force
        $removed++
    }
}
Get-ChildItem $Cache -Directory | Where-Object { -not (Get-ChildItem $_.FullName -Force) } | Remove-Item -Force

$missing = @($packages | Where-Object { -not $_.present })
foreach ($package in $missing) { Write-CiWarning "$($package.port) is installed but not in the cache ($($package.abi)): it won't be prebuilt" }

$manifest = [ordered]@{
    triplet = $Triplet
    created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    vcpkg = "$(& git -C (Join-Path $PSScriptRoot '..\..\dependency\vcpkg') rev-parse HEAD 2>$null)"
    packages = @($packages | Where-Object { $_.present } | ForEach-Object {
        [ordered]@{ port = $_.port; version = $_.version; abi = $_.abi; bytes = (Get-Item $_.zip).Length }
    })
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $Cache 'manifest.json') -Encoding utf8

$kept = $packages.Count - $missing.Count
$megabytes = Get-Megabytes @($packages | Where-Object { $_.present } | ForEach-Object { Get-Item $_.zip })
Write-Host ("==> Kept {0} packages ({1:N0} MB), removed {2} unused, {3} missing" -f $kept, $megabytes, $removed, $missing.Count)
Write-Summary (@('### vcpkg package set', '', ("{0} packages ({1:N0} MB) for {2}; {3} unused removed." -f $kept, $megabytes, $Triplet, $removed), '',
    '| Port | Version | ABI |', '|---|---|---|') + @($packages | Where-Object { $_.present } | ForEach-Object { "| $($_.port) | $($_.version) | ``$($_.abi.Substring(0, 12))…`` |" }))
