add_library(doctest_main STATIC doctest-main.cpp)
target_link_libraries(doctest_main PUBLIC doctest::doctest)
target_compile_definitions(doctest_main PUBLIC DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

add_executable(photon_tests
        version_tests.cpp
        error_tests.cpp
        message_tests.cpp
        codec_tests.cpp
        type_codec_tests.cpp
        scram_tests.cpp
        dsn_tests.cpp
        binding_tests.cpp
        pipeline_tests.cpp
        named_tests.cpp
        integration_tests.cpp
        stress_tests.cpp
)
if (PHOTON_WITH_PRISM)
    target_sources(photon_tests PRIVATE prism_integration_tests.cpp)
    target_link_libraries(photon_tests PRIVATE prism::prism)
endif ()
target_link_libraries(photon_tests PRIVATE photon doctest_main doctest::doctest)

add_test(NAME photon_tests COMMAND photon_tests)
