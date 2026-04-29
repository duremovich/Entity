# bus-logger -- canonical "hello, world" plugin. Logs registration to
# stdout via the IPluginContext::log sink. Copy from this when starting a
# new plugin.
#
# License: Apache 2.0 (inherited from repo root LICENSE-PLUGIN-API).

add_library(entity-plugin-bus-logger STATIC
    BusLoggerPlugin.cpp
)

target_link_libraries(entity-plugin-bus-logger
    PRIVATE
        entity-plugin-api
        # entity-bus      # bus-logger doesn't actually subscribe to anything
                          # yet -- left here as the place you'd add it for a
                          # real bus-using plugin.
)

target_compile_features(entity-plugin-bus-logger PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(entity-plugin-bus-logger PRIVATE /W4 /WX-)
else()
    target_compile_options(entity-plugin-bus-logger PRIVATE -Wall -Wextra -Wpedantic)
endif()

entity_register_plugin(
    entity-plugin-bus-logger
    entity_plugin_register_bus_logger
)
