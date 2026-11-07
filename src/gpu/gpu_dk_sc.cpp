/* gpu_dk_sc.cpp -- the game's shaders on the console (stage 12c, docs/deko3d.md): the SPIR-V the
 * compile service's spv target makes from the core's HLSL (ps3recomp's gpu_shader_spv.cpp, in
 * the Vulkan backend's binding model) to GLSL with deko3D's bindings (SPIRV-Cross), then to DKSH
 * (uam, third_party/uam).
 *
 * The spv target's bindings (gpu_shader_spv.h): set 0 holds the CB slots b<n> at binding n, the
 * RAW slots t<n> at 16 + n and the static samplers s<n> at 48 + n; the table slots are sets 1, 2,
 * ... in slot order, entry j of a texture table at binding 16 + j and of a sampler table at
 * 48 + j; the CONST32 slots are the push constant block. deko3D's (docs/deko3d.md): a CB or
 * CONST32 b<n> is uniform buffer n, a RAW t<n> storage buffer n - 16 (from t16), a texture t<j>
 * texture binding j, sampled with the sampler at s<j>.
 *
 * The spv target flips y in its vertex shaders for Vulkan (glslang's invert-y); deko3D keeps
 * D3D12's y, so SPIRV-Cross flips it back. uam's GLSL front end keeps one context for the
 * program's life, so compiles here are one at a time.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(__SWITCH__)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>
#include <vector>

#include "spirv_glsl.hpp"

#include "compiler_iface.h"

extern "C" {
#include "gpu.h"
#include "gpu_shader.h"
}

#include "gpu_dk_sc.h"

namespace {

std::mutex s_lock;

void say(char* msg, size_t cap, const char* fmt, const std::string& what)
{
    if (msg && cap) snprintf(msg, cap, fmt, what.c_str());
}

/* The layout's table slots, in slot order: set s (from 1) is table slot s - 1. */
struct Tables {
    const gpu_layout_slot* slot[GPU_MAX_SLOTS];
    uint32_t n = 0;
};

/* A texture's or sampler's register from its set and binding. */
bool reg_of(const Tables& t, uint32_t set, uint32_t binding, uint32_t base, uint32_t* reg)
{
    if (binding < base) return false;
    if (set == 0) {   /* a static sampler (48 + n) or a RAW slot (16 + n) */
        *reg = binding - base;
        return true;
    }
    if (set > t.n) return false;
    *reg = t.slot[set - 1]->reg + (binding - base);
    return true;
}

}  // namespace

extern "C" int gpu_dk_sc_dksh(gpu_sc_stage stage, const uint32_t* words, size_t nwords, const gpu_layout_desc* layout,
                              void** out, size_t* out_size, char* msg, size_t cap)
{
    if (msg && cap) msg[0] = 0;
    *out = NULL;
    *out_size = 0;
    if (!layout) return 0;
    Tables tables;
    uint32_t const32_reg = 0, nconst32 = 0;
    for (uint32_t s = 0; s < layout->nslots && s < GPU_MAX_SLOTS; s++) {
        const gpu_layout_slot* sl = &layout->slot[s];
        if (sl->kind == GPU_SLOT_TABLE_TEX || sl->kind == GPU_SLOT_TABLE_SAMPLER) tables.slot[tables.n++] = sl;
        if (sl->kind == GPU_SLOT_CONST32) {
            const32_reg = sl->reg;
            nconst32++;
        }
    }
    if (nconst32 > 1) {
        say(msg, cap, "%s", "a layout with more than one CONST32 slot has no deko3D binding here");
        return 0;
    }

    std::string glsl;
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    try {
        spirv_cross::CompilerGLSL c(words, nwords);
        spirv_cross::CompilerGLSL::Options o = c.get_common_options();
        o.version = 460;
        o.es = false;
        o.vulkan_semantics = false;
        o.separate_shader_objects = true;
        o.enable_420pack_extension = true;
        o.emit_push_constant_as_uniform_buffer = true;
        o.vertex.flip_vert_y = stage == GPU_SC_VS;
        o.vertex.fixup_clipspace = false;
        c.set_common_options(o);

        c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        const spirv_cross::ShaderResources r = c.get_shader_resources();

        for (const spirv_cross::Resource& b : r.uniform_buffers) {
            c.set_decoration(b.id, spv::DecorationBinding, c.get_decoration(b.id, spv::DecorationBinding) & 15);
            c.unset_decoration(b.id, spv::DecorationDescriptorSet);
        }
        for (const spirv_cross::Resource& b : r.push_constant_buffers)
            c.set_decoration(b.id, spv::DecorationBinding, const32_reg & 15);
        for (const spirv_cross::Resource& b : r.storage_buffers) {
            const uint32_t binding = c.get_decoration(b.id, spv::DecorationBinding);
            const uint32_t reg = binding >= 16 ? binding - 16 : binding;
            c.set_decoration(b.id, spv::DecorationBinding, (reg >= 16 ? reg - 16 : reg) & 15);
            c.unset_decoration(b.id, spv::DecorationDescriptorSet);
        }
        /* each texture and the sampler it is read with, as one binding at the texture's register */
        for (const spirv_cross::CombinedImageSampler& s : c.get_combined_image_samplers()) {
            const uint32_t iset = c.get_decoration(s.image_id, spv::DecorationDescriptorSet);
            const uint32_t ibind = c.get_decoration(s.image_id, spv::DecorationBinding);
            uint32_t treg = 0;
            if (!reg_of(tables, iset, ibind, 16, &treg)) {
                say(msg, cap, "a texture at a binding the layout does not have (%s)", c.get_name(s.image_id));
                return 0;
            }
            c.set_decoration(s.combined_id, spv::DecorationBinding, treg);
            c.unset_decoration(s.combined_id, spv::DecorationDescriptorSet);
        }
        for (const spirv_cross::Resource& b : r.sampled_images) {   /* (already combined in the module) */
            const uint32_t set = c.get_decoration(b.id, spv::DecorationDescriptorSet);
            const uint32_t bind = c.get_decoration(b.id, spv::DecorationBinding);
            uint32_t treg = 0;
            if (reg_of(tables, set, bind, 16, &treg)) c.set_decoration(b.id, spv::DecorationBinding, treg);
            c.unset_decoration(b.id, spv::DecorationDescriptorSet);
        }
        glsl = c.compile();
    } catch (const std::exception& e) {
        say(msg, cap, "SPIRV-Cross: %s", e.what());
        return 0;
    }

    /* deko3D's device origin is the upper left already: no redeclaration for uam to act on */
    static const char k_origin[] = "layout(origin_upper_left) in vec4 gl_FragCoord;";
    const size_t at = glsl.find(k_origin);
    if (at != std::string::npos) glsl.erase(at, sizeof k_origin - 1);

    pipeline_stage ps = stage == GPU_SC_PS ? pipeline_stage_fragment
                        : stage == GPU_SC_GS ? pipeline_stage_geometry : pipeline_stage_vertex;
    std::lock_guard<std::mutex> hold(s_lock);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* nouveau's optimization level: 3, as uam's own default; DESNX_DK_UAM_OPT_BIG=<0-3> for a
     * program of more than 20 KB of GLSL (a test of what the level costs a long compile) */
    int opt = 3;
    const bool big = glsl.size() > 20000;
    if (big) {
        const char* e = getenv("DESNX_DK_UAM_OPT_BIG");
        if (e && *e >= '0' && *e <= '3') opt = *e - '0';
    }
    /* Mesa's optimization loops once each for a program of more than 20 KB of GLSL: run to
     * their end, they took 14 of a long vertex program's 19 seconds, and nouveau optimizes
     * the code after them anyway. DESNX_DK_CONSERVATIVE=0 or 1: never or always. */
    const char* cons = getenv("DESNX_DK_CONSERVATIVE");
    const bool conservative = cons && *cons ? *cons != '0' : big;
    DekoCompiler::SetOptimizeConservatively(conservative);
    DekoCompiler dc(ps, opt);
    const bool compiled = dc.CompileGlsl(glsl.c_str());
    clock_gettime(CLOCK_MONOTONIC, &t2);
    const double cross_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    const double uam_ms = (double)(t2.tv_sec - t1.tv_sec) * 1e3 + (double)(t2.tv_nsec - t1.tv_nsec) / 1e6;
    if (cross_ms + uam_ms > 500.0 || getenv("DESNX_DK_SHADER_TIMES"))
        printf("[dk] shader %s: SPIRV-Cross %.0f ms, uam %.0f ms (Mesa's GLSL %.0f%s, nouveau at level %d %.0f; %zu "
               "SPIR-V words, %zu bytes of GLSL)\n",
               stage == GPU_SC_PS ? "ps" : stage == GPU_SC_GS ? "gs" : "vs", cross_ms, uam_ms, dc.m_msFrontend,
               conservative ? " once" : "", opt, dc.m_msCodegen, nwords, glsl.size());
    if (!compiled) {
        say(msg, cap, "uam could not compile the GLSL SPIRV-Cross made (%s)", glsl.substr(0, 160));
        if (getenv("DESNX_DK_SHADER_DUMP")) fprintf(stderr, "[dk] the GLSL uam refused:\n%s\n", glsl.c_str());
        return 0;
    }
    if (!dc.OutputDkshToMemory(out, out_size)) {
        say(msg, cap, "%s", "uam's DKSH could not be written to memory");
        return 0;
    }
    return 1;
}

#endif /* PS3RECOMP_RSX_HAL && __SWITCH__ */
