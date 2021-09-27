#https://notes.eatonphil.com/unit-testing-c-code-with-gtest.html
include_directories("${PROJECT_SOURCE_DIR}/include")

#file(GLOB sources "${PROJECT_SOURCE_DIR}/src/*.c")
set(_TEST_FILES_LIST ${TARGET_NAME}_source)
list(REMOVE_ITEM ${_TEST_FILES_LIST} "${PROJECT_SOURCE_DIR}/src/main.c")

file(GLOB tests "${PROJECT_SOURCE_DIR}/test/*.cpp")
list(REMOVE_ITEM tests "${PROJECT_SOURCE_DIR}/test/main.cpp")

foreach(file ${tests})
    set(name)
    get_filename_component(name ${file} NAME_WE)
    add_executable("${name}_tests"
            ${sources}
            ${file}
            "${PROJECT_SOURCE_DIR}/test/main.cpp")
    target_link_libraries("${name}_tests" uflib gtest_main)
    add_test(NAME ${name} COMMAND "${name}_tests")
endforeach()