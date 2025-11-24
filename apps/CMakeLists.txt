# Applications

# Editor application (main application)
add_executable(EntityMediaEditor
    editor/main.cpp
    # editor/EditorUI.cpp     # To be implemented
    # editor/EventHandler.cpp # To be implemented
)

target_link_libraries(EntityMediaEditor
    PRIVATE
        EntityMediaCore
        glfw
        imgui::imgui
)

# Set as startup project in Visual Studio
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT EntityMediaEditor)

# Player application (future - standalone player without editor UI)
# add_executable(EntityMediaPlayer
#     player/main.cpp
# )
#
# target_link_libraries(EntityMediaPlayer
#     PRIVATE
#         EntityMediaCore
#         glfw
# )
