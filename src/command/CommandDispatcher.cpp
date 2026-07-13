#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/core/Engine.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace entity {

CommandDispatcher::CommandDispatcher() {
    registerBuiltinFactories();

    // Initialize script results
    m_scriptResults = {
        {"success", true},
        {"commandsExecuted", 0},
        {"errors", nlohmann::json::array()},
        {"screenshots", nlohmann::json::array()}
    };
}

CommandDispatcher::~CommandDispatcher() = default;

void CommandDispatcher::enqueue(CommandPtr command) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_commandQueue.push(std::move(command));
}

bool CommandDispatcher::enqueue(const std::string& typeName, const nlohmann::json& params) {
    nlohmann::json fullJson = params;
    fullJson["type"] = typeName;

    auto command = createFromJson(fullJson);
    if (!command) {
        std::cerr << "Unknown command type: " << typeName << std::endl;
        return false;
    }

    enqueue(std::move(command));
    return true;
}

size_t CommandDispatcher::processQueue(Engine& engine, Affinity affinity) {
    // Wait states only block the Editor-thread drain. The show thread's
    // Show-affinity drain skips them so transport commands stay responsive
    // during editor modal loops or script waits.
    if (affinity != Affinity::Show) {
        if (m_waitFramesRemaining > 0) {
            m_waitFramesRemaining--;
            if (m_waitFramesRemaining > 0) {
                return 0;
            }
        }
        if (m_waitUntilActive) {
            if (std::chrono::steady_clock::now() < m_waitUntil) {
                return 0;
            }
            m_waitUntilActive = false;
        }
    }

    size_t executed = 0;

    while (true) {
        CommandPtr command;

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_commandQueue.empty()) {
                break;
            }
            // Peek the front. Only pop if affinity matches — otherwise the
            // other thread will handle it. Previously we popped into a
            // `skipped` vector and re-fronted at end of loop, but that
            // raced with the other thread's concurrent pops: editor could
            // grab a later command (e.g. WaitSeconds) while show was busy
            // draining earlier Editor-affinity commands into `skipped`,
            // then show's requeue would put the earlier ones back ahead of
            // the WaitSeconds — visible reordering at execution. Test
            // failure path: oa_section_freeze with WaitSeconds 2.0 hoisted
            // ahead of Play, so AssertPlaybackState ran without giving
            // Timeline+SectionScheduler time to park.
            const Affinity cmdAffinity = m_commandQueue.front()->getAffinity();
            if (cmdAffinity != affinity && cmdAffinity != Affinity::Either
                    && affinity != Affinity::Either) {
                break;
            }
            command = std::move(m_commandQueue.front());
            m_commandQueue.pop();
        }

        const char* typeName = command->getTypeName();
        std::string description = command->getDescription();

        std::cout << "[Command] Executing: " << description << std::endl;

        bool success = false;
        try {
            success = command->execute(engine);
        } catch (const std::exception& e) {
            std::cerr << "[Command] Exception in " << typeName << ": " << e.what() << std::endl;
            addErrorToResults(std::string("Exception in ") + typeName + ": " + e.what());
        }

        if (!success) {
            std::cerr << "[Command] Failed: " << typeName << std::endl;
            addErrorToResults(std::string("Command failed: ") + description);
        }

        if (m_recording) {
            m_recordedCommands.push_back(command->toJson());
        }

        if (success && dynamic_cast<UndoableCommand*>(command.get())) {
            auto* raw = static_cast<UndoableCommand*>(command.release());
            m_undoStack.emplace_back(raw);
            if (m_undoStack.size() > MAX_UNDO_DEPTH) {
                m_undoStack.pop_front();
            }
            m_redoStack.clear();
        }

        executed++;
        m_scriptCommandsExecuted++;

        if (affinity != Affinity::Show && (m_waitFramesRemaining > 0 || m_waitUntilActive)) {
            break;
        }
    }

    return executed;
}

bool CommandDispatcher::scriptReadyToFinish() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_scriptRunning && m_commandQueue.empty()
        && m_waitFramesRemaining == 0 && !m_waitUntilActive;
}

void CommandDispatcher::registerFactory(const std::string& typeName, CommandFactory factory) {
    m_factories[typeName] = std::move(factory);
}

void CommandDispatcher::registerBuiltinFactories() {
    // Transport commands
    registerFactory("Play", PlayCommand::fromJson);
    registerFactory("Pause", PauseCommand::fromJson);
    registerFactory("TogglePlayPause", TogglePlayPauseCommand::fromJson);
    registerFactory("Seek", SeekCommand::fromJson);
    registerFactory("SeekToFrame", SeekToFrameCommand::fromJson);

    // Navigation commands
    registerFactory("SeekToStart", SeekToStartCommand::fromJson);
    registerFactory("SeekToEnd", SeekToEndCommand::fromJson);
    registerFactory("StepForward", StepForwardCommand::fromJson);
    registerFactory("StepBackward", StepBackwardCommand::fromJson);

    // Clip commands
    registerFactory("SelectClip", SelectClipCommand::fromJson);
    registerFactory("DeselectAll", DeselectAllCommand::fromJson);
    registerFactory("SplitClip", SplitClipCommand::fromJson);
    registerFactory("DuplicateClip", DuplicateClipCommand::fromJson);
    registerFactory("DeleteClip", DeleteClipCommand::fromJson);
    registerFactory("DeleteClips", DeleteClipsCommand::fromJson);
    registerFactory("CopyClip", CopyClipCommand::fromJson);
    registerFactory("CutClip", CutClipCommand::fromJson);
    registerFactory("PasteClip", PasteClipCommand::fromJson);
    registerFactory("SetClipMedia", SetClipMediaCommand::fromJson);

    // Media commands
    registerFactory("ImportVideo", ImportVideoCommand::fromJson);

    // Project commands
    registerFactory("SaveProject", SaveProjectCommand::fromJson);
    registerFactory("LoadProject", LoadProjectCommand::fromJson);

    // Script control commands
    registerFactory("WaitFrames", WaitFramesCommand::fromJson);
    registerFactory("WaitSeconds", WaitSecondsCommand::fromJson);
    registerFactory("CaptureScreenshot", CaptureScreenshotCommand::fromJson);
    registerFactory("CaptureHash", CaptureHashCommand::fromJson);

    // Application commands
    registerFactory("Exit", ExitCommand::fromJson);

    // Property commands (for debugging and scripting)
    registerFactory("SetClipBlendMode", SetClipBlendModeCommand::fromJson);
    registerFactory("SetClipOpacity", SetClipOpacityCommand::fromJson);

    // Remote-control patching commands (ADR-0028)
    registerFactory("PatchLayerRemote",    PatchLayerRemoteCommand::fromJson);
    registerFactory("UnpatchLayerRemote",  UnpatchLayerRemoteCommand::fromJson);
    registerFactory("RenameRemotePatch",   RenameRemotePatchCommand::fromJson);
    registerFactory("SetRemotePatchArmed", SetRemotePatchArmedCommand::fromJson);
    registerFactory("RenameTrack",         RenameTrackCommand::fromJson);
    registerFactory("LogClipState", LogClipStateCommand::fromJson);
    registerFactory("SetClipRotation", SetClipRotationCommand::fromJson);
    registerFactory("SetClipPlaybackMode", SetClipPlaybackModeCommand::fromJson);
    registerFactory("SetClipFramerate", SetClipFramerateCommand::fromJson);
    registerFactory("SetClipDuration", SetClipDurationCommand::fromJson);
    registerFactory("SetLayerStartFrame", SetLayerStartFrameCommand::fromJson);
    registerFactory("SetLayerDuration", SetLayerDurationCommand::fromJson);
    registerFactory("SetClipMediaStartFrame", SetClipMediaStartFrameCommand::fromJson);
    registerFactory("SetClipMediaOutFrame", SetClipMediaOutFrameCommand::fromJson);
    registerFactory("RippleInsertTime", RippleInsertTimeCommand::fromJson);
    registerFactory("RippleDeleteTime", RippleDeleteTimeCommand::fromJson);

    // Section commands (Phase B refactor — break-points + Locked playback)
    registerFactory("AddSectionBreak", AddSectionBreakCommand::fromJson);
    registerFactory("RemoveSectionBreak", RemoveSectionBreakCommand::fromJson);
    registerFactory("EditSectionBreak", EditSectionBreakCommand::fromJson);
    registerFactory("SectionGo", SectionGoCommand::fromJson);
    registerFactory("SetClipSectionBehavior", SetClipSectionBehaviorCommand::fromJson);
    registerFactory("AddSection", AddSectionCommand::fromJson);  // legacy alias (emits 2 breaks)
    registerFactory("AssertSectionCount", AssertSectionCountCommand::fromJson);
    registerFactory("AssertPlayheadAtFrame", AssertPlayheadAtFrameCommand::fromJson);
    registerFactory("AssertPlaybackState", AssertPlaybackStateCommand::fromJson);
    registerFactory("AssertKeyframeInterpolation", AssertKeyframeInterpolationCommand::fromJson);
    registerFactory("AssertClipMediaFrame", AssertClipMediaFrameCommand::fromJson);
    registerFactory("AssertClipPresentedFrame", AssertClipPresentedFrameCommand::fromJson);
    registerFactory("LogClipPlayback", LogClipPlaybackCommand::fromJson);
    registerFactory("AssertClipFadeMultiplier", AssertClipFadeMultiplierCommand::fromJson);
    registerFactory("AssertRemoteRenderOpacity", AssertRemoteRenderOpacityCommand::fromJson);

    // Cue tag commands (Phase A — numbered timeline markers)
    registerFactory("FireCue", FireCueCommand::fromJson);
    registerFactory("ArmCue", ArmCueCommand::fromJson);

    // DMX commands (#13)
    registerFactory("SetDmxOut", SetDmxOutCommand::fromJson);
    registerFactory("SetDmxMappingsJson", SetDmxMappingsJsonCommand::fromJson);
    registerFactory("AddCueAt", AddCueAtCommand::fromJson);
    registerFactory("RemoveCue", RemoveCueCommand::fromJson);
    registerFactory("EditCue", EditCueCommand::fromJson);
    registerFactory("AssertCueCount", AssertCueCountCommand::fromJson);
    registerFactory("AssertCueExists", AssertCueExistsCommand::fromJson);

    // Keyframe animation commands
    registerFactory("AddKeyframe", AddKeyframeCommand::fromJson);
    registerFactory("ClearKeyframes", ClearKeyframesCommand::fromJson);
    registerFactory("UpsertKeyframe", UpsertKeyframeCommand::fromJson);
    registerFactory("SetKeyframeInterpolation", SetKeyframeInterpolationCommand::fromJson);
    registerFactory("ClearPropertyKeyframes", ClearPropertyKeyframesCommand::fromJson);
    registerFactory("ClearEffectParamKeyframes", ClearEffectParamKeyframesCommand::fromJson);
    registerFactory("MoveKeyframe", MoveKeyframeCommand::fromJson);
    registerFactory("RemoveKeyframe", RemoveKeyframeCommand::fromJson);
    registerFactory("UpsertEffectKeyframe", UpsertEffectKeyframeCommand::fromJson);
    registerFactory("RemoveEffectKeyframe", RemoveEffectKeyframeCommand::fromJson);

    // Timeline structure commands
    registerFactory("AddTrack", AddTrackCommand::fromJson);

    // Screen commands
    registerFactory("AddScreen", AddScreenCommand::fromJson);
    registerFactory("SetClipTargetScreen", SetClipTargetScreenCommand::fromJson);
    registerFactory("SetContentRouting", SetContentRoutingCommand::fromJson);
    registerFactory("AssertScreenExists", AssertScreenExistsCommand::fromJson);
    registerFactory("AssertScreenCount", AssertScreenCountCommand::fromJson);
    registerFactory("AssertVideoTextureSlotsAllocated", AssertVideoTextureSlotsAllocatedCommand::fromJson);
    registerFactory("AssertVideoTextureStaleGenerationSkips", AssertVideoTextureStaleGenerationSkipsCommand::fromJson);
    registerFactory("AssertFrameCached", AssertFrameCachedCommand::fromJson);
    registerFactory("SetFrameCacheBudget", SetFrameCacheBudgetCommand::fromJson);
    registerFactory("AssertFrameCacheBudgetOK", AssertFrameCacheBudgetOKCommand::fromJson);

    // Output / display assignment commands (B2 — multi-display stress test)
    registerFactory("EnumerateDisplays",     EnumerateDisplaysCommand::fromJson);
    registerFactory("AssignScreenToDisplay", AssignScreenToDisplayCommand::fromJson);
    registerFactory("SetOutputEnabled",      SetOutputEnabledCommand::fromJson);

    // UI commands
    registerFactory("SelectTab", SelectTabCommand::fromJson);
    registerFactory("SelectClip", SelectClipCommand::fromJson);

    // Stage 3c: show-thread health gate
    registerFactory("SleepMs", SleepMsCommand::fromJson);
    registerFactory("StallEditor", StallEditorCommand::fromJson);
    registerFactory("AssertShowFrameCountAtLeast", AssertShowFrameCountAtLeastCommand::fromJson);

    // Mesh upload pacing gate
    registerFactory("AddSyntheticModel", AddSyntheticModelCommand::fromJson);
    registerFactory("AssertMeshUploadCount", AssertMeshUploadCountCommand::fromJson);

    // Per-layer effects (issue #54)
    registerFactory("AddEffect", AddEffectCommand::fromJson);
    registerFactory("RemoveEffect", RemoveEffectCommand::fromJson);
    registerFactory("SetScaleLock", SetScaleLockCommand::fromJson);
    registerFactory("SetEffectEnabled", SetEffectEnabledCommand::fromJson);
    registerFactory("SetEffectFloatParam", SetEffectFloatParamCommand::fromJson);

    // Object Animation Layer commands (Phase 3.3 + 3.4)
    registerFactory("CreateObjectAnimationLayer", CreateObjectAnimationLayerCommand::fromJson);
    registerFactory("AssertObjectAnimationOutput", AssertObjectAnimationOutputCommand::fromJson);
    registerFactory("AssertScreenSnapshot", AssertScreenSnapshotCommand::fromJson);

    // Generative layer commands
    registerFactory("CreateMuncherLayer", CreateMuncherLayerCommand::fromJson);
    registerFactory("CreateTextLayer", CreateTextLayerCommand::fromJson);

    // Generative layer property commands
    registerFactory("SetGenerativeRenderSize", SetGenerativeRenderSizeCommand::fromJson);

    // Text layer property commands
    registerFactory("SetTextContent",   SetTextContentCommand::fromJson);
    registerFactory("SetTextFont",      SetTextFontCommand::fromJson);
    registerFactory("SetTextFontSize",  SetTextFontSizeCommand::fromJson);
    registerFactory("SetTextColor",     SetTextColorCommand::fromJson);
    registerFactory("SetTextAlignment", SetTextAlignmentCommand::fromJson);
    registerFactory("SetTextBold",      SetTextBoldCommand::fromJson);
    registerFactory("SetTextItalic",    SetTextItalicCommand::fromJson);

    // Audio commands
    registerFactory("SetAudioSourceGain", SetAudioSourceGainCommand::fromJson);
    registerFactory("SetAudioSourceMute", SetAudioSourceMuteCommand::fromJson);
    registerFactory("SetAudioSourceSolo", SetAudioSourceSoloCommand::fromJson);
    registerFactory("SetMasterGain",      SetMasterGainCommand::fromJson);
    registerFactory("SetMasterMute",      SetMasterMuteCommand::fromJson);

    // Audio capture commands (integration tests)
    registerFactory("ClearAudioCapture",           ClearAudioCaptureCommand::fromJson);
    registerFactory("AssertAudioCaptureRms",       AssertAudioCaptureRmsCommand::fromJson);
    registerFactory("AssertAudioCaptureGoertzel",  AssertAudioCaptureGoertzelCommand::fromJson);
    registerFactory("AssertAudioWorkerSeekFrame",  AssertAudioWorkerSeekFrameCommand::fromJson);
    registerFactory("AssertAudioWorkerSeekCountAtMost", AssertAudioWorkerSeekCountAtMostCommand::fromJson);

    // Input bus commands
    registerFactory("SetInputChannel", SetInputChannelCommand::fromJson);

    // Script utility commands
    registerFactory("Undo", UndoCommand::fromJson);
    registerFactory("AssertTrackLayerCount", AssertTrackLayerCountCommand::fromJson);
    registerFactory("AssertKeyframeCount", AssertKeyframeCountCommand::fromJson);
    registerFactory("AssertTextLayerState", AssertTextLayerStateCommand::fromJson);
    registerFactory("SetTextLayerProperties", SetTextLayerPropertiesCommand::fromJson);

    // Signal Output Layer commands (Phase 3 + Phase 8).
    registerFactory("CreateSignalLayer",      CreateSignalLayerCommand::fromJson);
    registerFactory("SetSignalMode",          SetSignalModeCommand::fromJson);
    registerFactory("SetSignalAddress",       SetSignalAddressCommand::fromJson);
    registerFactory("SetSignalArgs",          SetSignalArgsCommand::fromJson);
    registerFactory("SetSignalValueSource",   SetSignalValueSourceCommand::fromJson);
    registerFactory("SetSignalMapping",       SetSignalMappingCommand::fromJson);

    // Signal output diagnostic (Phase 2 transport prover).
    // Posts a hard-coded SignalEmit through Engine::postSignalEmit so the
    // osc-sender loopback can be exercised from a headless script.
    registerFactory("TestSignalEmit", TestSignalEmitCommand::fromJson);
}

CommandPtr CommandDispatcher::createFromJson(const nlohmann::json& json) {
    if (!json.contains("type") || !json["type"].is_string()) {
        std::cerr << "Command JSON missing 'type' field" << std::endl;
        return nullptr;
    }

    std::string typeName = json["type"].get<std::string>();
    auto it = m_factories.find(typeName);
    if (it == m_factories.end()) {
        std::cerr << "Unknown command type: " << typeName << std::endl;
        return nullptr;
    }

    return it->second(json);
}

bool CommandDispatcher::loadScript(const std::string& filepath) {
    std::cout << "[Script] Loading: " << filepath << std::endl;

    // Read file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[Script] Failed to open: " << filepath << std::endl;
        return false;
    }

    nlohmann::json scriptJson;
    try {
        file >> scriptJson;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[Script] JSON parse error: " << e.what() << std::endl;
        return false;
    }

    // Check version
    int version = scriptJson.value("version", 1);
    if (version > 1) {
        std::cerr << "[Script] Unsupported script version: " << version << std::endl;
        return false;
    }

    // Get commands array
    if (!scriptJson.contains("commands") || !scriptJson["commands"].is_array()) {
        std::cerr << "[Script] Script missing 'commands' array" << std::endl;
        return false;
    }

    const auto& commands = scriptJson["commands"];

    // Reset script tracking
    m_scriptRunning = true;
    m_scriptCommandsTotal = commands.size();
    m_scriptCommandsExecuted = 0;
    m_scriptResults = {
        {"success", true},
        {"commandsExecuted", 0},
        {"errors", nlohmann::json::array()},
        {"screenshots", nlohmann::json::array()},
        {"scriptPath", filepath}
    };

    // Parse and enqueue commands
    size_t loaded = 0;
    for (const auto& cmdJson : commands) {
        auto command = createFromJson(cmdJson);
        if (command) {
            enqueue(std::move(command));
            loaded++;
        } else {
            std::cerr << "[Script] Failed to parse command: " << cmdJson.dump() << std::endl;
            addErrorToResults("Failed to parse command: " + cmdJson.dump());
        }
    }

    std::cout << "[Script] Loaded " << loaded << "/" << commands.size() << " commands" << std::endl;
    return loaded > 0;
}

nlohmann::json CommandDispatcher::getRecordedCommands() const {
    nlohmann::json result = {
        {"version", 1},
        {"commands", nlohmann::json::array()}
    };

    for (const auto& cmd : m_recordedCommands) {
        result["commands"].push_back(cmd);
    }

    return result;
}

void CommandDispatcher::clearRecording() {
    m_recordedCommands.clear();
}

size_t CommandDispatcher::getPendingCount() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_commandQueue.size();
}

void CommandDispatcher::finishScript() {
    m_scriptRunning = false;
    m_scriptResults["commandsExecuted"] = m_scriptCommandsExecuted.load();

    // Check if there were any errors
    if (!m_scriptResults["errors"].empty()) {
        m_scriptResults["success"] = false;
    }

    std::cout << "[Script] Finished. Executed " << m_scriptCommandsExecuted
              << "/" << m_scriptCommandsTotal << " commands" << std::endl;

    writeScriptResults();
}

void CommandDispatcher::addScreenshotToResults(const std::string& path) {
    m_scriptResults["screenshots"].push_back(path);
}

void CommandDispatcher::addErrorToResults(const std::string& error) {
    m_scriptResults["errors"].push_back(error);
    m_scriptResults["success"] = false;
}

bool CommandDispatcher::undo(Engine& engine) {
    if (m_undoStack.empty()) {
        std::cout << "[Undo] Nothing to undo" << std::endl;
        return false;
    }

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    std::cout << "[Undo] " << cmd->getDescription() << std::endl;
    bool ok = false;
    try {
        ok = cmd->undo(engine);
    } catch (const std::exception& e) {
        std::cerr << "[Undo] Exception: " << e.what() << std::endl;
    }

    if (ok) {
        m_redoStack.push_back(std::move(cmd));
    }
    // On failure drop the command — pushing a failed undo back onto the
    // undo stack invites an infinite Ctrl+Z loop with no progress.
    return ok;
}

bool CommandDispatcher::redo(Engine& engine) {
    if (m_redoStack.empty()) {
        std::cout << "[Redo] Nothing to redo" << std::endl;
        return false;
    }

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    std::cout << "[Redo] " << cmd->getDescription() << std::endl;
    bool ok = false;
    try {
        ok = cmd->redo(engine);
    } catch (const std::exception& e) {
        std::cerr << "[Redo] Exception: " << e.what() << std::endl;
    }

    if (ok) {
        m_undoStack.push_back(std::move(cmd));
    }
    return ok;
}

void CommandDispatcher::clearHistory() {
    m_undoStack.clear();
    m_redoStack.clear();
}

void CommandDispatcher::writeScriptResults() {
    const std::string resultPath = "script_result.json";

    try {
        std::ofstream file(resultPath);
        if (file.is_open()) {
            file << m_scriptResults.dump(2) << std::endl;
            std::cout << "[Script] Results written to: " << resultPath << std::endl;
        } else {
            std::cerr << "[Script] Failed to write results to: " << resultPath << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Script] Error writing results: " << e.what() << std::endl;
    }
}

} // namespace entity
