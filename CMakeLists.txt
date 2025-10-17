cmake_minimum_required(VERSION 3.15)
project(corvus-sv-compiler VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add slang submodule
add_subdirectory(external/slang)

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/external/slang/include
)

# Source files
set(SOURCES
    src/main.cpp
)

# Create executable
add_executable(corvus-sv-compiler ${SOURCES})

# Link against slang
target_link_libraries(corvus-sv-compiler PRIVATE slang::slang)

# Installation rules
install(TARGETS corvus-sv-compiler DESTINATION bin)
