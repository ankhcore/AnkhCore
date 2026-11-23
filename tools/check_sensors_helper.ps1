# Compiles the 2.5 sensor helper against exactly the shipped helper files and runs its read-only
# --selfcheck (no mutex, no files, no fan commands). Works in a temporary folder only.
#   powershell -File tools\check_sensors_helper.ps1 -Inputs <folder with the shipped SENS files>
param([Parameter(Mandatory)][string]$Inputs, [Parameter(Mandatory)][string]$Files)   # Files: comma-separated
$ErrorActionPreference = 'Stop'
$list = $Files -split ','
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$tmp = Join-Path $env:TEMP ('ankh-sensorcheck-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null
try {
  foreach ($f in $list) { Copy-Item (Join-Path $Inputs $f) $tmp }
  Copy-Item (Join-Path $repo 'src\sensors\AnkhSensors.cs') $tmp
  $csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
  # console target and no admin manifest: the check runs unelevated and prints its result
  $out = & $csc /nologo /target:exe /platform:x64 /optimize "/out:$tmp\AnkhSensorsCheck.exe" "/reference:$tmp\LibreHardwareMonitorLib.dll" /reference:System.Management.dll "$tmp\AnkhSensors.cs" 2>&1
  "csc exit $LASTEXITCODE"; $out | Where-Object { $_ -match 'error|warning' } | Select-Object -First 10
  if ($LASTEXITCODE -ne 0) { exit 1 }
  if (Test-Path (Join-Path $Inputs 'AnkhSensors.exe.config')) { Copy-Item (Join-Path $Inputs 'AnkhSensors.exe.config') "$tmp\AnkhSensorsCheck.exe.config" }
  $r = & "$tmp\AnkhSensorsCheck.exe" --selfcheck 2>&1
  $r
  "selfcheck exit $LASTEXITCODE"
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
