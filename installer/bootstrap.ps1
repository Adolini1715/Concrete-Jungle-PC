# Install.bat runs this. It gets a private copy of Python (checked against its
# SHA-256), then hands over to installer\install.py, which does the rest.
# Nothing is installed on the system: everything stays in this folder (or in
# C:\PCJ-build when this folder's path has a space in it). A Python already
# installed on the PC is not used and not needed.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path -Parent $PSScriptRoot
$log = Join-Path $repo 'install-log.txt'
$py = Join-Path $PSScriptRoot 'python'
$exe = Join-Path $py 'tools\python.exe'
$url = 'https://www.nuget.org/api/v2/package/python/3.12.7'
$want = '149dd298e0b7a82250ca019471770fff079874088a4e8501ca20922d7df3a6ac'

function Say($m) { Write-Host $m; Add-Content -Path $log -Value $m -Encoding UTF8 }
function Stop-With($m) {
    Say ''
    Say "Problem: $m"
    exit 1
}

# Does the private Python run? (A copy cut short, or one an antivirus quarantined, does not.)
function Test-Python {
    if (-not (Test-Path $exe)) { return $false }
    try { $v = & $exe --version 2>&1; return ($LASTEXITCODE -eq 0 -and "$v" -match 'Python 3') } catch { return $false }
}

function Get-Python {
    Say 'Getting Python 3.12.7 for the installer (14 MB). It stays inside this folder.'
    $pkg = Join-Path $PSScriptRoot 'python.zip'
    if (Test-Path $pkg) { Remove-Item -Force $pkg }
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    try {
        if ($curl) {
            & curl.exe -sSL --retry 3 -o $pkg $url
            if ($LASTEXITCODE -ne 0) { throw "curl exit $LASTEXITCODE" }
        } else {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $pkg
        }
    } catch {
        Stop-With "could not download Python from nuget.org ($($_.Exception.Message)). Check the internet connection, then run Install.bat again."
    }
    $got = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLower()
    if ($got -ne $want) { Remove-Item $pkg; Stop-With "the Python download does not match its checksum ($got). Run Install.bat again." }
    if (Test-Path $py) { Remove-Item -Recurse -Force $py }
    New-Item -ItemType Directory -Force $py | Out-Null
    try { Expand-Archive -Path $pkg -DestinationPath $py -Force } catch { Stop-With "could not unpack Python ($($_.Exception.Message))." }
    Remove-Item $pkg
    if (-not (Test-Python)) {
        Stop-With ("Python was unpacked but will not start: $exe`n" +
                   "An antivirus may have blocked or removed it. Allow that file (or this folder) in the antivirus, then run Install.bat again.")
    }
    # pip writes notices to stderr; on Windows PowerShell 5.1 those arrive as error records,
    # which 'Stop' would turn into a crash of this script. Judge pip by its exit code instead.
    $ErrorActionPreference = 'Continue'
    & $exe -m ensurepip 2>&1 | Out-File -Append -Encoding UTF8 $log
    $pipExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($pipExit -ne 0) { Stop-With 'could not set up pip for the private Python. Run Install.bat again; if it repeats, send install-log.txt.' }
}

Add-Content -Path $log -Value "---- Install.bat $(Get-Date -Format s) in $repo" -Encoding UTF8
if (-not (Test-Python)) { Get-Python }
& $exe -I (Join-Path $PSScriptRoot 'install.py')
exit $LASTEXITCODE
