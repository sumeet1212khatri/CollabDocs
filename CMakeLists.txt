cmake_minimum_required(VERSION 3.16)
project(collabdocs VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release build optimizations
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# ─── Find dependencies ────────────────────────────────────────────────────────

find_package(Boost 1.74 REQUIRED COMPONENTS system coroutine context)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.2 REQUIRED)

# ─── Executable ───────────────────────────────────────────────────────────────

add_executable(collabdocs main.cpp)

target_include_directories(collabdocs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(collabdocs
    Boost::system
    Boost::coroutine
    Boost::context
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    nlohmann_json::nlohmann_json
)

target_compile_options(collabdocs PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0 -fsanitize=address>
    $<$<CONFIG:Release>:-O2>
)

target_link_options(collabdocs PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address>
)

# ─── OT stress test ───────────────────────────────────────────────────────────

add_executable(ot_test Ot_test.cpp)

target_include_directories(ot_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(ot_test
    Threads::Threads
    nlohmann_json::nlohmann_json
)

target_compile_options(ot_test PRIVATE -Wall -O2)
