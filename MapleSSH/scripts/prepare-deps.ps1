param([string]$WxSource)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$downloads = Join-Path $root 'out/downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
function Get-Verified([string]$Name, [string]$Url, [string]$Sha256) {
    $path = Join-Path $downloads $Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Sha256) {
        & curl.exe --fail --location --retry 6 --retry-all-errors --connect-timeout 15 --max-time 1800 --continue-at - --output $path $Url
        if ($LASTEXITCODE -ne 0) { throw "Download interrupted: $Name. Run again to resume." }
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Sha256) { throw "SHA-256 mismatch: $path. Do not use this archive." }
    return $path
}
function Expand-Verified([string]$Archive, [string]$Expected, [string]$Destination) {
    if (!(Test-Path -LiteralPath (Join-Path $Destination $Expected))) {
        New-Item -ItemType Directory -Force $Destination | Out-Null
        & "$env:SystemRoot/System32/tar.exe" -xf $Archive -C $Destination
        if ($LASTEXITCODE -ne 0) { throw "Cannot extract $Archive" }
        if (!(Test-Path -LiteralPath (Join-Path $Destination $Expected))) { throw "Archive is missing $Expected" }
    }
}
$archive = Get-Verified 'libvterm-0.3.3.tar.gz' 'https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz' '09156F43DD2128BD347CBEEBE50D9A571D32C64E0CF18D211197946AFF7226E0'
Expand-Verified $archive 'libvterm-0.3.3/include/vterm.h' $downloads
$archive = Get-Verified 'libssh2-1.11.1.tar.gz' 'https://www.libssh2.org/download/libssh2-1.11.1.tar.gz' 'D9EC76CBE34DB98EEC3539FE2C899D26B0C837CB3EB466A56B0F109CABF658F7'
Expand-Verified $archive 'libssh2-1.11.1/CMakeLists.txt' $downloads
$archive = Get-Verified 'openssl-3.5.9.tar.gz' 'https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz' '603F5602E2EEF00D77FBD429D34DCD5822BB301757A1BC9CDB24C670F1EB859A'
Expand-Verified $archive 'openssl-3.5.9/Configure' $downloads
$json = Get-Verified 'json.hpp' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63'
New-Item -ItemType Directory -Force (Join-Path $downloads 'include/nlohmann') | Out-Null
Copy-Item -LiteralPath $json -Destination (Join-Path $downloads 'include/nlohmann/json.hpp') -Force
$null = Get-Verified 'json-LICENSE.MIT' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT' '46A65CFFD1EA955132D95A8DD921640714A8D6B537D2E4E482D31145AE95B603'
if (!$WxSource) {
    $archive = Get-Verified 'wxWidgets-3.2.11.zip' 'https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.11/wxWidgets-3.2.11.zip' '02C4FDC8EC104A10EFD809238F800B632C4D5CC6A2D54582BFF775240007F01A'
    $WxSource = Join-Path $root 'out/deps/wxWidgets-3.2.11'
    Expand-Verified $archive 'build/cmake/init.cmake' $WxSource
}
if (!(Test-Path -LiteralPath (Join-Path $WxSource 'build/cmake/init.cmake'))) { throw 'A complete wxWidgets 3.2.11 source distribution is required.' }
Write-Host 'Dependency archives verified. wxWidgets source:' $WxSource
return $WxSource
