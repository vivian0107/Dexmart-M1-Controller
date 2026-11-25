param([string]$BuildDirectory = "build/diagnostics")
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue
$gccCommand = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
$stPlugins = "C:/ST/STM32CubeIDE*/STM32CubeIDE/plugins"
if (-not $env:ARM_GCC_BIN) {
    if ($gccCommand) { $env:ARM_GCC_BIN = Split-Path -Parent $gccCommand.Source }
    else {
        $stGcc = Get-ChildItem "$stPlugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32*/tools/bin/arm-none-eabi-gcc.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $stGcc) { throw "Set ARM_GCC_BIN to the directory containing arm-none-eabi-gcc." }
        $env:ARM_GCC_BIN = $stGcc.DirectoryName
    }
}
if ($cmakeCommand) { $cmakeExe = $cmakeCommand.Source }
else {
    $stCmake = Get-ChildItem "$stPlugins/com.st.stm32cube.ide.mcu.externaltools.cmake*/tools/bin/cmake.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $stCmake) { throw "CMake was not found." }
    $cmakeExe = $stCmake.FullName
}
if ($ninjaCommand) { $ninjaExe = $ninjaCommand.Source }
else {
    $stNinja = Get-ChildItem "$stPlugins/com.st.stm32cube.ide.mcu.externaltools.ninja*/tools/bin/ninja.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $stNinja) { throw "Ninja was not found." }
    $ninjaExe = $stNinja.FullName
}
if ([IO.Path]::IsPathRooted($BuildDirectory)) { $outputDirectory = $BuildDirectory }
else { $outputDirectory = Join-Path $projectRoot $BuildDirectory }
& $cmakeExe -S $projectRoot -B $outputDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DCMAKE_TOOLCHAIN_FILE=$projectRoot/cmake/arm-none-eabi.cmake"
if ($LASTEXITCODE -ne 0) { throw "Firmware configure failed." }
& $cmakeExe --build $outputDirectory
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed." }
$symbols = & (Join-Path $env:ARM_GCC_BIN "arm-none-eabi-nm.exe") (Join-Path $outputDirectory "m1_diagnostics.elf")
if ($LASTEXITCODE -ne 0) { throw "Could not inspect firmware symbols." }
if ($symbols | Select-String '\b(m1_sdo_write|m1_nmt|m1_sync|m1_rpdo[123])$') {
    throw "A write/motion codec was unexpectedly linked into the read-only diagnostic image."
}
Write-Output "Read-only diagnostic ELF, HEX and BIN built in $outputDirectory"
