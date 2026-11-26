# Play Double Dash!! (the Windows download's "Play Double Dash.bat" runs this).
#
# The download holds everything but the game's own code: that is made here,
# from your own disc (an image, .iso, .gcm or .ciso, as it is, or a folder
# extracted from one), the first time (and again after an update): qilin-disc
# reads its main.dol, qilin lifts that into C++ and qilin-dsp the sound
# processor's microcode in it, the bundled compiler (LLVM-MinGW) compiles it, and
# it's linked with the prebuilt rest into Double Dash.exe. Then, and every time
# after, it starts the game.
#
#   play.ps1 [-Rebuild] [-BuildOnly] [-ChooseDisc]
#
# -Rebuild builds again even if Double Dash.exe is up to date; -BuildOnly
# doesn't start the game afterwards; -ChooseDisc asks for the disc again.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [switch]$ChooseDisc
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}

$kit = $PSScriptRoot
$top = Split-Path -Parent $kit
$work = Join-Path $top "work"
$game = Join-Path $top "Double Dash.exe"
$stamp = Join-Path $work "built-with.txt"
$discFile = Join-Path $work "disc-folder.txt"
$kitVersion = (Get-Content (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# The game is compiled for this PC's own processor (-march=native: FMA for the
# console's fused multiply-adds, which are then one instruction instead of a call
# into the C library, and whatever else it has). So a build is for one CPU:
# it's built again if the folder moves to a PC with a different one.
$cpu = if ($onWindows) { "$env:PROCESSOR_IDENTIFIER" } else { "cross" }
# SHA-256 of a file, upper-case hex. Not Get-FileHash: that cmdlet is missing
# when Windows PowerShell inherits a PowerShell 7 module path, as it does
# when started from pwsh (or from CI's shell).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path))  # .NET resolves against the process folder, not PowerShell location
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
# Content identity matters while changes share an Unreleased version number.
$identityParts = @("lib/libmkdd_kit.a", "qilin.exe", "qilin-disc.exe", "qilin-dsp.exe", "include/dsp_core.hpp", "plan.tsv", "kit.ninja") | ForEach-Object {
    (Get-Sha256 (Join-Path $kit $_))
}
$builtFor = "$kitVersion | $cpu | $($identityParts -join ':')"
# Off Windows (testing the download under Wine), QILIN_KIT_RUNNER runs the kit's programs.
$runner = $env:QILIN_KIT_RUNNER

# Runs one of the kit's programs, its output to the window; returns its exit code.
function Invoke-Kit([string]$exe, [string[]]$arguments) {
    if ($runner) { & $runner $exe @arguments | Out-Host } else { & $exe @arguments | Out-Host }
    return $LASTEXITCODE
}
$qilin = Join-Path $kit "qilin.exe"
$discTool = Join-Path $kit "qilin-disc.exe"
# Whether a path is something to hand qilin-disc: an image file, or a folder with sys\main.dol.
# (A file picked inside an extracted disc's sys folder, or that folder, means the folder above.)
function Resolve-Disc([string]$path) {
    if (-not $path) { return "" }
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $parent = Split-Path -Parent $path
        if ((Split-Path -Leaf $parent) -eq "sys" -and @("main.dol", "boot.bin", "fst.bin", "bi2.bin", "apploader.img") -contains (Split-Path -Leaf $path)) {
            return (Split-Path -Parent $parent)
        }
        return $path
    }
    if ((Split-Path -Leaf $path) -eq "sys" -and (Test-Path -LiteralPath (Join-Path $path "main.dol"))) { return (Split-Path -Parent $path) }
    if (Test-Path -LiteralPath $path -PathType Container) { return $path }
    return ""
}
# Ask for the disc: an image, or any file in an extracted disc's sys folder. "" if nothing was chosen.
function Select-Disc {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Mario Kart: Double Dash!! disc: the image (.iso, .gcm, .ciso), or sys\main.dol of an extracted disc"
    $dialog.Filter = "GameCube discs (*.iso;*.gcm;*.ciso;main.dol)|*.iso;*.gcm;*.ciso;main.dol|All files (*.*)|*.*"
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return "" }
    return $dialog.FileName
}
# What qilin-disc says of a disc: "" if it is the supported one, else why not.
function Test-Disc([string]$tool, [string]$path) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns redirected error output into errors
    $text = if ($runner) { & $runner $tool info $path --json 2>&1 | ForEach-Object { "$_" } | Out-String } else { & $tool info $path --json 2>&1 | ForEach-Object { "$_" } | Out-String }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { return ($text.Trim() -replace "^qilin-disc: ", "") }
    $info = $text | ConvertFrom-Json
    if ($info.id -ne "GM4E01" -or $info.revision -ne 0) {
        return "it's $($info.id) revision $($info.revision) ($($info.title)): this port needs Mario Kart: Double Dash!! for the USA, GM4E01, revision 0"
    }
    if ($info.dol_sha256 -ne "e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae") {
        return "its main.dol isn't the supported build's (SHA-256 $($info.dol_sha256)): a modified or damaged copy?"
    }
    return ""
}

# The disc: the one used last, else ask (until one is the supported game).
$disc = if (-not $ChooseDisc -and (Test-Path $discFile)) { (Get-Content $discFile -Raw).Trim() } else { "" }
while ($true) {
    $resolved = Resolve-Disc $disc
    if ($resolved) {
        $why = Test-Disc $discTool $resolved
        if (-not $why) { $disc = $resolved; break }
        Write-Host "$resolved can't be used: $why" -ForegroundColor Yellow
    } elseif ($disc) {
        Write-Host "$disc isn't there." -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "no disc: write its path into $discFile" }
    $disc = Select-Disc
    if (-not $disc) {
        Write-Host "No disc chosen."
        exit 0
    }
}
$disc = $disc.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Up to date: just play.
$built = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path $game) -and $built -eq $builtFor) {
    if (-not $BuildOnly) { Start-Process -FilePath $game -ArgumentList "`"$disc`"" -WorkingDirectory $top }
    exit 0
}

Write-Host "Mario Kart: Double Dash!! QilinRecomp $kitVersion" -ForegroundColor Cyan
if ((Test-Path $game) -and $built -and -not $built.StartsWith("$kitVersion |")) { Write-Host "Updating the game for this version: a few minutes." }
elseif (Test-Path $game) { Write-Host "Building the game for this PC's processor: a few minutes." }
else { Write-Host "Building the game from your disc. This happens once (and after an update): a few minutes (up to 15 on a slower PC)." }
Write-Host "Disc: $disc"
Write-Host ""

# Lift main.dol (files whose text is unchanged are left alone, so an update
# recompiles only what changed).
Write-Host "== Recompiling main.dol into C++" -ForegroundColor Cyan
$gen = Join-Path $work "gen"
$dol = Join-Path $work "main.dol"
if ((Invoke-Kit $discTool @("dol", $disc, $dol)) -ne 0) { throw "the disc's main.dol couldn't be read" }
$lift = @("lift", $dol, "-o", $gen, "--plan", (Join-Path $kit "plan.tsv"), "--files", "32")
if ((Invoke-Kit $qilin $lift) -ne 0) { throw "qilin lift failed" }
# The DSP's microcode, beside it (after: qilin lift owns the folder). Replaced only when its
# text differs, so it too is compiled again only when it changed.
$ucode = Join-Path $gen "dsp_ucode.cpp"
$ucodeNew = Join-Path $work "dsp_ucode.new"
if ((Invoke-Kit (Join-Path $kit "qilin-dsp.exe") @("lift", $dol, "-o", $ucodeNew)) -ne 0) { throw "qilin-dsp lift failed" }
if ((Test-Path -LiteralPath $ucode) -and ((Get-Sha256 $ucode) -eq (Get-Sha256 $ucodeNew))) { Remove-Item -LiteralPath $ucodeNew -Force }
else { Move-Item -LiteralPath $ucodeNew -Destination $ucode -Force }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem $gen -Filter *.cpp | Sort-Object Name)
$lines = @(
    "# Written by play.ps1: the game's lifted code, compiled against the kit.",
    "kit = ../kit",
    "include ../kit/kit.ninja",
    "cxx = ..\kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
    "native = -march=native",
    "rule cxx",
    "  command = `$cxx `$cxxflags `$native -Igen -MD -MF `$out.d -c `$in -o `$out",
    "  depfile = `$out.d",
    "  deps = gcc",
    "  description = Compiling `$in",
    "rule link",
    "  command = `$cxx `$ldflags -o `$out `$in `$libs",
    "  description = Linking `$out"
)
if (-not $onWindows) { $lines[3] = "cxx = " + $env:QILIN_KIT_CXX; $lines[4] = "native =" }   # testing off Windows: a cross compiler, for any CPU
$objects = @()
foreach ($s in $sources) {
    $object = "obj/$($s.BaseName).o"
    $objects += $object
    $lines += "build ${object}: cxx gen/$($s.Name)"
}
# (Ninja splits a name at a space: the program is built as mkdd.exe and copied to its own name.)
$lines += "build mkdd.exe: link $($objects -join ' ') | ../kit/lib/libmkdd_kit.a"
$lines += "default mkdd.exe"
Set-Content -Path (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

Write-Host "== Compiling ($($sources.Count) files; the biggest take a few minutes each)" -ForegroundColor Cyan
$ninja = if ($onWindows) { Join-Path $kit "ninja.exe" } else { "ninja" }
$jobs = [Math]::Max(1, [Environment]::ProcessorCount)
if ($onWindows) { $code = Invoke-Kit $ninja @("-C", $work, "-j", "$jobs") }
else { & $ninja -C $work -j $jobs | Out-Host; $code = $LASTEXITCODE }
if ($code -ne 0) { throw "compiling failed" }
# Stage beside the destination; a failed copy leaves the old executable intact.
$staged = "$game.new"
Copy-Item -LiteralPath (Join-Path $work "mkdd.exe") -Destination $staged -Force
if (Test-Path -LiteralPath $game -PathType Leaf) {
    [System.IO.File]::Replace($staged, $game, "$game.previous", $true)
} else {
    [System.IO.File]::Move($staged, $game)
}
Set-Content -LiteralPath "$discFile.tmp" -Value $disc -Encoding UTF8
Move-Item -LiteralPath "$discFile.tmp" -Destination $discFile -Force
Set-Content -Path $stamp -Value $builtFor -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting Mario Kart: Double Dash!!..."
    Start-Process -FilePath $game -ArgumentList "`"$disc`"" -WorkingDirectory $top
}
exit 0
