param([string]$BuildDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'out/build'), [switch]$MapleStore)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$run = Join-Path $out ('fixture-' + [Guid]::NewGuid().ToString('N'))
$binary = Join-Path $out 'maplessh-fixture.exe'
Push-Location (Join-Path $root 'tests/fixture')
try { & go.exe build -o $binary .; if ($LASTEXITCODE -ne 0) { throw 'Go fixture build failed.' } } finally { Pop-Location }
New-Item -ItemType Directory -Force $run | Out-Null
$process = $null
$storeProcess = $null
try {
    $process = Start-Process -FilePath $binary -ArgumentList @('-root', ('"' + $run + '"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log')
    $ready = Join-Path $run 'ready.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($process.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Isolated fixture failed to start.' }
        Start-Sleep -Milliseconds 100
    }
    if ($MapleStore) {
        $storeBinary = Join-Path $out 'maplestore-fixture.exe'
        Push-Location (Join-Path (Split-Path $root -Parent) 'MapleStore')
        try { & go.exe build -o $storeBinary ./cmd/maplestore; if ($LASTEXITCODE -ne 0) { throw 'MapleStore fixture build failed.' } } finally { Pop-Location }
        $information = Get-Content -LiteralPath $ready -Raw | ConvertFrom-Json
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        $values = @{ MAPLESTORE_ADDR="127.0.0.1:$port"; MAPLESTORE_DATA_DIR=(Join-Path $run 'store-data'); MAPLESTORE_CONFIG_DIR=(Join-Path $run 'store-config'); MAPLESTORE_ADMIN_USERNAME=$information.username; MAPLESTORE_ADMIN_PASSWORD=$information.password; MAPLESTORE_SECURE_COOKIE='false' }
        $previous = @{}
        try {
            foreach ($name in $values.Keys) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process'); [Environment]::SetEnvironmentVariable($name, $values[$name], 'Process') }
            $storeProcess = Start-Process -FilePath $storeBinary -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'store-stdout.log') -RedirectStandardError (Join-Path $run 'store-stderr.log')
        } finally { foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') } }
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        for (;;) {
            try { $health = Invoke-WebRequest -Uri "http://127.0.0.1:$port/healthz" -TimeoutSec 1; if ($health.StatusCode -eq 200) { break } } catch {}
            if ($storeProcess.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Isolated MapleStore failed to start.' }
            Start-Sleep -Milliseconds 100
        }
        $information | Add-Member -NotePropertyName metrics_port -NotePropertyValue $information.dav_port
        $information.dav_port = $port
        [IO.File]::WriteAllText($ready, ($information | ConvertTo-Json -Depth 5))
        Write-Host 'Testing isolated MapleStore WebDAV with temporary credentials.'
    }
    & (Join-Path $BuildDirectory 'Release/maplessh_integration_tests.exe') $ready
    if ($LASTEXITCODE -ne 0) { throw 'SSH/WebDAV integration failed.' }
} finally {
    if ($storeProcess -and !$storeProcess.HasExited) { Stop-Process -Id $storeProcess.Id; $storeProcess.WaitForExit() }
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }
    $resolved = [IO.Path]::GetFullPath($run)
    $allowed = [IO.Path]::GetFullPath($out).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or (Split-Path $resolved -Leaf) -notlike 'fixture-*') { throw 'Refusing cleanup outside isolated fixture directory.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
