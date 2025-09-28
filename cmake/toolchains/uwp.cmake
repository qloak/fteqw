# Toolchain helpers for building FTEQW as a Windows Store (UWP) application.
#
# Invoke via CMake presets or pass -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/uwp.cmake.
# The toolchain expects to be used with the MSVC toolset provided by Visual Studio
# and the UWP workload.

set(CMAKE_SYSTEM_NAME WindowsStore)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Ensure the WinRT-specific code paths are enabled.
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} /DWINRT")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} /DWINRT /EHsc")

# Prefer modern language standards when building with MSVC.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
