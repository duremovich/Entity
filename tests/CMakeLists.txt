# Unit Tests

find_package(GTest CONFIG REQUIRED)

add_executable(EntityMediaTests
    unit/FrameCacheTests.cpp
    unit/KeyframeRoundtripTests.cpp
    unit/RippleTimeTests.cpp
    unit/DeleteClipUndoTests.cpp
    unit/CueTagTests.cpp
    unit/SectionBreakTests.cpp
    unit/SectionFadeTests.cpp
    unit/SectionAutoColorTests.cpp
    unit/PostBreakAnchorTests.cpp
    unit/PlaybackPresenterFadeCacheTests.cpp
    unit/ClipPlaybackPhaseTests.cpp
    unit/HapFormatTests.cpp
    unit/TranscodeManagerTests.cpp
    unit/SettingsTests.cpp
    unit/OcioManagerTests.cpp
    unit/OcioGpuProcessorTests.cpp
    unit/RuntimeShaderCompilerTests.cpp
    unit/DecoderColorSpaceTests.cpp
    unit/ProjectSerializerTests.cpp
    unit/ProjectManagerPathTests.cpp
    unit/ProjectManagerCollectTests.cpp
    unit/ProjectManagerBundleTests.cpp
    unit/ProjectManagerRecoveryTests.cpp
    unit/ProjectManagerCloseTests.cpp
    unit/RecentProjectsTests.cpp
    unit/MediaVersioningTests.cpp
    unit/ContentScannerTests.cpp
    unit/OcioOdtTests.cpp
    unit/MessageBusSerializationTests.cpp
    unit/InMemoryTransportTests.cpp
    unit/SceneStateTests.cpp
    unit/PlaybackTimeAuthorityTests.cpp
    unit/DirectorRendererRoundtripTests.cpp
    unit/AssetIdTests.cpp
    unit/DecodeBufferPoolTests.cpp
    unit/CalibrationSolverTests.cpp
    unit/WaitForGpuEarlyOutTests.cpp
    test_Decoder.cpp
)

target_link_libraries(EntityMediaTests
    PRIVATE
        EntityMediaCore
        GTest::gtest
        GTest::gtest_main
        Snappy::snappy
        Eigen3::Eigen
)

include(GoogleTest)
gtest_discover_tests(EntityMediaTests)

# Dev-time fixture regeneration tool (not wired into CTest).
# Pure FFmpeg consumers — gate on HAVE_FFMPEG and attach FFmpeg explicitly.
# EntityMediaCore exposes FFmpeg as PRIVATE so its includes don't propagate;
# locally vcpkg's global include path masks this, but CI uses FFMPEG_ROOT
# (gyan.dev shared build) which lives outside any global search path.
if(HAVE_FFMPEG)
    add_executable(generate_hap_fixture tools/generate_hap_fixture.cpp)
    target_link_libraries(generate_hap_fixture PRIVATE EntityMediaCore ${FFMPEG_LIBRARIES})
    target_include_directories(generate_hap_fixture PRIVATE ${FFMPEG_INCLUDE_DIRS})

    add_executable(probe_codecs tools/probe_codecs.cpp)
    target_link_libraries(probe_codecs PRIVATE EntityMediaCore ${FFMPEG_LIBRARIES})
    target_include_directories(probe_codecs PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()

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
    # ENTITY_FORCE_WARP locks D3D12 to the software rasterizer so golden hashes
    # match across machines (hardware GPUs produce different bit patterns for
    # the same draw calls). Goldens under tests/goldens/ are recorded against
    # WARP — running these tests on a hardware adapter will mismatch.
    set_tests_properties(integration_${name} PROPERTIES
        LABELS "integration"
        TIMEOUT 60
        ENVIRONMENT "ENTITY_FORCE_WARP=1"
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
entity_add_integration_test(hap_hdr_smoke        hap_hdr_smoke.json)
entity_add_integration_test(aces_smoke           aces_smoke.json)
entity_add_integration_test(cache_hit_after_seek cache_hit_after_seek.json)
entity_add_integration_test(cache_budget_stress  cache_budget_stress.json)
# Phase D entry, subtask 8 -- per-tick Director->Renderer bus loop end-to-end.
entity_add_integration_test(director_renderer_smoke director_renderer_smoke.json)

# Screen-persistence round-trip: save runs first (FIXTURES_SETUP), produces
# test_output/screen_persistence/test.entity; load consumes it and asserts.
# Running just the load test re-runs the save automatically.
entity_add_integration_test(screen_persistence_save screen_persistence_save.json)
entity_add_integration_test(screen_persistence_load screen_persistence_load.json)
set_tests_properties(integration_screen_persistence_save PROPERTIES
    FIXTURES_SETUP screen_persistence_data)
set_tests_properties(integration_screen_persistence_load PROPERTIES
    FIXTURES_REQUIRED screen_persistence_data)

# Sections persistence round-trip (Phase B — break-points schema).
entity_add_integration_test(sections_persistence_save sections_persistence_save.json)
entity_add_integration_test(sections_persistence_load sections_persistence_load.json)
set_tests_properties(integration_sections_persistence_save PROPERTIES
    FIXTURES_SETUP sections_persistence_data)
set_tests_properties(integration_sections_persistence_load PROPERTIES
    FIXTURES_REQUIRED sections_persistence_data)

# Sections legacy migration round-trip — covers the deprecated AddSection
# alias path that emits two break-points per region (start + end).
entity_add_integration_test(sections_legacy_migration_save sections_legacy_migration_save.json)
entity_add_integration_test(sections_legacy_migration_load sections_legacy_migration_load.json)
set_tests_properties(integration_sections_legacy_migration_save PROPERTIES
    FIXTURES_SETUP sections_legacy_migration_data)
set_tests_properties(integration_sections_legacy_migration_load PROPERTIES
    FIXTURES_REQUIRED sections_legacy_migration_data)

# Phase B — section break auto-pause + GO end-to-end.
entity_add_integration_test(section_pause_locked    section_pause_locked.json)
entity_add_integration_test(section_go_keyboard     section_go_keyboard.json)
entity_add_integration_test(section_keyframe_freeze section_keyframe_freeze.json)
# Manual-resume unstick — regression guard for the at-break latch
# clearing when the user clicks Play instead of pressing GO.
entity_add_integration_test(section_manual_resume   section_manual_resume.json)

# Phase C — Normal continuation phase (Loop / PingPong cycling at-break)
# plus Locked-overrides-PlaybackMode regression guard.
entity_add_integration_test(section_loop_normal     section_loop_normal.json)
entity_add_integration_test(section_pingpong_normal section_pingpong_normal.json)
entity_add_integration_test(section_locked_clip     section_locked_clip.json)
# Regression guard: flipping Normal -> Locked while parked at a break
# must stop the continuation cycle (mapToMediaFrame re-checks
# sectionBehavior on every call, not just at seed time).
entity_add_integration_test(section_normal_to_locked_at_break section_normal_to_locked_at_break.json)

# Phase D — section fade envelopes auto-applied at break boundaries.
entity_add_integration_test(section_fade_in       section_fade_in.json)
entity_add_integration_test(section_fade_out      section_fade_out.json)
entity_add_integration_test(section_fade_combined section_fade_combined.json)

# Round-2 fixup, Phase 4 — post-break anchor model. Spanning Normal-mode
# Loop clip resumes from where the user was watching at GO, not from
# the natural clipStart-derived rewind.
entity_add_integration_test(post_break_no_rewind  post_break_no_rewind.json)

# Round-3 fixup, Phase 1 — manual seek clears Timeline::sectionAtBreak().
# Park at break, scrub backward, hit Play; the at-break latch must drop so
# subsequent playback advances normally.
entity_add_integration_test(scrub_clears_at_break  scrub_clears_at_break.json)

# Round-3 fixup, Phase 1 — first-section playback (issue #3 regression guard).
# A clip entirely inside the first section must play normally on Play; the
# Phase 5 visibility gate must NOT fire when the playhead isn't at a break.
entity_add_integration_test(first_section_plays  first_section_plays.json)

# Phase 6 / queued-at-break — clip whose start aligns with a section break
# is queued waiting for GO; first visible post-GO tick must map to
# clip.mediaStartFrame, not mediaStartFrame + 1. Companion scripts cover
# the in-point=0 baseline and the non-zero in-point case.
entity_add_integration_test(at_break_clip_starts_at_inpoint        at_break_clip_starts_at_inpoint.json)
entity_add_integration_test(at_break_clip_starts_at_inpoint_offset at_break_clip_starts_at_inpoint_offset.json)

# Cue tag fire + persistence round-trip (Phase A — numbered timeline markers).
entity_add_integration_test(cue_fire cue_fire.json)
entity_add_integration_test(cues_persistence_save cues_persistence_save.json)
entity_add_integration_test(cues_persistence_load cues_persistence_load.json)
set_tests_properties(integration_cues_persistence_save PROPERTIES
    FIXTURES_SETUP cues_persistence_data)
set_tests_properties(integration_cues_persistence_load PROPERTIES
    FIXTURES_REQUIRED cues_persistence_data)

# Stage 3c: show-thread-keeps-advancing while editor is stalled.
# No pixel-hash comparison so no ENTITY_FORCE_WARP needed.
add_test(
    NAME integration_show_thread_advances
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/show_thread_advances.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_show_thread_advances PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# Regression gate: AddScreen must not double-allocate compose targets.
# If the bug recurs the GPU wedges, DeviceLost is posted, the show thread
# stalls, and the frame-count assertion fires before Exit.
entity_add_integration_test(screen_no_double_allocation screen_no_double_allocation.json)

# Phase 1 mesh-upload pacing: cold-start gate + one-per-tick steady-state cap.
# No pixel comparison needed; asserts upload counts directly.
entity_add_integration_test(mesh_upload_paces mesh_upload_paces.json)
