# RFC 6455 frame codec — functional, boundary and adversarial coverage.

add_c_test(
    NAME uf_ws_frame_tests
    TEST_SOURCE ws_frame_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/uflib
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)

# RFC 6455 message reassembly — end-to-end over the frame codec.
add_c_test(
    NAME uf_ws_message_tests
    TEST_SOURCE ws_message_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/uflib
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)

# RFC 6455 closing handshake and error mapping.
add_c_test(
    NAME uf_ws_close_tests
    TEST_SOURCE ws_close_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/uflib
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)

# RFC 6455 opening handshake: key, accept and reply validation.
add_c_test(
    NAME uf_ws_handshake_tests
    TEST_SOURCE ws_handshake_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/uflib
    LIBRARIES uflib::uflib ${UTF8PROC_LIBRARY}
)
