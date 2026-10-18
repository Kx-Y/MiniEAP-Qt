# ============================================================================
# Build MiniEAP (console client + Qt GUI) on Windows with Qt's bundled
# llvm-mingw toolchain, without invoking CMake.
#
#   powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
#
# Why not CMake here? Qt 6.10 for llvm-mingw is built against libc++, so the
# C++ front end must be compiled with the matching clang/libc++, while the C
# engine could use any toolchain. This script pins both to Qt's own
# llvm-mingw so the ABI matches Qt exactly.
#
# Requirements:
#   Qt 6.2+ for llvm-mingw (QT_DIR, default C:\Qt\6.10.0\llvm-mingw_64)
#   Qt's llvm-mingw toolchain  (MINGW_DIR, default C:\Qt\Tools\llvm-mingw1706_64)
# ============================================================================
param(
    [string]$QtDir = $env:QT_DIR,
    [string]$MingwDir = $env:MINGW_DIR,
    [string]$BuildDir = "build-win",
    [switch]$NoGui,
    [switch]$WithGuiConsole,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

if (-not $QtDir) { $QtDir = "C:\Qt\6.10.0\llvm-mingw_64" }
if (-not $MingwDir) { $MingwDir = "C:\Qt\Tools\llvm-mingw1706_64" }

$clang = Join-Path $MingwDir "bin\x86_64-w64-mingw32-clang.exe"
$clangxx = Join-Path $MingwDir "bin\x86_64-w64-mingw32-clang++.exe"
$moc = Join-Path $QtDir "bin\moc.exe"

foreach ($tool in @($clang, $clangxx, $moc)) {
    if (-not (Test-Path $tool)) { throw "missing tool: $tool" }
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $objDir = Join-Path $BuildDir "obj"
    $guiObjDir = Join-Path $BuildDir "gui"
    New-Item -ItemType Directory -Force $objDir, $guiObjDir | Out-Null

    # ---------------------------------------------------------------- engine
    #
    # Two variants of the same sources:
    #   console -> no MINIEAP_EMBEDDED: minieap.c is not compiled and the
    #              engine keeps calling the real exit(), like the original
    #              single-binary client.
    #   GUI     -> MINIEAP_EMBEDDED, exit() redirected to the bridge so a
    #              failing engine unwinds instead of killing the application.
    $baseDefs = @(
        "-DPKT_PLUGIN_RJV3_ENABLED", "-DPKT_PLUGIN_PRINTER_ENABLED",
        "-DIF_IMPL_NPCAP_ENABLED", "-D_WIN32_WINNT=0x0A00"
    )
    $guiEngineDefs = $baseDefs + @("-DMINIEAP_EMBEDDED", "-Dexit=minieap_embedded_exit")
    $includes = @("-Iinclude", "-Ipacket_plugin/rjv3/rjv3_hashes")

    # The engine sources, minus platform specific ones that do not apply here.
    # src/console and src/bridge supply their own main()/glue and are added
    # separately below.
    $engineSources = @(Get-ChildItem -Recurse -Filter *.c |
        Where-Object {
            $_.FullName -notmatch "\\build" -and
            $_.FullName -notmatch "\\src\\" -and
            $_.Name -notin @("net_util.c", "if_impl_sockraw.c", "if_impl_bpf.c",
                             "if_impl_libpcap.c")
        } | ForEach-Object { $_.FullName })
    # The wpcap loader is host glue, not an engine directory file
    $engineSources += (Resolve-Path "src\bridge\pcap_dyn.c").Path

    # The GUI front end is C++ (clang++), everything else is C (clang). Both
    # must come from the same llvm-mingw so the C ABI matches.
    $guiCxxSources = @(Get-ChildItem gui -Filter *.cpp | ForEach-Object { $_.FullName })

    function Build-EngineObjects([string]$suffix, [string[]]$compileDefs) {
        $objects = @()
        $n = 0
        foreach ($source in $engineSources) {
            $n++
            $name = [System.IO.Path]::GetFileNameWithoutExtension($source)
            $object = Join-Path $objDir ("$suffix{0:D2}_{1}.o" -f $n, $name)
            & $clang -c -w @compileDefs @includes $source -o $object
            if ($LASTEXITCODE -ne 0) { throw "failed to compile $source" }
            $objects += $object
        }
        return $objects
    }

    # ------------------------------------------------------- console client
    #
    # Without MINIEAP_EMBEDDED, minieap.c keeps its own main() (the original
    # single-binary program) and the engine keeps calling the real exit(), so
    # src/console/main_console.c is not part of this variant.
    $consoleObjects = Build-EngineObjects "con_" $baseDefs
    $consoleExe = Join-Path $BuildDir "minieap.exe"
    & $clangxx -o $consoleExe @consoleObjects -lws2_32 -liphlpapi
    if ($LASTEXITCODE -ne 0) { throw "console link failed" }
    Write-Host "built $consoleExe ($($consoleObjects.Count) engine objects)"

    if ($NoGui) {
        return
    }

    # ------------------------------------------------------ GUI engine copy
    $engineObjects = Build-EngineObjects "gui_" $guiEngineDefs
    $bridgeObj = Join-Path $objDir "gui_bridge.o"
    & $clang -c -w @guiEngineDefs @includes "src\bridge\bridge.c" -o $bridgeObj
    if ($LASTEXITCODE -ne 0) { throw "failed to compile the bridge" }
    $engineObjects += $bridgeObj
    Write-Host "engine (GUI): $($engineObjects.Count) objects"

    # ------------------------------------------------------------------- GUI
    $qtModules = @("Core", "Gui", "Widgets", "Network")
    $qtIncludes = @("-I$QtDir/include", "-I$MingwDir/include/c++/v1")
    foreach ($module in $qtModules) {
        $qtIncludes += "-I$QtDir/include/Qt$module"
    }

    # The version comes from gui/appversion.h so no quoted -D is needed here
    $guiDefs = @(
        "-DMINIEAP_EMBEDDED", "-D_WIN32_WINNT=0x0A00", "-DNOMINMAX",
        "-DUNICODE", "-D_UNICODE"
    )

    # moc for every header that declares Q_OBJECT
    foreach ($header in @("mainwindow", "enginebridge", "profiledialog")) {
        & $moc -Iinclude -Igui "gui\$header.h" -o (Join-Path $guiObjDir "moc_$header.cpp")
        if ($LASTEXITCODE -ne 0) { throw "moc failed for $header.h" }
    }

    $guiSources = @(
        "gui\main.cpp", "gui\mainwindow.cpp", "gui\enginebridge.cpp",
        "gui\profile.cpp", "gui\profiledialog.cpp", "gui\netinterfaces.cpp",
        (Join-Path $guiObjDir "moc_mainwindow.cpp"),
        (Join-Path $guiObjDir "moc_enginebridge.cpp"),
        (Join-Path $guiObjDir "moc_profiledialog.cpp")
    )

    $guiObjects = @()
    $i = 0
    foreach ($source in $guiSources) {
        $i++
        $object = Join-Path $guiObjDir ("g$i.o")
        & $clangxx -c -std=c++17 -w @guiDefs @qtIncludes -Iinclude -Igui $source -o $object
        if ($LASTEXITCODE -ne 0) { throw "failed to compile $source" }
        $guiObjects += $object
    }
    Write-Host "gui: $($guiObjects.Count) objects"

    $exe = Join-Path $BuildDir "minieap-qt.exe"
    & $clangxx -o $exe -mwindows @guiObjects @engineObjects `
        -L"$QtDir/lib" -L"$MingwDir/x86_64-w64-mingw32/lib" `
        -lQt6Widgets -lQt6Gui -lQt6Core -lQt6Network `
        -lws2_32 -liphlpapi -lole32 -lshell32 -luuid -luserenv -lversion -luxtheme -ldwmapi
    if ($LASTEXITCODE -ne 0) { throw "link failed" }
    Write-Host "built $exe"

    # Console variant of the Qt front end: same GUI, but attached to the
    # console for logs. Useful for support ("minieap-qt-console.exe") and for
    # automated checks; the direct engine bridge makes it GUI only.
    if ($WithGuiConsole) {
        $consoleExeQt = Join-Path $BuildDir "minieap-qt-console.exe"
        & $clangxx -o $consoleExeQt @guiObjects @engineObjects `
            -L"$QtDir/lib" -L"$MingwDir/x86_64-w64-mingw32/lib" `
            -lQt6Widgets -lQt6Gui -lQt6Core -lQt6Network `
            -lws2_32 -liphlpapi -lole32 -lshell32 -luuid -luserenv -lversion -luxtheme -ldwmapi
        if ($LASTEXITCODE -ne 0) { throw "console GUI link failed" }
        Write-Host "built $consoleExeQt"
    }

    # ------------------------------------------------------- runtime files
    # windeployqt cannot be used here (it shells out), so copy what the app
    # needs next to the executable. The same layout is what the CI artifact
    # and the release zip contain.
    foreach ($dll in @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll")) {
        Copy-Item (Join-Path $QtDir "bin\$dll") $BuildDir -Force
    }
    foreach ($dll in @("libc++.dll", "libunwind.dll", "libwinpthread-1.dll")) {
        Copy-Item (Join-Path $MingwDir "bin\$dll") $BuildDir -Force
    }
    $platformDir = Join-Path $BuildDir "platforms"
    New-Item -ItemType Directory -Force $platformDir | Out-Null
    Copy-Item (Join-Path $QtDir "plugins\platforms\qwindows.dll") $platformDir -Force
    Copy-Item (Join-Path $QtDir "plugins\platforms\qminimal.dll") $platformDir -Force
    Write-Host "runtime files copied to $BuildDir"
}
finally {
    Pop-Location
}
