# Builds mkdd in one go from the source, as the APB recomp's
# build-apb.ps1: configures (the first time), lifts your disc's main.dol and
# builds the game. Run it again after a `git pull`: only what changed is
# rebuilt.
#
#   powershell -ExecutionPolicy Bypass -File tools\build-mkdd.ps1 [-Disc <image or folder>] [-Run] [-Pull] [-ChooseDisc]
#
# The disc is your own copy (USA, GM4E01): an image (.iso, .gcm, .ciso) as it
# is, or a folder extracted from one (docs/disc.md).
#
# On Windows, "Play Double Dash.bat" and "Update Double Dash.bat" in the
# project's folder run it for you: double-click them. It needs Visual Studio
# 2022 17.10 or newer (with "Desktop development with C++") and finds its
# CMake and Git itself, from any window. With no -Disc it uses the disc it
# used last (build-game\disc-folder.txt), else asks with a file picker.
# -Run starts the game after; -Pull fetches the latest code first; -Play
# starts it straight away if it's already built.
param(
    [string]$Disc = "",
    [switch]$Run,
    [switch]$Pull,
    [switch]$ChooseDisc,
    [switch]$Play      # start the game if it's built (Play Double Dash.bat); else build it, then start it
)
$ErrorActionPreference = "Stop"
trap {   # any failure: say what in one red line, and end with an error code
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-game"
$discFile = Join-Path $build "disc-folder.txt"
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows

if ($Play -and -not $Disc -and -not $ChooseDisc -and -not $Pull) {
    $built = Join-Path $build "Release\mkdd.exe"
    if ((Test-Path $built -PathType Leaf) -and (Test-Path $discFile -PathType Leaf)) {
        $last = (Get-Content $discFile -Raw).Trim()
        if (-not (Test-Path -LiteralPath $last)) { throw "The remembered disc has moved ($last): use -ChooseDisc to select it again." }
        Start-Process -FilePath $built -ArgumentList "`"$last`"" -WorkingDirectory (Split-Path -Parent $built)
        exit 0
    }
    Write-Host "Mario Kart: Double Dash!! isn't built yet: building it now. The first build takes 10 to 40 minutes."
    $Run = $true
}

if ($Play) { $Run = $true }

function Invoke-Step([string]$what, [string]$exe, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# CMake and Git: on the PATH, else the ones Visual Studio installs, else the usual places.
function Add-Tool([string]$name, [string[]]$folders) {
    if (Get-Command $name -ErrorAction SilentlyContinue) { return $true }
    foreach ($f in $folders) {
        if ($f -and (Test-Path (Join-Path $f "$name.exe"))) {
            $env:PATH = "$f;$env:PATH"
            return $true
        }
    }
    return $false
}
if ($onWindows) {
    $vs = ""
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    $ide = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft" } else { "" }
    $haveCmake = Add-Tool "cmake" @($(if ($ide) { Join-Path $ide "CMake\CMake\bin" }), (Join-Path $env:ProgramFiles "CMake\bin"))
    $haveGit = Add-Tool "git" @((Join-Path $env:ProgramFiles "Git\cmd"), $(if ($ide) { Join-Path $ide "TeamFoundation\Team Explorer\Git\cmd" }))
    if (-not $vs) { throw "Visual Studio 2022 wasn't found. Install it (the free Community edition) with 'Desktop development with C++'." }
    if (-not $haveCmake) { throw "CMake wasn't found. In the Visual Studio Installer, add 'C++ CMake tools for Windows'." }
    if (-not $haveGit) { throw "Git wasn't found. Install Git for Windows (git-scm.com), or add 'Git for Windows' in the Visual Studio Installer." }
} elseif (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake isn't installed."
}

if ($Pull) {
    Write-Host "== Getting the latest code" -ForegroundColor Cyan
    & git -C $root pull --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull failed (exit code $LASTEXITCODE). If you changed files in the project folder, that's why." }
}

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
    $text = & $tool info $path --json 2>&1 | ForEach-Object { "$_" } | Out-String
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

# Built for this PC's own processor (QILIN_NATIVE: AVX2 and FMA where it has them).
$configure = @("-S", $root, "-B", $build, "-DQILIN_NATIVE=ON")
if ($onWindows -and -not (Test-Path (Join-Path $build "CMakeCache.txt"))) { $configure += @("-G", "Visual Studio 17 2022", "-A", "x64") }
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }
# The disc is read where it lies (docs/disc.md): qilin-disc, built first, says what it is and
# copies its main.dol out for the build.
Invoke-Step "Configuring" "cmake" $configure
Invoke-Step "Building the disc reader" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin_disc_tool")
$exe = if ($onWindows) { ".exe" } else { "" }
$discTool = @((Join-Path $build "Release/qilin-disc$exe"), (Join-Path $build "qilin-disc$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $discTool) { throw "built, but qilin-disc$exe isn't where it should be" }

# The disc: the one used last, else ask (until one is the supported game).
if (-not $Disc -and -not $ChooseDisc -and (Test-Path $discFile)) { $Disc = (Get-Content $discFile -Raw).Trim() }
while ($true) {
    $resolved = Resolve-Disc $Disc
    if ($resolved) {
        $why = Test-Disc $discTool $resolved
        if (-not $why) { $Disc = $resolved; break }
        Write-Host "$resolved can't be used: $why" -ForegroundColor Yellow
    } elseif ($Disc) {
        Write-Host "$Disc isn't there." -ForegroundColor Yellow
    }
    if (-not $onWindows) { throw "give the disc with -Disc <image or extracted folder>" }
    $Disc = Select-Disc
    if (-not $Disc) { throw "no disc chosen" }
}
$Disc = (Resolve-Path -LiteralPath $Disc).Path
# Its main.dol, replaced only when it differs: the game's code is lifted again only for a different disc.
$dol = Join-Path $build "main.dol"
Invoke-Step "Reading the disc's main.dol" $discTool @("dol", $Disc, "$dol.new")
$same = (Test-Path $dol) -and ((Get-Item $dol).Length -eq (Get-Item "$dol.new").Length) -and
        (-not (Compare-Object ([System.IO.File]::ReadAllBytes($dol)) ([System.IO.File]::ReadAllBytes("$dol.new")) -SyncWindow 0))
if ($same) { Remove-Item -LiteralPath "$dol.new" -Force } else { Move-Item -LiteralPath "$dol.new" -Destination $dol -Force }
Invoke-Step "Configuring for the game" "cmake" ($configure + "-DQILIN_GAME_DOL=$dol")
Invoke-Step "Building Mario Kart: Double Dash!! (the first time lifts and compiles the whole game: 10 to 40 minutes)" "cmake" @("--build", $build, "--config", "Release", "--target", "mkdd", "--parallel")
$game = @((Join-Path $build "Release/mkdd$exe"), (Join-Path $build "mkdd$exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $game) { throw "built, but mkdd$exe isn't where it should be" }

# Commit the selection only after both compilation and finding the output succeed.
Set-Content -LiteralPath "$discFile.tmp" -Value $Disc -Encoding UTF8
Move-Item -LiteralPath "$discFile.tmp" -Destination $discFile -Force
Write-Host "Built: $game" -ForegroundColor Green
if ($Run) {
    Write-Host "Starting Mario Kart: Double Dash!!..."
    Start-Process -FilePath $game -ArgumentList "`"$Disc`"" -WorkingDirectory (Split-Path -Parent $game)
} else {
    Write-Host "Run it with:  & `"$game`" `"$Disc`""
}
