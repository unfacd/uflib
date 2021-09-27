#configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)

#The PRIVATE, PUBLIC and INTERFACE keywords are about whether those particular files should be part of that target and/or part of a target that links to it
target_sources(${_PROJECT_NAME} PRIVATE
                tokeniser.c
                ufsrvuid.c
                utils.c
                utils_base32.c
                utils_base64.c
                utils_bits.c
                utils_crypto.c
                utils_file.c
                utils_hex.c
                utils_str.c
                utils_time.c
                utils_url.c
            )