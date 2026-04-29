# entity-plugin-api — public headers a plugin compiles against.
#
# INTERFACE library: pure header surface, no .cpp files. Plugins link this
# (and entity-bus, if they speak bus messages) and get the include path +
# C++20 requirement, nothing else. No transitive dependency on EntityMediaCore.
#
# License: Apache 2.0 (see LICENSE-PLUGIN-API at the repo root, which
# covers this directory plus `plugins/` and `entity-bus`). This is
# intentional: closed-source plugins must be able to include these headers
# without inheriting GPL terms.

add_library(entity-plugin-api INTERFACE)

target_include_directories(entity-plugin-api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(entity-plugin-api INTERFACE cxx_std_20)

# entity-plugin-api is a pure-header interface lib but PluginContext.hpp
# forward-declares entity::bus::IMessageTransport. Plugins that actually
# touch the bus link entity-bus directly from their own CMakeLists; we do
# not propagate that link here so a hot-path plugin that never touches the
# bus pays nothing for it.

install(DIRECTORY include/entity/plugin
    DESTINATION include/entity
    FILES_MATCHING PATTERN "*.hpp"
)
