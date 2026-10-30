cmake_minimum_required(VERSION 3.16)
project(FourierSketch VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(X11 REQUIRED)

add_library(fourier_core src/Fourier.cpp)
target_include_directories(fourier_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(fourier_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fourier-sketch src/main.cpp)
target_link_libraries(fourier-sketch PRIVATE fourier_core X11::X11)
target_compile_options(fourier-sketch PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(fourier-tests tests/FourierTests.cpp)
target_link_libraries(fourier-tests PRIVATE fourier_core)
target_compile_options(fourier-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME fourier-math COMMAND fourier-tests)

