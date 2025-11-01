cmake_minimum_required(VERSION 3.20)
project(wolf-sv-compiler VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(external/slang)

# Source files
set(SOURCES
    src/elaborate.cpp
    src/main.cpp
)

# Create executable
add_executable(wolf-sv-compiler ${SOURCES})

# Link against slang
target_link_libraries(wolf-sv-compiler PRIVATE slang::slang)

target_include_directories(wolf-sv-compiler
    PRIVATE
        include
)

# Installation rules
install(TARGETS wolf-sv-compiler DESTINATION bin)
