[CmdletBinding()]
param([switch]$StageOnly)

$ErrorActionPreference = 'Stop'
$solution = Join-Path $PSScriptRoot 'FlowingHair.sln'
$vsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$legacySetEnv = 'C:\Program Files\Microsoft SDKs\Windows\v7.1\Bin\SetEnv.Cmd'
$legacyDumpbin = 'C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64\dumpbin.exe'

$deps = Join-Path (Split-Path -Parent $PSScriptRoot) 'KenshiLib_Examples_deps'
if (-not $env:KENSHILIB_DIR -or -not (Test-Path -LiteralPath $env:KENSHILIB_DIR)) {
    $env:KENSHILIB_DIR = Join-Path $deps 'KenshiLib'
}
if (-not $env:BOOST_INCLUDE_PATH -or -not (Test-Path -LiteralPath $env:BOOST_INCLUDE_PATH)) {
    $env:BOOST_INCLUDE_PATH = Join-Path $deps 'boost_1_60_0'
}

$runningKenshi = Get-CimInstance Win32_Process -Filter "Name='kenshi_x64.exe'" -ErrorAction SilentlyContinue
if ($runningKenshi -and -not $StageOnly) {
    $ids = ($runningKenshi | Select-Object -ExpandProperty ProcessId) -join ', '
    throw "Kenshi is still running (PID(s): $ids). Close it before rebuilding FlowingHair.dll."
}

if (-not (Test-Path -LiteralPath $vsWhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$visualStudio = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'The Visual Studio C++ workload is not installed. Install C++ build tools and the Visual C++ 2010 (v100) x64 toolset required by KenshiLib.'
}

$msBuild = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'MSBuild') -Filter MSBuild.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\Bin\\MSBuild\.exe$' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $msBuild) {
    throw 'MSBuild was not found in the Visual Studio C++ installation.'
}

if (-not (Test-Path -LiteralPath $legacySetEnv)) {
    throw 'Windows SDK 7.1 is not installed; its VC10 x64 environment is required.'
}

$pluginSourcePath = Join-Path $PSScriptRoot 'src\Plugin.cpp'
$pluginSource = Get-Content -LiteralPath $pluginSourcePath -Raw
if ($pluginSource.Contains('->setIndexDataStartIndex(') -or $pluginSource.Contains('->setIndexDataEndIndex(')) {
    throw 'Unsafe body index-range override: installed Ogre writes those fields into shared SubMesh IndexData. Use the isolated draw-operation pointer override.'
}
if ($pluginSource.Contains('->getVisibilityMask(') -or $pluginSource.Contains('->isVisible()')) {
    throw 'Unsafe visibility gate: use resolved native mask getters and object visibility, not header-inline SceneManager fields or current-pass isVisible().'
}
if ($pluginSource.Contains('->shareSkeletonInstanceWith(')) {
    throw 'Unsafe skeleton sharing call detected in Plugin.cpp. Replacement entities must never join the character skeleton group.'
}
if ($pluginSource.Contains('->stopSharingSkeletonInstance()')) {
    throw 'Unsafe skeleton-sharing-set mutation detected in Plugin.cpp. Kenshi mutates this bookkeeping across renderer threads.'
}
if ($pluginSource.Contains('->hasSkeleton(') -or $pluginSource.Contains('->getSkeleton(') -or
    $pluginSource.Contains('->sharesSkeletonInstance(') -or
    $pluginSource.Contains('->getSkeletonInstanceSharingSet(')) {
    throw 'ABI-unsafe inline Ogre::Entity skeleton accessor detected. Installed Entity fields are eight bytes later than the KenshiLib headers.'
}
if ($pluginSource.Contains('addSoftwareAnimationRequest(') -or
    $pluginSource.Contains('_getSkelAnimVertexData(') -or
    $pluginSource.Contains('chooseVertexDataForBinding')) {
    throw 'Software-animation render path detected. Flowing Hair must use an isolated cloned mesh buffer.'
}
if (-not $pluginSource.Contains('->createEntity(') -or
    -not $pluginSource.Contains('->clone(') -or
    -not $pluginSource.Contains('getWorldTransforms(') -or
    -not $pluginSource.Contains('original->_updateAnimation()') -or
    -not $pluginSource.Contains('clearNamedAutoConstant(')) {
    throw 'Required isolated clone and read-only source render palette path is missing.'
}

# MSBuild 2022 understands the v100 project files, while SetEnv supplies the
# VC10 compiler DLL paths, SDK include paths, and x64 library paths.
$buildCommand = "`"$legacySetEnv`" /Release /x64 && `"$msBuild`" `"$solution`" /m /t:Rebuild /p:Configuration=Release /p:Platform=x64"
if ($StageOnly) {
    $buildCommand += ' /p:FlowingHairStage=true'
}
$savedErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & cmd.exe /d /c $buildCommand
    $buildExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($buildExitCode -ne 0) {
    throw "Build failed with exit code $buildExitCode."
}

if (-not $StageOnly) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'packaging\RE_Kenshi.json') -Destination (Join-Path $PSScriptRoot 'RE_Kenshi.json') -Force
}
$plugin = if ($StageOnly) { Join-Path $PSScriptRoot 'build\staged\FlowingHair.dll' } else { Join-Path $PSScriptRoot 'FlowingHair.dll' }
if (-not (Test-Path -LiteralPath $plugin)) { throw 'Plugin build completed without producing FlowingHair.dll.' }
if (-not (Test-Path -LiteralPath $legacyDumpbin)) {
    throw 'VC10 x64 dumpbin.exe was not found; cannot verify skeleton-safety imports.'
}
$imports = (& $legacyDumpbin /imports $plugin) -join "`n"
if ($LASTEXITCODE -ne 0 -or $imports -notmatch '(?im)^\s+KenshiLib\.dll\s*$') {
    throw 'Built DLL must import KenshiLib.dll.'
}
$exports = (& $legacyDumpbin /exports $plugin) -join "`n"
if ($LASTEXITCODE -ne 0 -or $exports -notmatch '\?startPlugin@@YAXXZ') {
    throw 'Built DLL is missing the RE_Kenshi startPlugin export.'
}
if ($imports -match 'shareSkeletonInstanceWith') {
    throw 'Built DLL imports unsafe shareSkeletonInstanceWith; validation failed.'
}
if ($imports -match 'stopSharingSkeletonInstance') {
    throw 'Built DLL imports unsafe stopSharingSkeletonInstance; validation failed.'
}
if ($imports -notmatch 'clone@Mesh' -or
    $imports -notmatch '_updateAnimation@Entity' -or
    $imports -notmatch 'clearNamedAutoConstant@GpuProgramParameters' -or
    $imports -notmatch 'setNamedConstant@GpuProgramParameters') {
    throw 'Built DLL is missing required clone/private render-palette imports.'
}
if ($imports -match 'addSoftwareAnimationRequest@Entity' -or
    $imports -match '_getSkelAnimVertexData@Entity' -or
    $imports -match 'chooseVertexDataForBinding@Entity') {
    throw 'Built DLL still imports the rejected software-animation render path.'
}
if ($StageOnly) { Write-Output "Validated staged DLL: $plugin (installed DLL unchanged)" }
else { Write-Output "Deployed FlowingHair.dll and RE_Kenshi.json to $PSScriptRoot" }
