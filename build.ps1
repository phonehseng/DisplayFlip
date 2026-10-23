# Builds dist\DisplayFlip.exe (+ Stream Deck plugin files) with MinGW-w64 g++ (MSYS2 UCRT64).
#   powershell -ExecutionPolicy Bypass -File build.ps1            # release build
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Install   # build, then install for this user
param([switch]$Install, [string]$Toolchain = "C:\msys64\ucrt64\bin")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (Test-Path "$Toolchain\g++.exe") { $env:PATH = "$Toolchain;$env:PATH" }
$out = Join-Path $root "dist"
$obj = Join-Path $root "build"
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$flags = @("-std=c++17", "-O2", "-municode", "-mwindows", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00",
           "-DWINVER=0x0A00", "-DNTDDI_VERSION=0x0A00000C", "-Wall", "-Wno-unknown-pragmas", "-Wno-missing-field-initializers",
           "-Wno-cast-function-type", "-ffunction-sections", "-fdata-sections")
$libs = @("-static", "-s", "-Wl,--gc-sections", "-luser32", "-lgdi32", "-lshell32", "-lole32", "-loleaut32", "-luuid",
          "-ldwmapi", "-lshcore", "-lwinhttp", "-ladvapi32", "-lpropsys")

Push-Location (Join-Path $root "src")
try {
  & windres displayflip.rc -O coff -o "$obj\resources.o"
  if ($LASTEXITCODE) { throw "windres failed" }
  $srcs = Get-ChildItem *.cpp | ForEach-Object { $_.Name }
  & g++ @flags @srcs "$obj\resources.o" -o "$out\DisplayFlip.exe" @libs
  if ($LASTEXITCODE) { throw "compile failed" }
} finally { Pop-Location }

# Stream Deck plugin files travel next to the exe
$sd = Join-Path $out "streamdeck"
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
Copy-Item -Recurse (Join-Path $root "streamdeck") $sd
Copy-Item (Join-Path $root "README.md") $out -ErrorAction SilentlyContinue
$size = [math]::Round((Get-Item "$out\DisplayFlip.exe").Length / 1KB)
Write-Host "Built dist\DisplayFlip.exe ($size KB)"

if ($Install) { & "$out\DisplayFlip.exe" install | Out-String | Write-Host }
