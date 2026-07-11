# Unit Tests

find_package(GTest CONFIG REQUIRED)

add_executable(EntityMediaTests
    unit/FrameCacheTests.cpp
    unit/EditorFrameRingTests.cpp
    unit/EditorReadGateTests.cpp
    unit/TextureUploaderGenerationTests.cpp
    unit/KeyframeRoundtripTests.cpp
    unit/RippleTimeTests.cpp
    unit/TrackInsertTests.cpp
    unit/DeleteClipUndoTests.cpp
    unit/DeleteClipsCommandTests.cpp
    unit/ObjectAnimationLayerTests.cpp
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
    unit/EffectKindRegistryTests.cpp
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
    unit/CatalogClipMathTests.cpp
    unit/PrecompMathTests.cpp
    unit/OAShowSideReevalTests.cpp
    unit/DirectorRendererRoundtripTests.cpp
    unit/AssetIdTests.cpp
    unit/MathInputTests.cpp
    unit/DecodeBufferPoolTests.cpp
    unit/DecodeBufferAllocatorTests.cpp
    unit/DecodedFrameTests.cpp
    unit/CalibrationSolverTests.cpp
    unit/WaitForGpuEarlyOutTests.cpp
    unit/DmxMappingTests.cpp
    unit/OscEncodingTest.cpp
    unit/OscMappingTableTest.cpp
    unit/AudioRingBufferTest.cpp
    unit/AudioMixerTest.cpp
    unit/RateSourceSelectorTest.cpp
    unit/SeekSyncControllerTest.cpp
    unit/SignalEmitSerializationTest.cpp
    unit/SignalLayerTest.cpp
    unit/SignalSnapshotBakeTest.cpp
    unit/SignalOutputSystemTest.cpp
    unit/SignalCommandsTest.cpp
    unit/SignalPersistenceTest.cpp
    unit/RemoteControlStoreTest.cpp
    unit/RemotePatchUtilTest.cpp
    unit/SetLayerCommandTests.cpp
    unit/RenameTrackCommandTests.cpp
    unit/AlphaPremultiplyTests.cpp
    unit/WarmSetTests.cpp
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

# ============================================================================
# D3D12-device-requiring unit tests.
#
# Gated on ENTITY_HAVE_D3D12_TESTS (default OFF) so the standard ctest sweep
# stays clean on machines without a GPU / in headless CI. Set the option ON to
# build UploadHeapBufferPoolTests and run it against a real D3D12 device.
# Each individual test calls GTEST_SKIP() if D3D12CreateDevice fails.
# ============================================================================

if(ENTITY_HAVE_D3D12_TESTS)
    add_executable(UploadHeapBufferPoolTests
        unit/UploadHeapBufferPoolTests.cpp
    )
    target_compile_definitions(UploadHeapBufferPoolTests
        PRIVATE ENTITY_HAVE_D3D12_TESTS
    )
    target_link_libraries(UploadHeapBufferPoolTests
        PRIVATE
            EntityMediaCore
            GTest::gtest
            GTest::gtest_main
    )
    gtest_discover_tests(UploadHeapBufferPoolTests)
endif()

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
    #
    # RESOURCE_LOCK "d3d12_warp" serializes all integration tests against
    # each other even under `ctest -jN`: each spawns a full EntityMediaEditor
    # sharing one WARP device, the FrameCache budget, and CMAKE_SOURCE_DIR as
    # working directory (autosave/test_output artifacts). Without it,
    # parallel runs fail non-deterministically on timing-sensitive checks.
    # The lock is applied by the blanket foreach at the bottom of this file
    # (which also covers integration tests registered via raw add_test()).
    # Unit tests are unaffected and still parallelize.
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
# Parked-clip visibility: a clip dropped onto a PARKED (paused, never
# played) playhead inside its range must resolve its GPU slot + decode the
# parked source frame into FrameCache without a Play or seek. Guards the
# drainRendererToDirector-before-buildSceneSnapshot provisioning-latency
# fix and the broader "visible while parked, no Play/seek dependency"
# contract. Requires test_media/basic_loop.mov — generate once with
# scripts/perf/setup-test-media.ps1 (CI runs it before ctest).
entity_add_integration_test(park_then_import_visible park_then_import_visible.json)
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

# NEW-08 — section-break crossing detection runs on the show thread. Play
# past a break; the show-thread detector parks the playhead, the editor
# applies the crossing via handleBreakAt from the R2D drain; SectionGo resumes.
entity_add_integration_test(section_break_show_detect  section_break_show_detect.json)

# Fix 5 (2026-05-23 follow-up) — section-break tail-window held-frame
# regression. A clip with authored duration much shorter than its trimmed
# source range, ending exactly at a section break with fadeSeconds > 0,
# used to make the SeekSyncController preroll gate ask the cache for the
# trimmed source's last frame (which the decoder never reached), stalling
# the gate for its full 3000ms timeout on every Section GO. The held
# frame is now mapToMediaFrame(clip, clipEnd - 1) — what was on screen
# at the last authored timeline frame — which the worker has already
# decoded, so the gate releases on the first tick. SectionBehavior set
# to Locked so this stays a held-frame test post 2026-05-23 amendment.
entity_add_integration_test(section_break_tail_no_freeze  section_break_tail_no_freeze.json)

# 2026-05-23 — ADR-0012 amendment regression guard for the Normal-mode
# break-aligned active-window extension. A Normal-mode clip ending exactly
# at a section break with source content past that point now extends its
# active window past clip.duration up to the source out point (Locked
# clips and non-break-aligned ends preserve original behavior). Companion
# to section_break_tail_no_freeze.json's Locked-held-frame coverage —
# this test exercises the Normal-extension path end-to-end.
entity_add_integration_test(normal_mode_extends_past_break normal_mode_extends_past_break.json)

# 2026-05-23 follow-up — two-clip-meeting-at-fade-break cache thrash
# regression guard. Two Normal clips on adjacent tracks meeting at a
# section break with fadeSeconds > 0. Pre-fix: Fix 8's extension fired
# for clip 1 (Normal + end-aligns-with-break + source > duration),
# decoder kept running past the break alongside clip 2's decoder, two
# 4K-class working sets blew FrameCache budget, Fix 4 cache-miss
# recovery thrashed, gate stalled, playhead froze. Post-fix: extension
# is gated on fadeSeconds == 0 so the fade case ends per the
# tail-frames mechanism — clip 1 fades out and decoder stops, no
# overlap, no thrash. Asserts the playhead advances cleanly past the
# fade window post-GO. Companion to normal_mode_extends_past_break.json
# (fadeSeconds=0 → extension still fires).
entity_add_integration_test(section_break_two_clip_no_thrash section_break_two_clip_no_thrash.json)

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

# RemotePatch persistence round-trip (ADR-0028 phase 6, schema v27).
# Load half asserts survival via RenameRemotePatch (succeeds only when
# the patch component exists and its store slot was rebound).
entity_add_integration_test(remote_patch_persistence_save remote_patch_persistence_save.json)
entity_add_integration_test(remote_patch_persistence_load remote_patch_persistence_load.json)
set_tests_properties(integration_remote_patch_persistence_save PROPERTIES
    FIXTURES_SETUP remote_patch_persistence_data)
set_tests_properties(integration_remote_patch_persistence_load PROPERTIES
    FIXTURES_REQUIRED remote_patch_persistence_data)

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

# NEW-09 (issue #74): decode + audio steering must keep advancing through an
# editor stall via the registry-free tickFromSnapshot fallback, without an
# audio seek storm at stall entry. Requires tests/fixtures/audio/tone_1khz.mov.
entity_add_integration_test(decode_stall_fallback decode_stall_fallback.json)

# NEW-09 (issue #74): LoadProject during active playback — the pre-#74
# CRITICAL crash class (show-thread fallback iterating registry views while
# load cleared/destroyed them). Save half builds the project fixture.
# Requires tests/fixtures/audio/tone_1khz.mov.
entity_add_integration_test(load_during_playback_save load_during_playback_save.json)
entity_add_integration_test(load_during_playback load_during_playback.json)
set_tests_properties(integration_load_during_playback_save PROPERTIES
    FIXTURES_SETUP load_during_playback_data)
set_tests_properties(integration_load_during_playback PROPERTIES
    FIXTURES_REQUIRED load_during_playback_data)

# Regression gate: AddScreen must not double-allocate compose targets.
# If the bug recurs the GPU wedges, DeviceLost is posted, the show thread
# stalls, and the frame-count assertion fires before Exit.
entity_add_integration_test(screen_no_double_allocation screen_no_double_allocation.json)

# Phase 1 (stress_16screen follow-up 2026-05-23) — video texture slot leak fix.
# Before the fix, deleting a Clip left its VideoTexture slot allocated until
# process exit. The fix adds an on_destroy<VideoTexture> observer that defers
# the free to the show thread's next beginShowFrame (after its fence wait).
# Asserts: import allocates 1 slot, delete + WaitSeconds reduces it back to 0.
entity_add_integration_test(video_texture_slot_leak video_texture_slot_leak.json)

# #90 — cross-tick stale-cached-snapshot guard. A show tick built from a
# SceneSnapshot cached across ticks must not upload/draw a freed-and-reused
# video-texture slot: the per-slot generation guard rejects the stale upload.
# Plays clip A, deletes it (schedules the slot free), stalls the editor so the
# show thread frees + bumps the slot generation while A's cached frames still
# upload (rejected), then imports B into the reused index. Asserts the guard
# fired via AssertVideoTextureStaleGenerationSkips.
entity_add_integration_test(video_texture_slot_reuse_stale_snapshot video_texture_slot_reuse_stale_snapshot.json)

# Phase 1 mesh-upload pacing: cold-start gate + one-per-tick steady-state cap.
# No pixel comparison needed; asserts upload counts directly.
entity_add_integration_test(mesh_upload_paces mesh_upload_paces.json)

# Phase 3.3 — Object Animation Layer end-to-end: CreateObjectAnimationLayer +
# AddKeyframe + Play/WaitSeconds + AssertObjectAnimationOutput.
# No pixel comparison; asserts ObjectAnimationOutput component state directly.
add_test(
    NAME integration_object_animation_layer
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/object_animation_layer.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_object_animation_layer PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# Phase 3.6 — OA layer + AnimatedProperties persistence round-trip (v15 schema).
# Save creates an OA layer targeting a Screen with a PositionX keyframe; load
# asserts the keyframe drove the correct output. No pixel comparison.
add_test(
    NAME integration_oa_layer_persistence_save
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/oa_layer_persistence_save.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_oa_layer_persistence_save PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
add_test(
    NAME integration_oa_layer_persistence_load
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/oa_layer_persistence_load.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_oa_layer_persistence_load PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
set_tests_properties(integration_oa_layer_persistence_save PROPERTIES
    FIXTURES_SETUP oa_layer_persistence_data)
set_tests_properties(integration_oa_layer_persistence_load PROPERTIES
    FIXTURES_REQUIRED oa_layer_persistence_data)

# Phase 3.7 — OA stall-fallback: baked tracks in SceneSnapshot.objectAnimationLayers
# are re-evaluated by buildRenderFrame per render frame so OA animation survives
# editor stalls. Three seek points assert the interpolated PositionX value at
# frames 0, 75, and 150 of a two-keyframe track (3.0 → 9.0 linear, midpoint 6.0).
add_test(
    NAME integration_oa_stall_fallback
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/oa_stall_fallback.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_oa_stall_fallback PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# Phase 3.8 — OA section freeze: Locked OA layer freezes at section break.
# Plays to a break, asserts positionX is held, waits, asserts still held, GO,
# then asserts positionX has advanced past the frozen value after resume.
add_test(
    NAME integration_oa_section_freeze
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/oa_section_freeze.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_oa_section_freeze PROPERTIES
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
    LABELS "integration"
    TIMEOUT 60
)

entity_add_integration_test(text_layer_create text_layer_create.json)

# Text layer persistence round-trip (v21 schema).
# Save creates a Text layer with all authoring fields set; load asserts every
# field survived the serialization round-trip.
add_test(
    NAME integration_text_layer_persistence_save
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/text_layer_persistence_save.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_text_layer_persistence_save PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
add_test(
    NAME integration_text_layer_persistence_load
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/text_layer_persistence_load.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_text_layer_persistence_load PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
set_tests_properties(integration_text_layer_persistence_save PROPERTIES
    FIXTURES_SETUP text_layer_persistence_data)
set_tests_properties(integration_text_layer_persistence_load PROPERTIES
    FIXTURES_REQUIRED text_layer_persistence_data)

# Generative layer ops: delete+undo and copy+paste for both Muncher and Text
# sub-kinds. Exercises the Phase 6 generalized delete/restore/clipboard paths.
# No pixel comparison; asserts track layer counts and TextLayerState fields.
add_test(
    NAME integration_generative_layer_ops
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/generative_layer_ops.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_generative_layer_ops PROPERTIES
    LABELS "integration"
    TIMEOUT 60
)

# Generative-layer keyframe persistence round-trip. Save creates a Text layer,
# adds Opacity + PositionX keyframes, moves one via MoveKeyframe; load asserts
# the keyframe tracks survived serialization, then exercises RemoveKeyframe.
add_test(
    NAME integration_generative_keyframe_persistence_save
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/generative_keyframe_persistence_save.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_generative_keyframe_persistence_save PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
add_test(
    NAME integration_generative_keyframe_persistence_load
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/generative_keyframe_persistence_load.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_generative_keyframe_persistence_load PROPERTIES
    LABELS "integration"
    TIMEOUT 30
    ENVIRONMENT "ENTITY_FORCE_WARP=1"
)
set_tests_properties(integration_generative_keyframe_persistence_save PROPERTIES
    FIXTURES_SETUP generative_keyframe_persistence_data)
set_tests_properties(integration_generative_keyframe_persistence_load PROPERTIES
    FIXTURES_REQUIRED generative_keyframe_persistence_data)

# Phase F audio tests — require tests/fixtures/audio/tone_1khz.mov.
# Generate the fixture once with: .\scripts\perf\setup-audio-fixtures.ps1
# These tests use LoopbackDevice (headless mode) so no ENTITY_FORCE_WARP needed.
add_test(
    NAME integration_audio_tone_playback
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/audio_tone_playback.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_audio_tone_playback PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

add_test(
    NAME integration_audio_loop_section_break
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/audio_loop_section_break.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_audio_loop_section_break PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

add_test(
    NAME integration_audio_pingpong
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/audio_pingpong.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_audio_pingpong PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

add_test(
    NAME integration_audio_loop_break_synced
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/audio_loop_break_synced.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_audio_loop_break_synced PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# ADR-0026 seek-sync preroll gate: mid-clip seek + Play checks that the gate
# holds then releases and A/V are both steered near the seek frame.
# Requires tests/fixtures/audio/tone_1khz.mov.
add_test(
    NAME integration_seek_sync_mid_clip
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/seek_sync_mid_clip.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_seek_sync_mid_clip PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# ADR-0026 seek-sync frame-0 control: Play from frame 0 must not hang; gate
# releases within ~one editor tick (decoders already warm at keyframe 0).
# Requires tests/fixtures/audio/tone_1khz.mov.
add_test(
    NAME integration_seek_sync_frame_zero
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/seek_sync_frame_zero.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_seek_sync_frame_zero PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# Paused-seek audio preroll: seeking while paused must steer/preroll the audio
# worker at the parked frame (decoupled from transport state), so a later Play
# does not stall on a cold audio seek + preroll under the seek-sync gate.
# Requires tests/fixtures/audio/tone_1khz.mov.
add_test(
    NAME integration_audio_paused_seek_preroll
    COMMAND $<TARGET_FILE:EntityMediaEditor>
            --headless --script ${CMAKE_SOURCE_DIR}/scripts/integration/audio_paused_seek_preroll.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(integration_audio_paused_seek_preroll PROPERTIES
    LABELS "integration"
    TIMEOUT 30
)

# ADR-0028 loopback OSC remote-layer test: patches a Muncher layer, drives
# opacity via UDP while the editor runs headless, and asserts the
# RemoteControlStore overlay applies the correct opacity at two points
# (disengaged: stored-not-applied, engaged: stored value applied on engage).
# Driven by a Python script that sends UDP OSC packets during the WaitSeconds
# windows in the JSON fixture. ENTITY_FORCE_WARP=1 so the D3D12 device is
# available (WARP software rasterizer) in CI without a GPU.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(
        NAME integration_remote_layer_osc
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/integration/remote_layer_osc_test.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(integration_remote_layer_osc PROPERTIES
        LABELS "integration"
        TIMEOUT 60
        ENVIRONMENT "ENTITY_FORCE_WARP=1"
    )
endif()

# ============================================================================
# Blanket RESOURCE_LOCK + ENTITY_FORCE_WARP for every integration test.
#
# Some integration tests are registered via raw add_test() rather than
# entity_add_integration_test(), so apply both by name here, after all
# registrations. See the comment in entity_add_integration_test() for why the
# lock exists: each test spawns a full EntityMediaEditor sharing one WARP
# device and CMAKE_SOURCE_DIR as working directory, and parallel runs fail
# non-deterministically without serialization.
#
# ENTITY_FORCE_WARP is blanket (issue #69), not just for golden-hash tests:
# tests left on the default adapter created ~hundreds of hardware
# device+swapchain churn cycles per suite run on the real GPU. One driver TDR
# under load (nvlddmkm event 153) leaves the driver degraded for minutes, and
# every subsequent hardware-adapter test then hits DEVICE_HUNG at its first
# Present, hangs its teardown, and gets SIGKILLed — the cross-process timeout
# cascade. WARP is per-process software rasterization: it cannot TDR the
# display driver, cannot wedge the desktop, and cannot leak a wedge into the
# next test. Hardware-adapter behavior is covered by the interactive editor
# and the perf scenarios (scripts/perf/), not by ctest.
# APPEND keeps per-test ENVIRONMENT entries (the duplicate WARP=1 on
# golden-hash tests is harmless).
# ============================================================================
get_directory_property(_entity_all_tests TESTS)
foreach(_entity_test IN LISTS _entity_all_tests)
    if(_entity_test MATCHES "^integration_")
        set_tests_properties(${_entity_test} PROPERTIES RESOURCE_LOCK "d3d12_warp")
        # ENTITY_FENCE_TIMEOUT_MS: the 5000ms default is tuned for live-show
        # hardware, where a 5s fence stall means the GPU is in real trouble.
        # WARP starved by the parallel unit-test load (-j8) can legitimately
        # stall past it while perfectly healthy; the watchdog then reports a
        # spurious device-lost (RemovedReason 0x0) and kills the test — the
        # June "watchdog-vs-TDR race" (docs/perf/device-hung-2026-06), seen
        # again 2026-07-04 on integration_generative_layer_ops. 10s keeps a
        # genuine wedge detectable well inside each test's TIMEOUT.
        set_property(TEST ${_entity_test} APPEND PROPERTY
                     ENVIRONMENT "ENTITY_FORCE_WARP=1;ENTITY_FENCE_TIMEOUT_MS=10000")
    endif()
endforeach()
