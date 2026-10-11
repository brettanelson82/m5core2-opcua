# Regenerate components/open62541 (single-file build of open62541 with mbedTLS
# encryption) and re-apply the clock patch. Windows PowerShell, ESP-IDF v5.5.
#
#   git clone --depth 1 --branch v1.5.8 https://github.com/open62541/open62541.git <src>
#   powershell -File tools\gen_open62541.ps1 -Src <src> -Idf C:\Espressif\v5.5.5\esp-idf
#
# Only the amalgamation targets are built, so nothing is compiled for real:
# the ESP32 compiler is only there for CMake's checks, and the FreeRTOS kernel
# is a stand-in folder.
param(
    [Parameter(Mandatory)] [string]$Src,      # open62541 source checkout (tag v1.5.8)
    [Parameter(Mandatory)] [string]$Idf,      # ESP-IDF folder, e.g. C:\Espressif\v5.5.5\esp-idf
    [string]$Activate = "",                   # ESP-IDF activation script (EIM installs one)
    [string]$Project = (Split-Path $PSScriptRoot -Parent)
)
$ErrorActionPreference = "Stop"
if ($Activate) { . $Activate *> $null }

$src = $Src.Replace('\', '/')
$idf = $Idf.Replace('\', '/')
$bld = "$src/build-esp"
$gcc = (Get-Command xtensa-esp32-elf-gcc).Source.Replace('\', '/')
# Use the ESP-IDF Python: CMake's own search can hit the Windows Store stub and hang
$py  = (Get-Command python).Source.Replace('\', '/')

# Stand-in FreeRTOS kernel: open62541's CMake wants a folder with a CMakeLists.txt
$stub = "$src/stub-freertos"
New-Item -ItemType Directory -Force "$stub/include" | Out-Null
Set-Content -Encoding ascii "$stub/CMakeLists.txt" "add_library(freertos_kernel INTERFACE)"

cmake -S $src -B $bld -G Ninja `
  "-DCMAKE_BUILD_TYPE=MinSizeRel" "-DCMAKE_SYSTEM_NAME=Generic" `
  "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY" `
  "-DPython3_EXECUTABLE=$py" `
  "-DUA_ARCHITECTURE=freertos-lwip" "-DUA_LWIP_EXTERNAL=ON" `
  "-DFREERTOS_KERNEL_PATH=$stub" "-DFREERTOS_PORT_PATH=$idf/components/freertos/FreeRTOS-Kernel/portable/xtensa" `
  "-DUA_ENABLE_AMALGAMATION=ON" "-DUA_NAMESPACE_ZERO=REDUCED" `
  "-DUA_ENABLE_ENCRYPTION=MBEDTLS" "-DMBEDTLS_INCLUDE_DIRS=$idf/components/mbedtls/mbedtls/include" `
  "-DMBEDTLS_LIBRARY=mbedtls" "-DMBEDX509_LIBRARY=mbedx509" "-DMBEDCRYPTO_LIBRARY=mbedcrypto" `
  "-DUA_ENABLE_PUBSUB=OFF" "-DUA_ENABLE_HISTORIZING=OFF" "-DUA_ENABLE_JSON_ENCODING=OFF" `
  "-DUA_ENABLE_XML_ENCODING=OFF" "-DUA_ENABLE_DISCOVERY_MULTICAST=OFF" "-DUA_ENABLE_DIAGNOSTICS=OFF" `
  "-DUA_ENABLE_SUBSCRIPTIONS=ON" "-DUA_ENABLE_METHODCALLS=ON" "-DUA_ENABLE_NODEMANAGEMENT=ON" `
  "-DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF" "-DUA_MULTITHREADING=0" "-DUA_LOGLEVEL=300"
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $bld --target open62541-amalgamation
if ($LASTEXITCODE) { throw "amalgamation build failed" }

$dst = "$Project/components/open62541"
Copy-Item "$bld/open62541.c" "$dst/open62541.c" -Force
Copy-Item "$bld/open62541.h" "$dst/include/open62541.h" -Force
& $py "$Project/tools/patch_amalgamation.py" "$dst/open62541.c"
Write-Host "Done. Rebuild the firmware (idf.py build)."
