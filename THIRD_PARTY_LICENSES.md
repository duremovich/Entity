# Third-Party Licenses

Entity Media Server links several third-party libraries, all resolved at
build time through the vcpkg manifest (`vcpkg.json`). None of them are
vendored into this repository — vcpkg fetches each one, along with its
own license text, into `vcpkg_installed/` during the build.

Every dependency below carries a license compatible with the Entity core
engine's GPLv3 + linking exception (see `LICENSE`). Permissive licenses
(MIT, BSD, Zlib) and the weak-copyleft licenses here (MPL-2.0,
LGPL-2.1-or-later) all combine cleanly with GPLv3.

## Dependencies

| Component | License | Role |
|---|---|---|
| [EnTT](https://github.com/skypjack/entt) | MIT | Entity Component System |
| [GLFW](https://www.glfw.org/) | Zlib | Windowing and input |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | Editor UI (immediate-mode GUI) |
| [imgui-node-editor](https://github.com/thedmd/imgui-node-editor) | MIT | Node-graph editor widget |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON parsing (projects, settings, scripts) |
| [GLM](https://github.com/g-truc/glm) | MIT | Math (vectors, matrices) |
| [Eigen](https://eigen.tuxfamily.org/) | MPL-2.0 | Linear algebra (projector calibration solver) |
| [OpenColorIO](https://opencolorio.org/) | BSD-3-Clause | Color management pipeline |
| [Snappy](https://github.com/google/snappy) | BSD-3-Clause | HAP frame (de)compression |
| [stb](https://github.com/nothings/stb) | MIT OR Public Domain | Image loading (PNG sequences) |
| [Tracy](https://github.com/wolfpld/tracy) | BSD-3-Clause | Frame profiler (on-demand) |
| [GoogleTest](https://github.com/google/googletest) | BSD-3-Clause | Unit test framework (test builds only) |
| [FFmpeg](https://ffmpeg.org/) | LGPL-2.1-or-later | Media demuxing and decoding |

## FFmpeg

FFmpeg is used under the LGPL-2.1-or-later terms. Depending on how it is
configured at build time, an FFmpeg build can include components under
the GPL or codecs that are patent-encumbered in some jurisdictions. The
vcpkg manifest builds FFmpeg with the `snappy` feature (for the HAP
encoder) and otherwise default options; CI uses a prebuilt shared
FFmpeg. Anyone redistributing Entity binaries is responsible for
confirming the FFmpeg configuration they ship satisfies LGPL conveyance
requirements and any applicable codec licensing in their jurisdiction.

## Authoritative license text

The text above is a summary. The authoritative, per-version license
text for each library is installed by vcpkg under
`vcpkg_installed/<triplet>/share/<package>/copyright` after a build.
