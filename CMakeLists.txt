cmake_minimum_required(VERSION 3.24)
project(hoshi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(HOSHI_IOS ON)
endif()

# Sanitizers are for the desktop tests; an iOS app build does not use them
if(HOSHI_IOS)
    option(HOSHI_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
else()
    option(HOSHI_SANITIZE "Build with AddressSanitizer and UBSan" ON)
endif()

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

# Cryptography on top of OpenSSL's libcrypto. The desktop tests use the system's OpenSSL 3; the iOS app
# links a static libcrypto built from source by third_party/build-openssl-ios.sh.
if(HOSHI_IOS)
    # The sysroot is "iphonesimulator" or a path like .../iPhoneSimulator26.5.sdk
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(HOSHI_OPENSSL ${CMAKE_SOURCE_DIR}/third_party/openssl/iphonesimulator)
    else()
        set(HOSHI_OPENSSL ${CMAKE_SOURCE_DIR}/third_party/openssl/iphoneos)
    endif()
    if(NOT EXISTS ${HOSHI_OPENSSL}/lib/libcrypto.a)
        message(FATAL_ERROR "No OpenSSL for iOS in ${HOSHI_OPENSSL}: run third_party/build-openssl-ios.sh")
    endif()
    add_library(hoshi_openssl STATIC IMPORTED)
    set_target_properties(hoshi_openssl PROPERTIES
        IMPORTED_LOCATION ${HOSHI_OPENSSL}/lib/libcrypto.a
        INTERFACE_INCLUDE_DIRECTORIES ${HOSHI_OPENSSL}/include)
else()
    find_package(OpenSSL 3 REQUIRED)
    add_library(hoshi_openssl ALIAS OpenSSL::Crypto)
endif()
add_library(hoshi_crypto core/crypto.cpp core/megolm.cpp core/olm.cpp core/e2ee.cpp core/sas.cpp core/verification.cpp)
target_include_directories(hoshi_crypto PUBLIC core)
target_link_libraries(hoshi_crypto PUBLIC hoshi_openssl hoshi_core)
target_compile_options(hoshi_crypto PRIVATE -Wall -Wextra -Werror)

if(HOSHI_IOS)
    # Simulator build without Xcode (works in a sandbox that keeps ~/Library read-only):
    # cmake -S . -B build-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
    enable_language(OBJCXX)
    add_executable(Hoshi MACOSX_BUNDLE
        app/main.mm
        app/AppRouter.mm
        app/Credentials.mm
        app/DevicesViewController.mm
        app/LoginViewController.mm
        app/RoomsViewController.mm
        app/TimelineText.mm
        app/TimelineViewController.mm
        app/Session.mm
        app/UrlSessionHttp.mm
        app/VerificationViewController.mm
    )
    target_include_directories(Hoshi PRIVATE app)
    target_link_libraries(Hoshi PRIVATE "-framework Security")
    target_link_libraries(Hoshi PRIVATE hoshi_crypto hoshi_core "-framework UIKit" "-framework Foundation" "-framework CoreGraphics" "-framework Security")
    target_compile_options(Hoshi PRIVATE -fobjc-arc -Wall -Wextra -Werror)
    set_target_properties(Hoshi PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/app/Info.plist
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.hoshidayori.hoshi"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.github.hoshidayori.hoshi"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    )
    return()
endif()

enable_testing()

add_executable(crypto_test core/crypto_test.cpp)
target_link_libraries(crypto_test PRIVATE hoshi_crypto)
target_compile_options(crypto_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME crypto_test COMMAND crypto_test)

add_executable(megolm_test core/megolm_test.cpp)
target_link_libraries(megolm_test PRIVATE hoshi_crypto hoshi_core)
target_compile_options(megolm_test PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(megolm_test PRIVATE HOSHI_TESTDATA="${CMAKE_SOURCE_DIR}/core/testdata")
add_test(NAME megolm_test COMMAND megolm_test)

add_executable(olm_test core/olm_test.cpp)
target_link_libraries(olm_test PRIVATE hoshi_crypto)
target_compile_options(olm_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME olm_test COMMAND olm_test)

add_executable(verification_test core/verification_test.cpp)
target_link_libraries(verification_test PRIVATE hoshi_crypto hoshi_core)
target_compile_options(verification_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME verification_test COMMAND verification_test)

add_executable(e2ee_test core/e2ee_test.cpp)
target_link_libraries(e2ee_test PRIVATE hoshi_crypto hoshi_core)
target_compile_options(e2ee_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME e2ee_test COMMAND e2ee_test)

# Driven by interop/olm_interop.py
add_executable(olm_tool core/olm_tool.cpp)
target_link_libraries(olm_tool PRIVATE hoshi_crypto)
target_compile_options(olm_tool PRIVATE -Wall -Wextra -Werror)

# Driven by interop/sas_interop.py
add_executable(sas_tool core/sas_tool.cpp)
target_link_libraries(sas_tool PRIVATE hoshi_crypto)
target_compile_options(sas_tool PRIVATE -Wall -Wextra -Werror)

# Compatibility with vodozemac, when its Python bindings are installed in interop/venv
set(HOSHI_INTEROP_PYTHON ${CMAKE_SOURCE_DIR}/interop/venv/bin/python)
if(EXISTS ${HOSHI_INTEROP_PYTHON})
    add_test(NAME olm_interop COMMAND ${HOSHI_INTEROP_PYTHON} ${CMAKE_SOURCE_DIR}/interop/olm_interop.py $<TARGET_FILE:olm_tool>)
    # With Element downloaded by third_party/serve-element.sh, its emoji table is checked too
    file(GLOB HOSHI_ELEMENT_BUNDLE ${CMAKE_SOURCE_DIR}/third_party/element/element-v*/bundles/*/init.js)
    add_test(NAME sas_interop COMMAND ${HOSHI_INTEROP_PYTHON} ${CMAKE_SOURCE_DIR}/interop/sas_interop.py $<TARGET_FILE:sas_tool>
        ${HOSHI_ELEMENT_BUNDLE})
    set_tests_properties(sas_interop PROPERTIES LABELS interop)
    add_test(NAME megolm_interop COMMAND sh -c "$<TARGET_FILE:megolm_tool> megolm_hoshi.json && ${HOSHI_INTEROP_PYTHON} ${CMAKE_SOURCE_DIR}/interop/megolm_vectors.py check megolm_hoshi.json")
    set_tests_properties(olm_interop megolm_interop PROPERTIES LABELS interop)
endif()

# Writes a session for interop/megolm_vectors.py check
add_executable(megolm_tool core/megolm_tool.cpp)
target_link_libraries(megolm_tool PRIVATE hoshi_crypto hoshi_core)
target_compile_options(megolm_tool PRIVATE -Wall -Wextra -Werror)

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
target_compile_definitions(e2e_test PRIVATE HOSHI_TESTDATA="${CMAKE_SOURCE_DIR}/core/testdata")
add_test(NAME e2e_test COMMAND e2e_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/synapse/data)
set_tests_properties(e2e_test PROPERTIES LABELS synapse)

add_executable(e2ee_e2e_test core/e2ee_e2e_test.cpp)
target_link_libraries(e2ee_e2e_test PRIVATE hoshi_crypto hoshi_core)
target_compile_options(e2ee_e2e_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME e2ee_e2e_test COMMAND e2ee_e2e_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/synapse/data)
set_tests_properties(e2ee_e2e_test PROPERTIES LABELS synapse)
