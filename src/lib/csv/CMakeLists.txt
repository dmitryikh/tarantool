set(lib_sources
    csv.c
)

set_source_files_compile_flags(${lib_sources})
add_library(csv STATIC ${lib_sources})

if(ENABLE_FUZZER)
    set(TestName "test_csv")
    add_executable(${TestName} ${TestName}.c)
    set_target_properties(${TestName}
            PROPERTIES
            COMPILE_FLAGS "-fsanitize=fuzzer,address -g -O1"
            LINK_FLAGS "-fsanitize=fuzzer,address")
    target_link_libraries(${TestName} PRIVATE csv)
    set_target_properties(${TestName} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")
endif()
