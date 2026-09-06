cmake_minimum_required(VERSION 3.20)
project(dsh-start-control LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dsh-start-control WIN32
    src/main.cpp
    src/resource.rc
)

target_compile_definitions(dsh-start-control PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(dsh-start-control PRIVATE -Os -ffunction-sections -fdata-sections -finput-charset=UTF-8 -fexec-charset=UTF-8)
target_link_options(dsh-start-control PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s)
target_link_libraries(dsh-start-control PRIVATE comctl32 shell32 shlwapi ole32 oleaut32 advapi32 user32 gdi32 comdlg32 iphlpapi ws2_32)

set_target_properties(dsh-start-control PROPERTIES
    OUTPUT_NAME "dsh-start-control"
    WIN32_EXECUTABLE TRUE
)
