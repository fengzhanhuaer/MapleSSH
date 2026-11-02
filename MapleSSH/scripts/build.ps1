param([string]$WxSource, [string]$PerlPath = 'perl.exe', [int]$Jobs = 8, [switch]$SkipPrepare, [switch]$Integration)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$build = Join-Path $out 'build'
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio with Desktop development with C++ and a Windows SDK.' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$environment = & cmd.exe /c ('"' + $dev + '" -arch=x64 -host_arch=x64 >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the x64 MSVC environment.' }
foreach ($line in $environment) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') } }
if (!$SkipPrepare) { $WxSource = & "$PSScriptRoot/prepare-deps.ps1" -WxSource $WxSource }
if (!$WxSource) { throw 'Supply -WxSource when using -SkipPrepare.' }
$WxSource = (Resolve-Path -LiteralPath $WxSource).Path
$openssl = Join-Path $out 'openssl'
if (!(Test-Path -LiteralPath (Join-Path $openssl 'lib/libcrypto.lib'))) {
    $perl = (Get-Command $PerlPath -ErrorAction Stop).Source
    $platform = & $perl -e 'print $^O'
    if ($LASTEXITCODE -ne 0 -or $platform -ne 'MSWin32') { throw 'A native Windows Perl is required for OpenSSL (not Git/Cygwin Perl).' }
    $env:PATH = (Split-Path $perl -Parent) + ';' + $env:PATH
    $env:LC_ALL = 'C'; $env:LC_CTYPE = 'C'; $env:LANG = 'C'
    Push-Location (Join-Path $out 'downloads/openssl-3.5.9')
    try {
        & $perl Configure VC-WIN64A no-shared no-tests no-asm no-module no-apps no-docs "--prefix=$openssl" --libdir=lib
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL configure failed.' }
        & nmake.exe /NOLOGO build_libs
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL compilation failed.' }
        & nmake.exe /NOLOGO install_dev
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL installation failed.' }
    } finally { Pop-Location }
}
& cmake.exe -S $root -B $build -A x64 "-DMAPLE_WX_SOURCE=$WxSource" "-DOPENSSL_ROOT_DIR=$openssl" -DMAPLE_DEPS_ONLY=OFF -DMAPLE_BUILD_GUI=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake.exe --build $build --config Release --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
& ctest.exe --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
$executable = Join-Path $build 'bin/Release/MapleSSH.exe'
& "$PSScriptRoot/check-portable.ps1" -Executable $executable
if ($Integration) { & "$PSScriptRoot/integration.ps1" -BuildDirectory $build }
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $dist 'MapleSSH.exe') -Force
$hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $dist 'MapleSSH.exe.sha256'), "$hash  MapleSSH.exe" + [Environment]::NewLine)
Write-Host 'Portable executable:' (Join-Path $dist 'MapleSSH.exe')
