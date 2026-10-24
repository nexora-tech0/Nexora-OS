cmake_minimum_required(VERSION 3.16)
project(NexoraBrowserCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nexora_browser_url user/apps/browser/core/url.cpp)
target_include_directories(nexora_browser_url PUBLIC user/apps/browser/core)

add_executable(url_parser_test tests/url_parser_test.cpp)
target_link_libraries(url_parser_test PRIVATE nexora_browser_url)

enable_testing()
add_test(NAME url_parser_test COMMAND url_parser_test)
