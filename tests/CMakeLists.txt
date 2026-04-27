# Unit Tests

find_package(GTest CONFIG REQUIRED)

add_executable(EntityMediaTests
    unit/FrameCacheTests.cpp
    unit/KeyframeRoundtripTests.cpp
    unit/RippleTimeTests.cpp
    unit/HapFormatTests.cpp
    unit/TranscodeManagerTests.cpp
    unit/SettingsTests.cpp
    unit/OcioManagerTests.cpp
    unit/OcioGpuProcessorTests.cpp
    unit/RuntimeShaderCompilerTests.cpp
    test_Decoder.cpp
)

target_link_libraries(EntityMediaTests
    PRIVATE
        EntityMediaCore
        GTest::gtest
        GTest::gtest_main
        Snappy::snappy
)

include(GoogleTest)
gtest_discover_tests(EntityMediaTests)

# Dev-time fixture regeneration tool (not wired into CTest).
add_executable(generate_hap_fixture tools/generate_hap_fixture.cpp)
target_link_libraries(generate_hap_fixture PRIVATE EntityMediaCore)

add_executable(probe_codecs tools/probe_codecs.cpp)
target_link_libraries(probe_codecs PRIVATE EntityMediaCore)

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
entity_add_integration_test(blend_difference   blend_difference.json)
entity_add_integration_test(hap_roundtrip       hap_roundtrip.json)
entity_add_integration_test(hap_basic_roundtrip  hap_basic_roundtrip.json)
entity_add_integration_test(hap_q_roundtrip      hap_q_roundtrip.json)
entity_add_integration_test(cache_hit_after_seek cache_hit_after_seek.json)
entity_add_integration_test(cache_budget_stress  cache_budget_stress.json)

# Screen-persistence round-trip: save runs first (FIXTURES_SETUP), produces
# test_output/screen_persistence/test.entity; load consumes it and asserts.
# Running just the load test re-runs the save automatically.
entity_add_integration_test(screen_persistence_save screen_persistence_save.json)
entity_add_integration_test(screen_persistence_load screen_persistence_load.json)
set_tests_properties(integration_screen_persistence_save PROPERTIES
    FIXTURES_SETUP screen_persistence_data)
set_tests_properties(integration_screen_persistence_load PROPERTIES
    FIXTURES_REQUIRED screen_persistence_data)

# Sections persistence round-trip (Phase C #5).
entity_add_integration_test(sections_persistence_save sections_persistence_save.json)
entity_add_integration_test(sections_persistence_load sections_persistence_load.json)
set_tests_properties(integration_sections_persistence_save PROPERTIES
    FIXTURES_SETUP sections_persistence_data)
set_tests_properties(integration_sections_persistence_load PROPERTIES
    FIXTURES_REQUIRED sections_persistence_data)
