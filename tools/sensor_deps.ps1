# Lists the assembly closure the sensor helper needs at runtime, starting from LibreHardwareMonitorLib.dll
# (the only third-party assembly AnkhSensors.cs references). Metadata-only: nothing is executed.
#   powershell -File tools\sensor_deps.ps1 -Dir <folder with the helper DLLs>
# Output: one line per assembly: LOCAL <file> (must ship), GAC <name> (in-box .NET Framework), MISSING <name>.
param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$local = @{}
Get-ChildItem $Dir -Filter *.dll | ForEach-Object {
  try { $n = [Reflection.AssemblyName]::GetAssemblyName($_.FullName); $local[$n.Name] = $_.Name } catch { }
}
$seen = @{}; $queue = New-Object System.Collections.Queue
$queue.Enqueue('LibreHardwareMonitorLib')
$out = @()
while ($queue.Count) {
  $name = $queue.Dequeue()
  if ($seen.ContainsKey($name)) { continue }
  $seen[$name] = $true
  if ($local.ContainsKey($name)) {
    $file = Join-Path $Dir $local[$name]
    $out += "LOCAL   $($local[$name])"
    $asm = [Reflection.Assembly]::ReflectionOnlyLoadFrom($file)
    foreach ($r in $asm.GetReferencedAssemblies()) { $queue.Enqueue($r.Name) }
  } else {
    $inbox = $false
    try { [void][Reflection.Assembly]::ReflectionOnlyLoad($name); $inbox = $true } catch { }
    if (-not $inbox) { try { [void][Reflection.Assembly]::ReflectionOnlyLoad("$name, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089"); $inbox = $true } catch { } }
    if (-not $inbox) { try { [void][Reflection.Assembly]::ReflectionOnlyLoad("$name, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a"); $inbox = $true } catch { } }
    $out += $(if ($inbox -or $name -in 'mscorlib', 'netstandard') { "GAC     $name" } else { "MISSING $name" })
  }
}
$out | Sort-Object
$unused = $local.Values | Where-Object { "LOCAL   $_" -notin $out }
if ($unused) { "NOT NEEDED " + (($unused | Sort-Object) -join ', ') }
