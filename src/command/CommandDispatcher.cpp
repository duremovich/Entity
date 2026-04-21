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

size_t CommandDispatcher::processQueue(Engine& engine) {
    // Handle WaitFrames countdown
    if (m_waitFramesRemaining > 0) {
        m_waitFramesRemaining--;
        if (m_waitFramesRemaining > 0) {
            return 0;  // Still waiting
        }
        // Done waiting, continue processing
    }

    size_t executed = 0;

    while (true) {
        CommandPtr command;

        // Extract command from queue (lock scope)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_commandQueue.empty()) {
                break;
            }
            command = std::move(m_commandQueue.front());
            m_commandQueue.pop();
        }

        // Execute command
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

        // Record if enabled
        if (m_recording) {
            m_recordedCommands.push_back(command->toJson());
        }

        executed++;
        m_scriptCommandsExecuted++;

        // If this was a WaitFrames command, stop processing until wait completes
        if (m_waitFramesRemaining > 0) {
            break;
        }
    }

    // Check if script finished
    if (m_scriptRunning && m_commandQueue.empty() && m_waitFramesRemaining == 0) {
        finishScript();
    }

    return executed;
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

    // Media commands
    registerFactory("ImportVideo", ImportVideoCommand::fromJson);

    // Project commands
    registerFactory("SaveProject", SaveProjectCommand::fromJson);
    registerFactory("LoadProject", LoadProjectCommand::fromJson);

    // Script control commands
    registerFactory("WaitFrames", WaitFramesCommand::fromJson);
    registerFactory("CaptureScreenshot", CaptureScreenshotCommand::fromJson);
    registerFactory("CaptureHash", CaptureHashCommand::fromJson);

    // Application commands
    registerFactory("Exit", ExitCommand::fromJson);

    // Property commands (for debugging and scripting)
    registerFactory("SetClipBlendMode", SetClipBlendModeCommand::fromJson);
    registerFactory("SetClipOpacity", SetClipOpacityCommand::fromJson);
    registerFactory("LogClipState", LogClipStateCommand::fromJson);
    registerFactory("SetClipRotation", SetClipRotationCommand::fromJson);
    registerFactory("SetClipPlaybackMode", SetClipPlaybackModeCommand::fromJson);
    registerFactory("SetClipFramerate", SetClipFramerateCommand::fromJson);
    registerFactory("SetClipDuration", SetClipDurationCommand::fromJson);

    // Keyframe animation commands
    registerFactory("AddKeyframe", AddKeyframeCommand::fromJson);
    registerFactory("ClearKeyframes", ClearKeyframesCommand::fromJson);

    // Screen commands
    registerFactory("AddScreen", AddScreenCommand::fromJson);
    registerFactory("SetClipTargetScreen", SetClipTargetScreenCommand::fromJson);
    registerFactory("AssertScreenExists", AssertScreenExistsCommand::fromJson);
    registerFactory("AssertScreenCount", AssertScreenCountCommand::fromJson);

    // UI commands
    registerFactory("SelectTab", SelectTabCommand::fromJson);
    registerFactory("SelectClip", SelectClipCommand::fromJson);
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
    m_scriptResults["commandsExecuted"] = m_scriptCommandsExecuted;

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
