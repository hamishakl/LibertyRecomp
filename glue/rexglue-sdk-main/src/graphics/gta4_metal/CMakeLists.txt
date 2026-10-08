# One title renderer implementation is linked by the plugin and GPU contract tests.
# Offline shader conversion is not part of this runtime library.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(METAL_RENDERER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(METAL_NATIVE_HEADER "${METAL_RENDERER_GENERATED}/metal_native_library.h")
add_custom_command(OUTPUT "${METAL_NATIVE_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${REXGLUE_ROOT}/tools/compile_metal_library.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native.metal"
        --output "${METAL_NATIVE_HEADER}" --symbol kMetalNativeLibrary
        --sdk macosx --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    DEPENDS shaders/native.metal "${REXGLUE_ROOT}/tools/compile_metal_library.py"
    COMMENT "Compiling Metal attachment-operation shaders" VERBATIM)
set(METAL_TEMPORAL_HEADER "${METAL_RENDERER_GENERATED}/metal_temporal_library.h")
add_custom_command(OUTPUT "${METAL_TEMPORAL_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${REXGLUE_ROOT}/tools/compile_metal_library.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/temporal.metal"
        --output "${METAL_TEMPORAL_HEADER}" --symbol kMetalTemporalLibrary
        --strict-math
        --sdk macosx --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    DEPENDS shaders/temporal.metal shaders/temporal_exposure.metal shaders/temporal_motion_coverage.metal "${REXGLUE_ROOT}/tools/compile_metal_library.py"
    COMMENT "Compiling native temporal reconstruction and UI shaders" VERBATIM)
add_library(rex-gta4-metal-temporal STATIC temporal/effects.mm temporal/library.mm temporal/motion_coverage.mm "${METAL_TEMPORAL_HEADER}")
set_source_files_properties(temporal/effects.mm temporal/library.mm temporal/motion_coverage.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_target_properties(rex-gta4-metal-temporal PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rex-gta4-metal-temporal PUBLIC "${REXGLUE_ROOT}/src" PRIVATE "${METAL_RENDERER_GENERATED}")
target_link_libraries(rex-gta4-metal-temporal PUBLIC "-framework Metal" "-framework MetalFX" "-framework Foundation")

set(METAL_RENDERER_SOURCES renderer.mm pipelines.mm pipeline_store.mm gpu_pass_timer.mm draw.mm attachment_passes.mm modern_sky_probe.mm
    resolve.mm depth_passes.mm readback.mm present.mm post_processing.mm depth_of_field.mm sun_shafts.mm shaders.mm
    resources.mm resource_buffers.mm resource_textures.mm resource_samplers.mm resource_fonts.mm
    texture_preparation.mm temporal/live.mm)
set_source_files_properties(${METAL_RENDERER_SOURCES} graphics_system.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
add_library(rex-gta4-metal-renderer STATIC ${METAL_RENDERER_SOURCES}
    guest_state.cpp shader_archive.cpp ../gta4_native/core/font_atlas.cpp "${METAL_NATIVE_HEADER}")
set_target_properties(rex-gta4-metal-renderer PROPERTIES CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rex-gta4-metal-renderer PUBLIC "${REXGLUE_ROOT}/src"
    PRIVATE "${METAL_RENDERER_GENERATED}" "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib")
target_compile_definitions(rex-gta4-metal-renderer PRIVATE
    GTA4_NATIVE_FONT_ASSET_ROOT="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/font_atlases")
target_link_libraries(rex-gta4-metal-renderer PUBLIC rexruntime xxHash::xxhash rex-gta4-metal-temporal
    "-framework Metal" "-framework QuartzCore" "-framework Foundation")

add_library(rexgpu-gta4-metal SHARED plugin_main.cpp graphics_system.mm)
add_library(rex::gpu-gta4-metal ALIAS rexgpu-gta4-metal)
set_target_properties(rexgpu-gta4-metal PROPERTIES EXPORT_NAME gpu-gta4-metal
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(rexgpu-gta4-metal PRIVATE rex-gta4-metal-renderer)

if(REXGLUE_BUILD_TESTS)
    add_executable(rex-metal-pipeline-lookup-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_pipeline_lookup_test.cpp")
    target_include_directories(rex-metal-pipeline-lookup-test PRIVATE "${REXGLUE_ROOT}/src")
    target_compile_options(rex-metal-pipeline-lookup-test PRIVATE -UNDEBUG)
    add_executable(rex-metal-shader-metadata-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_shader_metadata_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_shader_metadata_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-shader-metadata-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-state-tracking-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_state_tracking_test.cpp")
    target_include_directories(rex-metal-state-tracking-test PRIVATE
        "${REXGLUE_ROOT}/include" "${REXGLUE_ROOT}/src")
    target_compile_options(rex-metal-state-tracking-test PRIVATE -UNDEBUG)
    add_executable(rex-metal-encoder-residency-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_encoder_residency_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_encoder_residency_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(rex-metal-encoder-residency-test PRIVATE "${REXGLUE_ROOT}/src")
    target_link_libraries(rex-metal-encoder-residency-test PRIVATE "-framework Metal" "-framework Foundation")
    add_executable(rex-metal-upload-arena-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_upload_arena_cpu_test.mm"
        "${REXGLUE_ROOT}/src/ui/metal/upload_arena.mm" "${REXGLUE_ROOT}/src/ui/metal/frame_ring.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_upload_arena_cpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(rex-metal-upload-arena-test PRIVATE -fobjc-arc)
    target_include_directories(rex-metal-upload-arena-test PRIVATE "${REXGLUE_ROOT}/src")
    target_link_libraries(rex-metal-upload-arena-test PRIVATE "-framework Metal" "-framework Foundation")
    add_executable(rex-metal-constant-cache-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_constant_cache_test.cpp")
    target_include_directories(rex-metal-constant-cache-test PRIVATE "${REXGLUE_ROOT}/src")
    target_compile_options(rex-metal-constant-cache-test PRIVATE -UNDEBUG)
    add_executable(rex-metal-modern-sky-probe-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_modern_sky_probe_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_modern_sky_probe_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-modern-sky-probe-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-render-phase-stack-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_render_phase_stack_test.cpp")
    target_include_directories(rex-metal-render-phase-stack-test PRIVATE
        "${REXGLUE_ROOT}/include" "${REXGLUE_ROOT}/src")
    add_executable(rex-metal-temporal-pacing-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_pacing_test.cpp")
    target_link_libraries(rex-metal-temporal-pacing-test PRIVATE rexruntime)
    add_executable(rex-metal-temporal-draw-policy-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_draw_policy_test.cpp")
    target_include_directories(rex-metal-temporal-draw-policy-test PRIVATE
        "${REXGLUE_ROOT}/include" "${REXGLUE_ROOT}/src")
    add_executable(rex-metal-temporal-queue-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_queue_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_queue_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-temporal-queue-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-temporal-live-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_live_gpu_test.mm"
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_coverage_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_live_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-temporal-live-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-temporal-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_gpu_test.mm"
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_coverage_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_gpu_test.mm"
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_coverage_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-temporal-test PRIVATE rex-gta4-metal-temporal)
    add_executable(rex-metal-temporal-motion-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_motion_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_temporal_motion_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-temporal-motion-test PRIVATE rex-gta4-metal-temporal)
    add_executable(rex-metal-sun-shafts-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_sun_shafts_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_sun_shafts_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-sun-shafts-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-dof-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_dof_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_dof_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-dof-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-output-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_output_gpu_test.mm"
        "${REXGLUE_ROOT}/src/ui/metal/immediate_drawer.mm")
    # The production presenter and drawer share hidden context RTTI in rexui.
    # Keep the test-created context and its drawer in the same module too.
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_output_gpu_test.mm"
        "${REXGLUE_ROOT}/src/ui/metal/immediate_drawer.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(rex-metal-output-test PROPERTIES CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_link_libraries(rex-metal-output-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-renderer-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_renderer_gpu_test.mm")
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_renderer_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rex-metal-renderer-test PRIVATE rex-gta4-metal-renderer)
    add_executable(rex-metal-resources-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_resources_gpu_test.mm"
        )
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_resources_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(rex-metal-resources-test PRIVATE "${REXGLUE_ROOT}/src")
    target_link_libraries(rex-metal-resources-test PRIVATE rex-gta4-metal-renderer "-framework Metal" "-framework Foundation")
    add_executable(rex-metal-frontend-test EXCLUDE_FROM_ALL
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_frontend_gpu_test.mm"
        guest_state.cpp)
    set_source_files_properties("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/metal_frontend_gpu_test.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(rex-metal-frontend-test PRIVATE "${REXGLUE_ROOT}/src")
    target_link_libraries(rex-metal-frontend-test PRIVATE rexruntime
        "-framework Metal" "-framework Foundation")
endif()
