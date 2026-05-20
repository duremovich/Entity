# osc-sender -- outbound OSC over UDP. Broadcasts transport state and fires
# user-defined mappings to external OSC destinations (show-control software, etc.).
# See manifest.json for the broadcast address namespace.
#
# License: Apache 2.0 (inherited from repo root LICENSE-PLUGIN-API).

add_library(entity-plugin-osc-sender STATIC
    OscSenderPlugin.cpp
)

target_link_libraries(entity-plugin-osc-sender
    PRIVATE
        entity-plugin-api
        # entity-bus       # not used: we route via IPluginContext accessors
)

target_compile_features(entity-plugin-osc-sender PRIVATE cxx_std_20)

if(WIN32)
    # Winsock for the UDP sender thread.
    target_link_libraries(entity-plugin-osc-sender PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(entity-plugin-osc-sender PRIVATE /W4 /WX-)
else()
    target_compile_options(entity-plugin-osc-sender PRIVATE -Wall -Wextra -Wpedantic)
endif()

entity_register_plugin(
    entity-plugin-osc-sender
    entity_plugin_register_osc_sender
)
