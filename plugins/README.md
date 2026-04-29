# plugins/

First-party plugins that ship with the open-core repository. Each
subdirectory is one plugin; the build system globs them automatically.

## License

All plugins under this directory are **Apache 2.0** by default — the same
terms as `plugin-api/` and `entity-bus`, declared once at the repo root in
`LICENSE-PLUGIN-API`. There is no per-plugin `LICENSE` file unless a
plugin's `manifest.json` declares an alternate license (rare).

## Adding a plugin

1. Create `plugins/<name>/` with three files:
   - `CMakeLists.txt`
   - `manifest.json`
   - `<Something>Plugin.cpp` (your code)

2. In `CMakeLists.txt`:

   ```cmake
   add_library(entity-plugin-<name> STATIC <Something>Plugin.cpp)
   target_link_libraries(entity-plugin-<name>
       PRIVATE
           entity-plugin-api
           # entity-bus       # add only if your plugin sends/receives bus messages
   )
   target_compile_features(entity-plugin-<name> PRIVATE cxx_std_20)

   entity_register_plugin(
       entity-plugin-<name>
       entity_plugin_register_<name_with_underscores>
   )
   ```

3. In `<Something>Plugin.cpp`:

   ```cpp
   #include "entity/plugin/PluginContext.hpp"

   extern "C" int entity_plugin_register_<name_with_underscores>(
           entity::plugin::IPluginContext* ctx) {
       if (ctx == nullptr) return -1;
       if (ctx->apiVersion() != entity::plugin::PLUGIN_API_VERSION) return -2;
       ctx->log(entity::plugin::LogLevel::Info, "<name> registered");
       return 0;
   }
   ```

4. In `manifest.json`:

   ```json
   {
     "name":               "<name>",
     "version":            "0.1.0",
     "license":            "Apache-2.0",
     "requiredApiVersion": 0,
     "description":        "What this plugin does.",
     "kind":               "control-plane"
   }
   ```

   `kind` is `"control-plane"` for plugins that talk only to `entity-bus`,
   or `"hot-path"` for plugins that implement `OutputDriver` /
   `CodecProvider`.

5. Re-run cmake configure (`cmake -B build`) and rebuild. The plugin
   discovery glob picks up your new folder automatically; no edits to any
   other CMakeLists.txt are required.

## Reference plugin

`bus-logger/` is the canonical "hello, world" plugin. It does nothing
useful; copy from it when starting a new plugin.

## Private plugins (Entity Pro)

Proprietary plugins live in a separate `Entity-Pro` repository, not here.
Build them in by passing `-DEXTRA_PLUGIN_DIRS=<path>/Entity-Pro/plugins` to
CMake; the discovery loop picks them up the same way it picks up first-
party plugins. CI never references the private repo, so the public build
stays clean.
