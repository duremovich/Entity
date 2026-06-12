vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

if ("docking-experimental" IN_LIST FEATURES)
    vcpkg_from_github(
       OUT_SOURCE_PATH SOURCE_PATH
       REPO ocornut/imgui
       REF dc3e531ff28450bff73fde0163b1d076b6bb5605
       SHA512 c716d90c57a036f0117eb02814c3ed164db31727f604fb7568e8413dfba73b678e09c6405a2af52e12522cb766fcf585a97a69b33097349b66755e5b9b04fd06
       HEAD_REF docking
       )
else()
    vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ocornut/imgui
    REF v${VERSION}
    SHA512 69023cbff02287cb6409b08079d21469680222ca985fd1acd658dcf704c615cb2d74668821dc4830bef70be6033640b0528d3a103b70ebeb4b7563576305bf80
    HEAD_REF master
    )
endif()

file(COPY "${CMAKE_CURRENT_LIST_DIR}/imgui-config.cmake.in" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

# Entity overlay patch (DEVICE_HUNG investigation, docs/perf/device-hung-2026-06).
# Name the per-frame vertex/index UPLOAD buffers so they show up as a named
# class in DRED RecentFreed= breadcrumbs. This is a falsification probe: a
# DRED capture that names ImGuiVtx/ImGuiIdx in RecentFreed= confirms the
# use-after-free is an ImGui frame buffer. The SetName calls are inserted
# immediately after each successful CreateCommittedResource in the DX12
# backend's grow-and-realloc path. No behavior change beyond debug naming.
if ("dx12-binding" IN_LIST FEATURES)
    vcpkg_replace_string("${SOURCE_PATH}/backends/imgui_impl_dx12.cpp"
        "        if (bd->pd3dDevice->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fr->VertexBuffer)) < 0)\n            return;\n"
        "        if (bd->pd3dDevice->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fr->VertexBuffer)) < 0)\n            return;\n        fr->VertexBuffer->SetName(L\"ImGuiVtx\");\n")
    vcpkg_replace_string("${SOURCE_PATH}/backends/imgui_impl_dx12.cpp"
        "        if (bd->pd3dDevice->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fr->IndexBuffer)) < 0)\n            return;\n"
        "        if (bd->pd3dDevice->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fr->IndexBuffer)) < 0)\n            return;\n        fr->IndexBuffer->SetName(L\"ImGuiIdx\");\n")

    # Fail loud if either anchor drifted and the patch silently dropped.
    # vcpkg_replace_string is a no-op (not an error) when its match string is
    # absent at the default backcompat level, so a future imgui pin bump could
    # leave the backend unpatched while the port build still succeeds. That
    # would make DRED show <unnamed> on the freed buffer -- a FALSE negative
    # for this falsification probe. Re-read the source and abort the port build
    # if the SetName calls are not present.
    file(READ "${SOURCE_PATH}/backends/imgui_impl_dx12.cpp" _entity_dx12_backend)
    if(NOT _entity_dx12_backend MATCHES "fr->VertexBuffer->SetName\\(L\"ImGuiVtx\"\\)")
        message(FATAL_ERROR
            "Entity overlay: ImGuiVtx SetName patch did not apply to "
            "imgui_impl_dx12.cpp -- the VertexBuffer CreateCommittedResource "
            "anchor drifted (likely an imgui version bump). The DRED "
            "falsification probe would silently report <unnamed>. Update the "
            "anchor in ports/imgui/portfile.cmake before proceeding.")
    endif()
    if(NOT _entity_dx12_backend MATCHES "fr->IndexBuffer->SetName\\(L\"ImGuiIdx\"\\)")
        message(FATAL_ERROR
            "Entity overlay: ImGuiIdx SetName patch did not apply to "
            "imgui_impl_dx12.cpp -- the IndexBuffer CreateCommittedResource "
            "anchor drifted (likely an imgui version bump). The DRED "
            "falsification probe would silently report <unnamed>. Update the "
            "anchor in ports/imgui/portfile.cmake before proceeding.")
    endif()
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
    allegro5-binding            IMGUI_BUILD_ALLEGRO5_BINDING
    dx9-binding                 IMGUI_BUILD_DX9_BINDING
    dx10-binding                IMGUI_BUILD_DX10_BINDING
    dx11-binding                IMGUI_BUILD_DX11_BINDING
    dx12-binding                IMGUI_BUILD_DX12_BINDING
    glfw-binding                IMGUI_BUILD_GLFW_BINDING
    glut-binding                IMGUI_BUILD_GLUT_BINDING
    metal-binding               IMGUI_BUILD_METAL_BINDING
    opengl2-binding             IMGUI_BUILD_OPENGL2_BINDING
    opengl3-binding             IMGUI_BUILD_OPENGL3_BINDING
    osx-binding                 IMGUI_BUILD_OSX_BINDING
    sdl2-binding                IMGUI_BUILD_SDL2_BINDING
    sdl2-renderer-binding       IMGUI_BUILD_SDL2_RENDERER_BINDING
    vulkan-binding              IMGUI_BUILD_VULKAN_BINDING
    win32-binding               IMGUI_BUILD_WIN32_BINDING
    freetype                    IMGUI_FREETYPE
    wchar32                     IMGUI_USE_WCHAR32
)

if ("libigl-imgui" IN_LIST FEATURES)
    vcpkg_download_distfile(
        IMGUI_FONTS_DROID_SANS_H
        URLS
            https://raw.githubusercontent.com/libigl/libigl-imgui/c3efb9b62780f55f9bba34561f79a3087e057fc0/imgui_fonts_droid_sans.h
        FILENAME "imgui_fonts_droid_sans.h"
        SHA512
            abe9250c9a5989e0a3f2285bbcc83696ff8e38c1f5657c358e6fe616ff792d3c6e5ff2fa23c2eeae7d7b307392e0dc798a95d14f6d10f8e9bfbd7768d36d8b31
    )

    file(INSTALL "${IMGUI_FONTS_DROID_SANS_H}" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
    OPTIONS_DEBUG
        -DIMGUI_SKIP_HEADERS=ON
)

vcpkg_cmake_install()

if ("freetype" IN_LIST FEATURES)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/imconfig.h" "//#define IMGUI_ENABLE_FREETYPE" "#define IMGUI_ENABLE_FREETYPE")
endif()
if ("wchar32" IN_LIST FEATURES)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/imconfig.h" "//#define IMGUI_USE_WCHAR32" "#define IMGUI_USE_WCHAR32")
endif()

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
