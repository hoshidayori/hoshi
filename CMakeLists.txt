cmake_minimum_required(VERSION 3.24)
project(hoshi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HOSHI_SANITIZE "Build with AddressSanitizer and UBSan" ON)

add_library(hoshi_core
    core/json.cpp
    core/sync.cpp
    core/client.cpp
    core/unix_http.cpp
)
target_include_directories(hoshi_core PUBLIC core)
target_compile_options(hoshi_core PRIVATE -Wall -Wextra -Werror)

if(HOSHI_SANITIZE)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
    target_compile_options(hoshi_core PUBLIC -fsanitize=address,undefined)
    target_link_options(hoshi_core PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()

add_executable(json_test core/json_test.cpp)
target_link_libraries(json_test PRIVATE hoshi_core)
target_compile_options(json_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME json_test COMMAND json_test)

add_executable(sync_test core/sync_test.cpp)
target_link_libraries(sync_test PRIVATE hoshi_core)
target_compile_options(sync_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME sync_test COMMAND sync_test)

add_executable(client_test core/client_test.cpp)
target_link_libraries(client_test PRIVATE hoshi_core)
target_compile_options(client_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME client_test COMMAND client_test)

# Needs the local test Synapse running (synapse/start.sh); not part of the default test run
add_executable(e2e_test core/e2e_test.cpp)
target_link_libraries(e2e_test PRIVATE hoshi_core)
target_compile_options(e2e_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME e2e_test COMMAND e2e_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/synapse/data)
set_tests_properties(e2e_test PROPERTIES LABELS synapse)
