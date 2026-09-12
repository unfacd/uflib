add_c_test(
    NAME defensive_strlen_minmax_tests
    TEST_SOURCE defensive_strlen_minmax_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)
add_c_test(
    NAME email_tests
    TEST_SOURCE email_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)
add_c_test(
    NAME utf8_tests
    TEST_SOURCE utf8_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)
add_c_test(
    NAME url_tests
    TEST_SOURCE url_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
add_c_test(
    NAME utils_hex_tests
    TEST_SOURCE utils_hex_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
add_c_test(
    NAME utils_base64_tests
    TEST_SOURCE utils_base64_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
add_c_test(
    NAME utils_base32_tests
    TEST_SOURCE utils_base32_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib m
)
add_c_test(
    NAME utils_crypto_tests
    TEST_SOURCE utils_crypto_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib resolv ${UTF8PROC_LIBRARY}
)
add_c_test(
    NAME utils_collection_str_tests
    TEST_SOURCE utils_collection_str_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
add_c_test(
    NAME utils_base64url_tests
    TEST_SOURCE utils_base64url_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)

# ── Standalone test executable (build only, not registered with CTest) ──

add_executable(utils_base64_test_standalone
    utils_base64_test_standalone.c
)
target_include_directories(utils_base64_test_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(utils_base64_test_standalone PRIVATE
    uflib::uflib m
)
ufsrv_apply_build_options(utils_base64_test_standalone)