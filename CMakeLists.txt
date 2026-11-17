cmake_minimum_required(VERSION 3.16.0)

# Native ESP-IDF, built through PlatformIO. src/ is the one application component; the vendored LVGL and the board
# support under lib/, and NightMareNetwork from lib_deps, are PlatformIO libraries compiled with the same flags.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

# Fixed so project.cmake never shells out to `git describe`; the real version string comes from include/Version.h.
set(PROJECT_VER "0.1.0")

# Build only what src/ depends on, directly or not, instead of every component ESP-IDF ships. Besides the time saved,
# this is what keeps the linker-script step working on Windows: its command line names one fragment file per component
# built, and with all of them it runs past cmd.exe's 8191-character limit.
set(COMPONENTS src __pio_env esptool_py bootloader partition_table)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Dashboard)
