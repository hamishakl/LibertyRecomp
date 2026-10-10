#include <rex/graphics/gta4_native/options.h>

REXCVAR_DEFINE_BOOL(gta4_native_vector_fonts, true, "GTA IV/Graphics/Text",
                    "Replace stock compressed font atlases with licensed high-resolution atlases")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gta4_trace_vector_fonts, false, "GTA IV/Diagnostics",
                    "Log the native vector-font identification and replacement path")
    .debug_only();

REXCVAR_DEFINE_BOOL(gta4_native_spatial_aa, true, "GTA IV/Graphics/Anti-Aliasing",
                    "Legacy compatibility toggle for the native spatial edge resolve");

REXCVAR_DEFINE_BOOL(gta4_native_output_dither, true, "GTA IV/Graphics/Post-Processing",
                    "Apply stable display-space dithering to reduce output banding");

REXCVAR_DEFINE_BOOL(gta4_native_hdr_high_precision, true, "GTA IV/Graphics/HDR",
                    "Preserve the final display-ready resolve in FP16 while HDR is active");

REXCVAR_DEFINE_STRING(gta4_texture_filtering, "trilinear", "GTA IV/Graphics/Texture Filtering",
                      "Material texture filtering: bilinear or trilinear")
    .allowed({"bilinear", "trilinear"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(gta4_anisotropic_filtering, "1x", "GTA IV/Graphics/Texture Filtering",
                      "Material anisotropic filtering: 1x, 2x, 4x, 8x, or 16x")
    .allowed({"1x", "2x", "4x", "8x", "16x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(gta4_native_msaa, "4x", "GTA IV/Graphics/Anti-Aliasing",
                      "Legacy deferred-scene multisampling compatibility setting")
    .allowed({"off", "original", "2x", "4x"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(
    gta4_native_light_overrides, "pair", "GTA IV/Graphics/Native Renderer",
    "Shader override selection: stock modules, legacy stage selection, or approved pipeline pairs")
    .allowed({"stock", "stage", "pair"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gta4_native_host_sun_shafts, false, "GTA IV/Graphics/Native Renderer",
                    "Legacy compatibility flag; sun shafts now follow gta4_modern_shaders");

REXCVAR_DEFINE_BOOL(gta4_native_host_fog, false, "GTA IV/Graphics/Native Renderer",
                    "Legacy compatibility flag; fog replacements now follow gta4_modern_shaders");

REXCVAR_DEFINE_UINT32(gta4_native_frames_in_flight, 2, "GTA IV/Graphics/Native Renderer",
                      "Native renderer frame-resource slots (3 lets the CPU encode a frame while "
                      "two are on the GPU; pair with present_frames_ahead=1)")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
