Set-Location C:\kindle-win32
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path ".\win32_sim.exe").Path
$psi.Arguments = "apps/progman.exe"
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$p = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Milliseconds 1500
if (-not $p.HasExited) {
    $p.Kill()
}
$stdout = $p.StandardOutput.ReadToEnd()
$stderr = $p.StandardError.ReadToEnd()
Write-Output "=== STDOUT ==="
Write-Output ($stdout -split "`n" | Select-Object -First 40)
if ($stderr) {
    Write-Output "=== STDERR ==="
    Write-Output ($stderr -split "`n" | Select-Object -First 40)
}
