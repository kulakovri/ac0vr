cmake_minimum_required(VERSION 3.20)
project(ps2recomp_ac5 C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(PS2_GEN_OPT "-O1" CACHE STRING "optimisation level of the generated code")
set(GEN_OPT "${PS2_GEN_OPT}")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

option(PS2_DIAG "compile the entry histogram and the guest-store watch in" OFF)

find_package(Threads REQUIRED)

# The player's build (tools/setup/toolchains/*.cmake; night_setup 2026-11-04): a portable compiler with no MSYS2, no
# pkg-config and no Vulkan SDK. PS2_SDK_DIR holds the headers and import libraries of the libraries we ship
# (tools/setup/make_player_sdk.sh), PS2_PREBUILT_SPIRV the compiled shaders. All empty: the developer's build as before.
set(PS2_SDK_DIR "" CACHE PATH "headers and import libraries of SDL3, the OpenXR loader and vulkan-1 (the player's build)")
set(PS2_PREBUILT_SPIRV "" CACHE PATH "a folder of compiled .spv files: copied instead of running glslc (the player's build)")
option(PS2_BUILD_GSREPLAY "also build the gsreplay tool" ON)
if(PS2_SDK_DIR)
  set(SDL3_INCLUDE_DIRS "${PS2_SDK_DIR}/include")
  set(SDL3_LIBRARY_DIRS "${PS2_SDK_DIR}/lib")
  set(SDL3_LIBRARIES SDL3)
  set(Vulkan_INCLUDE_DIRS "${PS2_SDK_DIR}/include")
  add_library(Vulkan::Vulkan INTERFACE IMPORTED)
  set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES vulkan-1)
else()
  find_package(Vulkan REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED sdl3)
endif()

set(PS2_GENERATED_DIR "generated" CACHE STRING
    "recompiler output directory, relative to the source tree")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/${PS2_GENERATED_DIR}")

if(NOT EXISTS "${GEN_DIR}/ps2_func_table.c")
  message(FATAL_ERROR
    "No generated code in ${PS2_GENERATED_DIR}/. Make it from your own disc with the recompiler (README, Build, step 2) "
    "and configure with -DPS2_GENERATED_DIR=gen_zero.")
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/ps2_code_*.c")
list(APPEND GENERATED_SOURCES
     "${GEN_DIR}/ps2_func_table.c"
     "${GEN_DIR}/ps2_image.c"
     "${GEN_DIR}/ps2_symbols.c")

set(RUNTIME_SOURCES
    runtime/src/ps2_core.c
    runtime/src/ps2_crash.c
    runtime/src/ps2_hostprof.c
    runtime/src/ps2_hw.c
    runtime/src/ps2_gfxq.c
    runtime/src/ps2_gs.c
    runtime/src/ps2_texrep.c
    runtime/src/zero_fmv.c
    runtime/src/ps2_vu.c
    runtime/src/ps2_vu0.c
    runtime/src/ps2_ipu.c
    runtime/src/ps2_kernel.c
    runtime/src/ps2_spu2.c
    runtime/src/ps2_spatial.c
    runtime/src/ps2_nustream.c
    runtime/src/ps2_memcard.c
    runtime/src/ps2_quit.c
    runtime/src/ps2_audio.c
    runtime/src/ps2_recexact.c
    runtime/src/ps2_gforce_audio.c
    runtime/src/ps2_sif.c
    runtime/src/ps2_vfs.c
    runtime/src/ps2_hle_test.c
    runtime/src/ps2_hle_sif.c
    runtime/src/ps2_hle_cdvd.c
    runtime/src/ps2_hle_pad.c
    runtime/src/ps2_touch.c
    runtime/src/ps2_hotas.c
    runtime/src/ps2_hle_iop.c
    runtime/src/ps2_video.c
    runtime/src/ps2_clouds.c
    runtime/src/ps2_clouds_far.c
    runtime/src/ps2_clouds_near.c
    runtime/src/ps2_clouds_shadow.c
    runtime/src/ps2_clouds_optics.c
    runtime/src/ps2_clouds_wake.c
    runtime/src/ps2_vkcen.c
    runtime/src/ps2_clouds_art.c
    runtime/src/ps2_trails.c
    runtime/src/ps2_cracks.c
    runtime/src/ps2_headvol.c
    runtime/src/ps2_state.c
    runtime/src/ps2_capture.c
    runtime/src/ps2_statecap.c
    runtime/src/ps2_settings.c
    runtime/src/ps2_mod.c
    runtime/src/ps2_hook.c
    runtime/src/ps2_modapi.c
    runtime/src/ps2_patch.c
    runtime/src/ps2_params.c
    runtime/src/ps2_lua.c
    runtime/src/ps2_ui.cpp
    runtime/src/ps2_launcher.cpp
    runtime/src/rn/rn_census.c
    runtime/src/rn/rn_2d.c
    runtime/src/rn/rn_dump.c
    runtime/src/rn/rn_vp.c
    runtime/src/rn/rn_vp_model.c
    runtime/src/rn/rn_vp_terrain.c
    runtime/src/rn/rn_vp_shadow.c
    runtime/src/rn/rn_vp_sprite.c
    runtime/src/rn/rn_vp_load.c
    runtime/src/rn/rn_vp_lit.c
    runtime/src/rn/rn_vp_fx.c
    runtime/src/rn/rn_vp_flat.c
    runtime/src/rn/rn_vpfuzz.c
    runtime/src/rn/rn_intent.c
    runtime/src/rn/rn_fixes.c
    runtime/src/rn/rn_sun.c
    runtime/src/rn/rn_sky.c
    runtime/src/rn/rn_tri3d.c
    runtime/src/rn/rn_billboard.c
    runtime/src/rn/rn_worldtri.c
    runtime/src/rn/rn_screen.c
    runtime/src/rn/rn_tap.c
    runtime/src/ps2_main.c)

set(LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${LUA_DIR}/*.c")
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC "${LUA_DIR}")
set_source_files_properties(${LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(lua54 PRIVATE -O2)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
list(APPEND RUNTIME_SOURCES ${IMGUI_SOURCES})

add_executable(ac5 ${RUNTIME_SOURCES} ${GENERATED_SOURCES})
target_sources(ac5 PRIVATE runtime/src/zero_vr.c runtime/src/zero_menus.c runtime/src/zero_audio3d.c runtime/src/zero_syssnd.c runtime/src/zero_clouds.c runtime/src/zero_terrain.c runtime/src/zero_terrain_mat.c runtime/src/zero_props.c runtime/src/zero_water.c runtime/src/zero_texpack.c runtime/src/zero_trails.c runtime/src/zero_cracks.c runtime/src/zero_flyby.c runtime/src/ps2_xr.c)
target_link_libraries(ac5 PRIVATE openxr_loader)
target_include_directories(ac5 PRIVATE runtime/include "${GEN_DIR}"
                                       ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                       ${SDL3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
target_link_directories(ac5 PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(ac5 PRIVATE Threads::Threads m
                                  ${SDL3_LIBRARIES} Vulkan::Vulkan lua54)
target_link_options(ac5 PRIVATE -static-libstdc++ -static-libgcc)

option(PS2_HOOK_ENTRIES "patchable entry sleds in generated code, for mod hooks" ON)
# ps2_vu0_advance (runtime/include/ps2_vu0.h): ON inlines the VU0 flag pipeline's whole slot loop at every call site
# of the generated code (the build before 2026-10-24); OFF keeps only the pending test inline (ps2_vu0.c)
option(PS2_VU0_ADV_INLINE "inline the whole VU0 flag pipeline loop at every generated call site (old build)" OFF)
# -mcmodel=small on the generated code only: its extern data (ps2_pt, ps2_trace_ring, ps2_trace_pos, ps2_loop_ctr,
# ps2_zero_q) lives in this image, so RIP-relative access replaces the medium model's .refptr pointer loads
option(PS2_GEN_CMODEL_SMALL "compile the generated code with -mcmodel=small (no .refptr indirection)" OFF)
set(GEN_COMPILE_OPTIONS "${GEN_OPT}")
if(PS2_HOOK_ENTRIES)
  list(APPEND GEN_COMPILE_OPTIONS "-fpatchable-function-entry=8")
endif()
if(PS2_GEN_CMODEL_SMALL)
  list(APPEND GEN_COMPILE_OPTIONS "-mcmodel=small")
endif()
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
                            COMPILE_OPTIONS "${GEN_COMPILE_OPTIONS}")

set(HOT_SOURCES runtime/src/ps2_vu.c runtime/src/ps2_gs.c runtime/src/ps2_hw.c)
set_source_files_properties(${HOT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3")

if(PS2_DIAG)
  target_compile_definitions(ac5 PRIVATE PS2_DIAG=1)
endif()
if(PS2_VU0_ADV_INLINE)
  target_compile_definitions(ac5 PRIVATE PS2_VU0_ADV_INLINE=1)
endif()

target_compile_options(ac5 PRIVATE
    -fno-strict-aliasing
    -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

if(PS2_BUILD_GSREPLAY)
set(REPLAY_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM REPLAY_SOURCES runtime/src/ps2_main.c
                                runtime/src/ps2_lua.c
                                runtime/src/ps2_modapi.c
                                runtime/src/ps2_mod.c
                                runtime/src/ps2_hook.c
                                runtime/src/ps2_patch.c
                                runtime/src/ps2_params.c
                                runtime/src/rn/rn_intent.c
                                runtime/src/rn/rn_fixes.c
                                runtime/src/rn/rn_tap.c)
list(APPEND REPLAY_SOURCES runtime/src/ps2_replay.c)

add_executable(gsreplay ${REPLAY_SOURCES} runtime/src/ps2_xr.c)
target_link_libraries(gsreplay PRIVATE openxr_loader)
target_include_directories(gsreplay PRIVATE runtime/include "${GEN_DIR}"
                                            "${LUA_DIR}"
                                            ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                            ${SDL3_INCLUDE_DIRS}
                                            ${Vulkan_INCLUDE_DIRS})
target_link_directories(gsreplay PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(gsreplay PRIVATE Threads::Threads m
                                       ${SDL3_LIBRARIES} Vulkan::Vulkan)
target_link_options(gsreplay PRIVATE -static-libstdc++ -static-libgcc)
if(PS2_DIAG)
  target_compile_definitions(gsreplay PRIVATE PS2_DIAG=1)
endif()
if(PS2_VU0_ADV_INLINE)
  target_compile_definitions(gsreplay PRIVATE PS2_VU0_ADV_INLINE=1)
endif()

target_compile_options(gsreplay PRIVATE
    -fno-strict-aliasing -fwrapv
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

set(SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
if(PS2_PREBUILT_SPIRV)
add_custom_target(shaders ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS2_PREBUILT_SPIRV}" "${SHADER_OUT}"
  COMMENT "prebuilt SPIR-V from ${PS2_PREBUILT_SPIRV}")
else()
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
  message(FATAL_ERROR "glslc not found; it ships with the Vulkan SDK")
endif()

set(SPV_FILES)
# includes of the GS-path shaders (mesh.vert: the cloud shadow lookup for cloud shadows on the ground)
set(GS_GLSL_INC "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_water.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_detail.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/city_look.glsl"
               "${CMAKE_SOURCE_DIR}/runtime/shaders/terrain_zcensus.glsl")
foreach(SH gs.vert gs.frag gs_zcopy.frag colclip.frag present.vert present.frag
           rn_vis.frag rn_sun.vert rn_sun.frag mesh.vert mesh.frag cockpit.vert cockpit.frag
           cockpit_shadow.vert cockpit_comp.vert cockpit_comp.frag cockpit_screen.frag cockpit_cut.frag edge_aa.frag
           cloudveil.vert cloudveil.frag gforce.vert gforce.frag trails.vert trails.frag haze.frag eye_adapt.frag
           burst.vert burst.frag
           xr_depth.frag pmask.vert mirterr.vert mirterr.frag)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${SH}.spv"
    COMMAND "${GLSLC}" -O "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}"
            -o "${SHADER_OUT}/${SH}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" ${GS_GLSL_INC}
    COMMENT "glslc ${SH}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${SH}.spv")
endforeach()
# multiview (single-pass stereo) variants of the GS shaders: same sources compiled with -DMV
foreach(PAIR "gs.vert;gs_mv.vert" "gs.frag;gs_mv.frag" "gs_zcopy.frag;gs_zcopy_mv.frag"
             "mesh.vert;mesh_mv.vert" "mesh.frag;mesh_mv.frag"
             "cockpit.vert;cockpit_mv.vert" "cockpit.frag;cockpit_mv.frag" "cockpit_comp.frag;cockpit_comp_mv.frag"
             "cockpit_screen.frag;cockpit_screen_mv.frag" "cockpit_cut.frag;cockpit_cut_mv.frag"
             "edge_aa.frag;edge_aa_mv.frag" "cloudveil.vert;cloudveil_mv.vert"
             "trails.vert;trails_mv.vert" "haze.frag;haze_mv.frag" "xr_depth.frag;xr_depth_mv.frag"
             "burst.vert;burst_mv.vert" "burst.frag;burst_mv.frag"
             "pmask.vert;pmask_mv.vert" "smooth_warp.frag;smooth_warp_mv.frag")
  list(GET PAIR 0 SRC)
  list(GET PAIR 1 DST)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" -O -DMV "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc -DMV ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# shader water: the water variant of mesh.frag (terrain_water.glsl), used only by the terrain's water patches
# near-ground materials (terrain_detail.glsl): the terrain's first pass near the eye, also on water patches
foreach(TRIPLE "mesh.frag;mesh_wat.frag;-DWATER" "mesh.frag;mesh_wat_mv.frag;-DMV;-DWATER"
               "mesh.frag;mesh_det.frag;-DTDETAIL" "mesh.frag;mesh_det_mv.frag;-DMV;-DTDETAIL"
               "mesh.frag;mesh_watdet.frag;-DWATER;-DTDETAIL" "mesh.frag;mesh_watdet_mv.frag;-DMV;-DWATER;-DTDETAIL"
               # T5 terrain texture pack (zero_texpack.c): the terrain's first pass of the patches the pack serves
               "mesh.frag;mesh_tp.frag;-DTEXPACK" "mesh.frag;mesh_tp_mv.frag;-DMV;-DTEXPACK"
               "mesh.frag;mesh_wattp.frag;-DWATER;-DTEXPACK" "mesh.frag;mesh_wattp_mv.frag;-DMV;-DWATER;-DTEXPACK"
               "mesh.frag;mesh_dettp.frag;-DTDETAIL;-DTEXPACK" "mesh.frag;mesh_dettp_mv.frag;-DMV;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp.frag;-DWATER;-DTDETAIL;-DTEXPACK"
               "mesh.frag;mesh_watdettp_mv.frag;-DMV;-DWATER;-DTDETAIL;-DTEXPACK"
               # the city's detailed look (zero_props.c, city_look.glsl): only the city buildings' draws
               "mesh.frag;mesh_city.frag;-DCITY" "mesh.frag;mesh_city_mv.frag;-DMV;-DCITY"
               # perf5_terrain: the terrain overlay passes' zero-colour cull (PS2_PERF_TERRAIN_ZCULL) and the census of
               # their fragments (PS2_TERRAIN_ZCENSUS, diagnostic: terrain_zcensus.glsl)
               "mesh.vert;mesh_zc.vert;-DZCULL" "mesh.vert;mesh_zc_mv.vert;-DMV;-DZCULL"
               "mesh.frag;mesh_zcen.frag;-DZCENSUS" "mesh.frag;mesh_zcen_mv.frag;-DMV;-DZCENSUS"
               "gs.frag;gs_zcen.frag;-DZCENSUS" "gs.frag;gs_zcen_mv.frag;-DMV;-DZCENSUS"
               "gs.frag;gs_zcen_lr.frag;-DLOCAL_READ;-DZCENSUS" "gs.frag;gs_zcen_mv_lr.frag;-DMV;-DLOCAL_READ;-DZCENSUS")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}" ${GS_GLSL_INC}
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# DATE through dynamic rendering local read (input attachment instead of a target snapshot); the cockpit's vertex
# shader with the primitive shading rate of ACZ_VR_COCKPIT_VRS (perf4_cockpit)
foreach(TRIPLE "gs.frag;gs_lr.frag;-DLOCAL_READ" "gs.frag;gs_mv_lr.frag;-DMV;-DLOCAL_READ"
               "cockpit.vert;cockpit_vrs.vert;-DCPIT_VRS" "cockpit.vert;cockpit_vrs_mv.vert;-DMV;-DCPIT_VRS")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# the canopy glass's variant module (PS2_PERF_GLASS_VARIANTS, perf5_glassexact): cockpit.frag with -DGXV, specialised per
# glass state (the plain cockpit[_mv].frag.spv stays the same SPIR-V)
foreach(TRIPLE "cockpit.frag;cockpit_gx.frag;-DGXV" "cockpit.frag;cockpit_gx_mv.frag;-DMV;-DGXV")
  list(GET TRIPLE 0 SRC)
  list(GET TRIPLE 1 DST)
  list(SUBLIST TRIPLE 2 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
            -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SRC}"
    COMMENT "glslc ${DEFS} ${SRC}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# clouds (ps2_clouds.c, ACZ_VR_CLOUDS): compute passes and the composite; all include cloud_common.glsl
set(CLOUD_GLSL "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_common.glsl" "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.glsl"
    "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_optics.glsl")
# (cloud_optics.comp: the optics probe, ps2_clouds_optics.c; cloud_near_reuse.comp: ACZ_VR_CLOUD_NEAR_REUSE, ps2_clouds_near.c;
#  cloud_wake.comp: the cloud wake's probe, ps2_clouds_wake.c)
foreach(SH cloud_noise.comp cloud_weather.comp cloud_near.comp cloud_near_reuse.comp cloud_test.comp cloud_comp.frag cloud_optics.comp
        cloud_shafts.comp
        cloud_wake.comp)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${SH}.spv"
    COMMAND "${GLSLC}" -O "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" -o "${SHADER_OUT}/${SH}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/${SH}" ${CLOUD_GLSL}
    COMMENT "glslc ${SH}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${SH}.spv")
endforeach()
# S2's far renderer: one source, a variant per pass (cloud_far.glsl is S2's include)
foreach(PASS weather wsoft tiles args march)
  string(TOUPPER "${PASS}" PASS_UP)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/cloud_far_${PASS}.comp.spv"
    COMMAND "${GLSLC}" -O -DPASS_${PASS_UP} "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp"
            -o "${SHADER_OUT}/cloud_far_${PASS}.comp.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.comp" "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_far.glsl"
            ${CLOUD_GLSL}
    COMMENT "glslc -DPASS_${PASS_UP} cloud_far.comp")
  list(APPEND SPV_FILES "${SHADER_OUT}/cloud_far_${PASS}.comp.spv")
endforeach()
# the cloud shadow map (ps2_clouds_shadow.c): slice build and blend in one dispatch
add_custom_command(
  OUTPUT "${SHADER_OUT}/cloud_shadow.comp.spv"
  COMMAND "${GLSLC}" -O "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" -o "${SHADER_OUT}/cloud_shadow.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_shadow.comp" ${CLOUD_GLSL}
  COMMENT "glslc cloud_shadow.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/cloud_shadow.comp.spv")
add_custom_command(
  OUTPUT "${SHADER_OUT}/cloud_comp_mv.frag.spv"
  COMMAND "${GLSLC}" -O -DMV "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" -o "${SHADER_OUT}/cloud_comp_mv.frag.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" ${CLOUD_GLSL}
  COMMENT "glslc -DMV cloud_comp.frag")
list(APPEND SPV_FILES "${SHADER_OUT}/cloud_comp_mv.frag.spv")
# the cloud depth stamp for the game's effect sprites (ACZ_VR_CLOUD_FX, bug #46): cloud_comp.frag with -DSTAMP;
# its soft replacement, the effects' cloud veil (ACZ_VR_CLOUD_FX_SOFT, bug #254): -DVEIL
foreach(TRIPLE "cloud_stamp.frag;-DSTAMP" "cloud_stamp_mv.frag;-DSTAMP;-DMV" "cloud_veil.frag;-DVEIL" "cloud_veil_mv.frag;-DVEIL;-DMV")
  list(GET TRIPLE 0 DST)
  list(SUBLIST TRIPLE 1 -1 DEFS)
  add_custom_command(
    OUTPUT "${SHADER_OUT}/${DST}.spv"
    COMMAND "${GLSLC}" -O ${DEFS} "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" -o "${SHADER_OUT}/${DST}.spv"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/cloud_comp.frag" ${CLOUD_GLSL}
    COMMENT "glslc cloud_comp.frag -> ${DST}")
  list(APPEND SPV_FILES "${SHADER_OUT}/${DST}.spv")
endforeach()
# the frame-exact recording's NV12 conversion of the window image (ACZ_REC_EXACT, ps2_video.c rx_*)
add_custom_command(
  OUTPUT "${SHADER_OUT}/rx_nv12.comp.spv"
  COMMAND "${GLSLC}" -O "${CMAKE_SOURCE_DIR}/runtime/shaders/rx_nv12.comp" -o "${SHADER_OUT}/rx_nv12.comp.spv"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/shaders/rx_nv12.comp"
  COMMENT "glslc rx_nv12.comp")
list(APPEND SPV_FILES "${SHADER_OUT}/rx_nv12.comp.spv")
add_custom_target(shaders ALL DEPENDS ${SPV_FILES})
endif()
add_dependencies(ac5 shaders)
if(PS2_BUILD_GSREPLAY)
  add_dependencies(gsreplay shaders)
endif()

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c23")
check_c_source_compiles("
void g(void){}
void f(void){ [[gnu::musttail]] return g(); }
int main(void){ f(); return 0; }" HAVE_MUSTTAIL)
if(NOT HAVE_MUSTTAIL)
  message(WARNING
    "This compiler has no musttail attribute. Cross-function guest loops will "
    "consume stack. GCC 15+ or Clang 13+ is required for a correct build.")
endif()
