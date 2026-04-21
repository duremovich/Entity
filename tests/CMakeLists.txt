# Unit Tests

find_package(GTest CONFIG REQUIRED)

add_executable(EntityMediaTests
    unit/FrameRingBufferTests.cpp
    unit/BufferSystemTests.cpp
    test_Decoder.cpp
)

target_link_libraries(EntityMediaTests
    PRIVATE
        EntityMediaCore
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(EntityMediaTests)

# ============================================================================
# Integration Tests (headless scripts driving the full app)
#
# Each test invokes EntityMediaEditor with --headless and a script. The script
# ends with CaptureHash commands that compare pixel-hashes against goldens
# under tests/goldens/. Exit-code propagation makes CTest fail on mismatch.
# ============================================================================

function(entity_add_integration_test name script)
    add_test(
        NAME integration_${name}
        COMMAND $<TARGET_FILE:EntityMediaEditor> --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/${script}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(integration_${name} PROPERTIES
        LABELS "integration"
        TIMEOUT 60
    )
endfunction()

entity_add_integration_test(smoke              smoke_compare.json)
entity_add_integration_test(multi_screen       multi_screen.json)
entity_add_integration_test(png_sequence_seek  png_sequence_seek.json)
entity_add_integration_test(seek_past_clip_end seek_past_clip_end.json)
entity_add_integration_test(mixed_fps          mixed_fps.json)
entity_add_integration_test(ping_pong          ping_pong.json)
entity_add_integration_test(blend              blend.json)

# Screen-persistence round-trip: save runs first (FIXTURES_SETUP), produces
# test_output/screen_persistence/test.entity; load consumes it and asserts.
# Running just the load test re-runs the save automatically.
entity_add_integration_test(screen_persistence_save screen_persistence_save.json)
entity_add_integration_test(screen_persistence_load screen_persistence_load.json)
set_tests_properties(integration_screen_persistence_save PROPERTIES
    FIXTURES_SETUP screen_persistence_data)
set_tests_properties(integration_screen_persistence_load PROPERTIES
    FIXTURES_REQUIRED screen_persistence_data)
