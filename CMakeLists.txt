cmake_minimum_required(VERSION 3.16)
project(internetexploader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# GTK4 and WebKitGTK (the WebKit2 GTK4 API).
# Package name varies by distro:
#   - Ubuntu 24.04+/Debian 13+/Fedora 40+: webkitgtk-6.0
#   - Slightly older distros: webkit2gtk-4.1
# If the first one isn't found, CMake will fall back to the second.
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(WEBKITGTK webkitgtk-6.0)

if(NOT WEBKITGTK_FOUND)
    pkg_check_modules(WEBKITGTK REQUIRED webkit2gtk-4.1)
endif()

add_executable(internetexploader
    src/main.cpp
)

target_include_directories(internetexploader PRIVATE
    ${GTK4_INCLUDE_DIRS}
    ${WEBKITGTK_INCLUDE_DIRS}
)

target_compile_options(internetexploader PRIVATE
    ${GTK4_CFLAGS_OTHER}
    ${WEBKITGTK_CFLAGS_OTHER}
)

target_link_libraries(internetexploader PRIVATE
    ${GTK4_LIBRARIES}
    ${WEBKITGTK_LIBRARIES}
)

target_link_directories(internetexploader PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${WEBKITGTK_LIBRARY_DIRS}
)

# Copy the sites/ and LaunchAssets/ folders next to the built binary so
# the app finds them at runtime, the same way it looked for them next
# to the .exe on Windows.
add_custom_command(TARGET internetexploader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sites
        $<TARGET_FILE_DIR:internetexploader>/sites
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/LaunchAssets
        $<TARGET_FILE_DIR:internetexploader>/LaunchAssets
)
