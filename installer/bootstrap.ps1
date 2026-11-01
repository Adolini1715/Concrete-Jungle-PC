# Install.bat runs this. It gets a private copy of Python (checked against its
# SHA-256), then hands over to installer\install.py, which does the rest.
# Nothing is installed on the system: everything stays in this folder (or in
# C:\PCJ-build when this folder's path has a space in it).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path -Parent $PSScriptRoot
$py = Join-Path $PSScriptRoot 'python'
$exe = Join-Path $py 'tools\python.exe'
$url = 'https://www.nuget.org/api/v2/package/python/3.12.7'
$want = '149dd298e0b7a82250ca019471770fff079874088a4e8501ca20922d7df3a6ac'

if (-not (Test-Path $exe)) {
    Write-Host 'Getting Python 3.12.7 for the installer (14 MB)'
    $pkg = Join-Path $PSScriptRoot 'python.zip'
    & curl.exe -sSL --retry 3 -o $pkg $url
    if ($LASTEXITCODE -ne 0) { throw 'Could not download Python. Check the internet connection.' }
    $got = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLower()
    if ($got -ne $want) { Remove-Item $pkg; throw "The Python download does not match its checksum ($got)." }
    New-Item -ItemType Directory -Force $py | Out-Null
    & tar.exe -xf $pkg -C $py
    if ($LASTEXITCODE -ne 0) { throw 'Could not unpack Python.' }
    Remove-Item $pkg
    & $exe -m ensurepip
    if ($LASTEXITCODE -ne 0) { throw 'Could not set up pip.' }
}
& $exe -I (Join-Path $PSScriptRoot 'install.py')
exit $LASTEXITCODE
