add_library(http_parser STATIC http_parser.c)

if(ENABLE_FUZZER)
    set(TestName "test_http_parser")
    add_executable(${TestName} ${TestName}.c)
    set_target_properties(${TestName}
            PROPERTIES
            COMPILE_FLAGS "-fsanitize=fuzzer,address -g -O1"
            LINK_FLAGS "-fsanitize=fuzzer,address")
    target_link_libraries(${TestName} PRIVATE http_parser)
    set_target_properties(${TestName} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")
endif()
