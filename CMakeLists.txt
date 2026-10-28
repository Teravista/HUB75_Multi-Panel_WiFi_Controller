# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Uncomment the following two lines to compile for bare RP2350 without a board
# set(PICO_PLATFORM rp2350)
# set(PICO_BOARD none CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(hub75 C CXX ASM)
option(HUB75_VERBOSE_LOGS "Enable verbose diagnostics and the startup serial delay" OFF)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

set(LOCAL_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/.env")
if(NOT EXISTS "${LOCAL_ENV_FILE}")
    message(FATAL_ERROR "Missing .env file. Copy .env.example to .env and fill in its values.")
endif()

set(NETWORK_CONFIG_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/src/config/networkConfig.h.in")
set(NETWORK_CONFIG_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateNetworkConfig.cmake")
set(NETWORK_CONFIG_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/networkConfig.h")

add_custom_command(
    OUTPUT "${NETWORK_CONFIG_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DENV_FILE=${LOCAL_ENV_FILE}"
            "-DTEMPLATE_FILE=${NETWORK_CONFIG_TEMPLATE}"
            "-DOUTPUT_FILE=${NETWORK_CONFIG_HEADER}"
            -P "${NETWORK_CONFIG_GENERATOR}"
    DEPENDS
            "${LOCAL_ENV_FILE}"
            "${NETWORK_CONFIG_TEMPLATE}"
            "${NETWORK_CONFIG_GENERATOR}"
    COMMENT "Generating firmware network configuration"
    VERBATIM
)

set(WEB_PAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
set(WEB_PAGE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/webPage.hpp")
add_custom_command(
    OUTPUT "${WEB_PAGE_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${WEB_PAGE_SOURCE}" "-DOUTPUT=${WEB_PAGE_HEADER}"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/EmbedWebPage.cmake"
    DEPENDS "${WEB_PAGE_SOURCE}" "${CMAKE_CURRENT_LIST_DIR}/cmake/EmbedWebPage.cmake"
            "${CMAKE_CURRENT_LIST_DIR}/cmake/webPage.hpp.in"
    COMMENT "Embedding media upload page"
    VERBATIM
)

# Add executable. Default name is the project name
add_executable(hub75 mainEntry.cpp)

pico_set_program_name(hub75 "hub75_demo")
pico_set_program_version(hub75 "2.0")

set(PICO_PIO_VERSION 1)
pico_set_float_implementation(hub75 pico)

set(PANEL_GENERIC 0)
set(PANEL_FM6126A 1)
set(PANEL_RUL6024 2)

# No need to modify preprocessor defines in hub75.cpp - instead set their values here.
#
# Example:
# Settings for a RP2350B microcontroller with GPIO pins spanning from 30 to 43.
# Beware to set `PICO_PLATFORM rp2350` and `PICO_BOARD none` prior to `include(pico_sdk_import.cmake)`
target_compile_definitions(hub75 PRIVATE
    PICO_RP2350A=0             # PICO_RP2350A=0` means not a RP2350A but a RP2350B microcontroller - uncomment for RP235xB microcontroller only!
    # USE_PICO_GRAPHICS=true      # set to false if you use hub75 as a library - any reference to pico_graphics is removed
    MATRIX_PANEL_WIDTH=512       # your matrix panel width
    MATRIX_PANEL_HEIGHT=64      # your matrix panel height
    # DATA_BASE_PIN=30            # base GPIO pin (aka start index) of R0, G0, B0, R1, G1, B1 GPIO pins
    # DATA_N_PINS=6               # number (count) of colour pins (usually 6)
    # ROWSEL_BASE_PIN=36          # base GPIO address pin (aka start index) of A, B (, C, D. E) GPIO pins
    # ROWSEL_N_PINS=5             # number (count) of address pins available on your matrix panel board (look at your panels connector)
    # CLK_PIN=41                  # GPIO pin for CLK
    # STROBE_PIN=42               # GPIO pin for STROBE (LATCH)
    # OEN_PIN=43                  # GPIO for OE pin
    # PANEL_TYPE=PANEL_GENERIC    # select PANEL_TYPE
    # INVERTED_STB=false          # inverted pin signal for OE (untested)
    # TEMPORAL_DITHERING=false    # experimental - switch on temporal dithering to improve colour depth (currently 2 additional bits)
    # SM_CLOCKDIV_FACTOR=1.0f     # to prevent flicker or ghosting it might be worth a try to reduce state machine speed
    # BIT_DEPTH=10                # numner (count) of bit-planes used for BCM (Binary Code Modulation) - valid values for BIT_DEPTH are 8 or 10
    CPU_FRAME_RGB565=1
    JPEG_PARALLEL_READER=1
    HUB75_MULTICORE=false      # Keep HUB75 on Core 0; application networking owns Core 1.
    PICO_CORE1_STACK_SIZE=0x1000
    PICO_STACK_SIZE=0x1000
    CYW43_PIO_CLOCK_DIV_INT=4  # compensate cyw43 wireless SPI PIO clock divider for the 250MHz overclocked sys clock (default div=2 assumes ~125MHz sys clock; div=4 keeps the wireless SPI PIO near the same safe ~62.5MHz)
)

# Generate PIO header
pico_generate_pio_header(hub75 ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src/hub75.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(hub75 0)
pico_enable_stdio_usb(hub75 1)

add_subdirectory(third_party/hub75/common)
add_subdirectory(third_party/hub75/libraries)
add_subdirectory(third_party/fatfs_sd/src)
target_compile_definitions(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE
    USE_PRINTF=$<BOOL:${HUB75_VERBOSE_LOGS}>
    USE_DBG_PRINTF=$<BOOL:${HUB75_VERBOSE_LOGS}>)

target_compile_definitions(hub75 PRIVATE HUB75_VERBOSE_LOGS=$<BOOL:${HUB75_VERBOSE_LOGS}>)

target_sources(hub75 PRIVATE
    ${NETWORK_CONFIG_HEADER}
    ${WEB_PAGE_HEADER}

    ${CMAKE_CURRENT_LIST_DIR}/src/core/Core1SchedulerClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/serialLog.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/display/FrameRateTrackerClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/display/PanelRendererClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/display/RenderPipelineClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/display/cpuFrame.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/network/FrameBufferControllerClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/FrameReceiverClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/NetworkServicesClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/TcpVideoUploadClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/WifiConnectionClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/PicoHotspotClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/clientActivity.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/network/networkCorePico.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/http/HttpMediaUploadClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/http/api/httpControl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/http/httpRequest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/http/webServer.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/media/JsonWriterClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/MediaLibraryClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/MediaPlayerClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/PhotoPersistenceClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/jpeg/JpegInputPipeClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/jpeg/jpegFrameDecoder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/jpeg/jpegWorkerPico.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/mediaMetadata.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/media/mediaTransfer.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/sd/MemoryManagerClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sd/PlaybackPrefetchClass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sd/memoryManagerPico.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sd/memoryStorage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sd/storageCrc32.cpp

    ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src/fm6126a.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src/hub75.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src/rgb565.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src/rul6024.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/tjpgd/tjpgd.c
)

# Add the standard library to the build
target_link_libraries(hub75
        pico_stdlib
        pico_multicore
        pico_aon_timer
        hardware_pio
        hardware_dma
        hardware_timer
        hardware_vreg
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
        pico_graphics)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(hub75
                          pico_cyw43_arch_lwip_threadsafe_background)
endif()

# Add the standard include files to the build
target_include_directories(hub75 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_LIST_DIR}/third_party/hub75/src
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/src/config
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(hub75)

# Optimize the CPU-intensive JPEG decoder while retaining size optimization elsewhere.
set_source_files_properties(third_party/tjpgd/tjpgd.c src/media/jpeg/jpegFrameDecoder.cpp
    PROPERTIES COMPILE_OPTIONS "-O3")
