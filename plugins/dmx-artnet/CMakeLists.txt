# dmx-artnet -- DMX-512 I/O over Art-Net (UDP 6454), sACN/E1.31 (UDP
# multicast 5568), and Enttec DMX-USB-Pro (FTDI serial, Windows).
#
# Inbound traffic routes through IPluginContext::enqueueCommand per
# ADR-0013. Outbound (Phase 3) subscribes to a new entity-bus message
# (DmxFrame) so engine-internal cells can drive DMX without static
# coupling -- this is the one documented exception to ADR-0013, see
# ADR-0023.
#
# License: Apache 2.0 (inherited from repo root LICENSE-PLUGIN-API).

add_library(entity-plugin-dmx-artnet STATIC
    DmxArtnetPlugin.cpp
    ArtnetParser.cpp
    SacnParser.cpp
    MulticastJoin.cpp
    UniverseTable.cpp
    OutboundUniverseTable.cpp
    OutboundSender.cpp
    DefaultMappings.cpp
    CommandEmitter.cpp
    MappingResolver.cpp
)

if(WIN32)
    target_sources(entity-plugin-dmx-artnet PRIVATE
        EnttecSerial.cpp
        EnttecFrameParser.cpp
    )
endif()

target_link_libraries(entity-plugin-dmx-artnet
    PRIVATE
        entity-plugin-api
        nlohmann_json::nlohmann_json   # Phase 5 mapping JSON; Enttec sidecar file
        # entity-bus not used: Phase 3 outbound routes via
        # entity::dmx::OutboundBridge (a function-pointer hook in the
        # core lib) per ADR-0023.
)

# The plugin uses entity::dmx::OutboundBridge::setHook from
# include/entity/dmx/OutboundBridge.hpp -- that lives in EntityMediaCore
# headers; add the include dir so the plugin can see it.
target_include_directories(entity-plugin-dmx-artnet
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(entity-plugin-dmx-artnet PRIVATE cxx_std_20)

if(WIN32)
    # Winsock for UDP listeners + outbound sender.
    # SetupAPI for FTDI VCP COM-port enumeration (Phase 4).
    target_link_libraries(entity-plugin-dmx-artnet PRIVATE ws2_32 setupapi)
endif()

if(MSVC)
    target_compile_options(entity-plugin-dmx-artnet PRIVATE /W4 /WX-)
else()
    target_compile_options(entity-plugin-dmx-artnet PRIVATE -Wall -Wextra -Wpedantic)
endif()

entity_register_plugin(
    entity-plugin-dmx-artnet
    entity_plugin_register_dmx_artnet
)
