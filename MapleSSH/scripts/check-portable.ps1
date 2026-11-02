param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$output = & dumpbin.exe /headers /dependents $Executable
if ($LASTEXITCODE -ne 0) { throw 'dumpbin failed.' }
if (($output -join [Environment]::NewLine) -notmatch '8664 machine') { throw 'Executable is not x64.' }
$imports = @($output | ForEach-Object { if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $matches[1] } } | Sort-Object -Unique)
$system = @('KERNEL32.dll','USER32.dll','GDI32.dll','COMDLG32.dll','WINSPOOL.DRV','ADVAPI32.dll','SHELL32.dll','OLE32.dll','OLEAUT32.dll','UUID.dll','COMCTL32.dll','WINMM.dll','WS2_32.dll','CRYPT32.dll','BCRYPT.dll','WINHTTP.dll','XMLLITE.dll','SHLWAPI.dll','UXTHEME.dll','DWMAPI.dll','VERSION.dll','RPCRT4.dll','MSIMG32.dll','IMM32.dll','OLEACC.dll','WTSAPI32.dll','IPHLPAPI.dll','NTDLL.dll','SETUPAPI.dll')
foreach ($library in $imports) { if ($library -notin $system -and $library -notmatch '^api-ms-win-') { throw "Non-approved dependency: $library. Review before distributing." } }
if (!$imports.Count) { throw 'No PE imports were found.' }
Write-Host 'x64 system-only imports:' ($imports -join ', ')
