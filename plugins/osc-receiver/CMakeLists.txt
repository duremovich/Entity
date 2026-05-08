# osc-receiver -- inbound OSC over UDP. Translates a small fixed namespace
# of OSC addresses into Director commands via IPluginContext::enqueueCommand.
# See manifest.json for the address list.
#
# License: Apache 2.0 (inherited from repo root LICENSE-PLUGIN-API).

add_library(entity-plugin-osc-receiver STATIC
    OscReceiverPlugin.cpp
)

target_link_libraries(entity-plugin-osc-receiver
    PRIVATE
        entity-plugin-api
        # entity-bus       # not used: we route via IPluginContext::enqueueCommand
)

target_compile_features(entity-plugin-osc-receiver PRIVATE cxx_std_20)

if(WIN32)
    # Winsock for the UDP listener thread.
    target_link_libraries(entity-plugin-osc-receiver PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(entity-plugin-osc-receiver PRIVATE /W4 /WX-)
else()
    target_compile_options(entity-plugin-osc-receiver PRIVATE -Wall -Wextra -Wpedantic)
endif()

entity_register_plugin(
    entity-plugin-osc-receiver
    entity_plugin_register_osc_receiver
)
