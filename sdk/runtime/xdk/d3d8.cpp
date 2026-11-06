// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "gpu/vertex_program_slots.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_SetBackBufferScale
#define SYM_D3DDevice_SetBackBufferScale FFFFFE04
#define HAS_D3DDevice_SetBackBufferScale 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
// STREET: every write to g_state marks its blocks for the render thread (gpu::g_track_state)
#define TOUCH(field) gpu::TouchState(g_state, &(field), sizeof(field))
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// Keep uploaded programs separate from CreateVertexShader's declaration/function.
// The Godfather links fragments at runtime, uploads them at slot 12, then calls
// SelectVertexShader(declaration, 12). Its declaration handle alone is not a program.
gpu::VertexProgramSlots g_program_slots;
uint32_t g_constant_load = 0, g_program_start = gpu::VertexProgramSlots::kSlots;
uint64_t g_program_revision = 0;
std::shared_ptr<gpu::VertexShader> g_program_decl;
using LoadedProgramKey = std::pair<std::shared_ptr<gpu::VertexShader>, std::vector<uint32_t>>;
std::map<LoadedProgramKey, std::shared_ptr<gpu::VertexShader>> g_loaded_programs;

// VINPUT v1: SetVertexShaderInput supplies 16 native attribute descriptors,
// not a CreateVertexShader handle. Its reusable shader object has a 20-byte
// header followed by {stream, offset, type, tessellation} for each register.
#ifndef HAS_D3D_InputShader
#define HAS_D3D_InputShader 0
#endif
using NativeInputKey = std::array<uint32_t, 64>;
std::map<NativeInputKey, std::shared_ptr<gpu::VertexShader>> g_input_declarations;

uint32_t NativeInputBytes(uint32_t type) {
    switch (type) {
        case 0x40: case 0x16: return 4;
        case 0x72: return 12;  // FLOAT2H: x, y, w
        case 0x12: case 0x22: case 0x32: case 0x42: return (type >> 4) * 4;
        case 0x11: case 0x21: case 0x31: case 0x41:
        case 0x15: case 0x25: case 0x35: case 0x45: return (type >> 4) * 2;
        case 0x14: case 0x24: case 0x34: case 0x44: return type >> 4;
        default: return 0;
    }
}

bool IsNativeInputShader(uint32_t handle) {
#if HAS_D3D_InputShader
    return handle == (SYM(D3D_InputShader) | 1u);
#else
    return false;
#endif
}

std::shared_ptr<gpu::VertexShader> NativeInputDeclaration(uint32_t handle) {
    if (!IsNativeInputShader(handle)) return {};
    NativeInputKey key;
    std::memcpy(key.data(), HostPtr((handle & ~1u) + 20), sizeof(key));
    auto it = g_input_declarations.find(key);
    if (it != g_input_declarations.end()) return it->second;
    auto vs = std::make_shared<gpu::VertexShader>();
    vs->handle = handle;
    uint32_t mask = 0;
    for (uint32_t r = 0; r < 16; ++r) {
        uint32_t stream = key[r * 4], offset = key[r * 4 + 1], type = key[r * 4 + 2];
        if (type == 0x02) continue;  // D3DVSDT_NONE
        uint32_t bytes = NativeInputBytes(type);
        if (stream >= 16 || !bytes || offset > UINT32_MAX - bytes || key[r * 4 + 3]) {
            static unsigned failures = 0;
            if (failures++ < 4) LOGR("VINPUT invalid r=%u stream=%u offset=%u type=%X tess=%X",
                                     r, stream, offset, type, key[r * 4 + 3]);
            return {};
        }
        vs->inputs[r] = {int(stream), offset, type};
        vs->stream_stride[stream] = std::max(vs->stream_stride[stream], offset + bytes);
        mask |= 1u << r;
    }
    // Native code overwrites the same record on the next call. Cache immutable
    // layouts by their full contents so queued draws keep their own attributes.
    g_input_declarations.emplace(key, vs);
    static unsigned reports = 0;
    if (reports++ < 8) LOGR("VINPUT native declaration handle=%08X regs=%04X stride0=%u",
                           handle, mask, vs->stream_stride[0]);
    return vs;
}

std::shared_ptr<gpu::VertexShader> LoadedProgram(const std::shared_ptr<gpu::VertexShader>& decl, uint32_t start) {
    std::vector<uint32_t> words;
    if (!decl || !g_program_slots.Read(start, words)) {
        static unsigned failures = 0;
        if (failures++ < 8) LOGR("VPSLOT invalid program at slot=%u declaration=%08X", start, decl ? decl->handle : 0);
        return {};
    }
    LoadedProgramKey key{decl, std::move(words)};
    auto it = g_loaded_programs.find(key);
    if (it != g_loaded_programs.end()) return it->second;
    // Draws queued on the render thread own the previous object. Never mutate it.
    auto vs = std::make_shared<gpu::VertexShader>();
    vs->handle = decl->handle;
    vs->decl = decl->decl;
    vs->inputs = decl->inputs;
    vs->stream_stride = decl->stream_stride;
    vs->constants = decl->constants;
    vs->program = key.second;
    vs->program_type = 0x2078;
    vs->hash = 0;
    g_loaded_programs.emplace(std::move(key), vs);
    static unsigned reports = 0;
    if (reports++ < 24) LOGR("VPSLOT selected handle=%08X slot=%u instructions=%zu stride0=%u",
                           vs->handle, start, vs->program.size() / 4, vs->stream_stride[0]);
    return vs;
}

void RefreshLoadedProgram() {
    if (!g_program_decl || g_program_revision == g_program_slots.Revision()) return;
    g_program_revision = g_program_slots.Revision();
    if (auto vs = LoadedProgram(g_program_decl, g_program_start)) g_state.vs = std::move(vs);
}

bool SelectLoadedProgram(uint32_t handle, uint32_t start) {
    // A zero handle changes the instruction address while keeping the declaration.
    if (!handle) handle = g_state.vs_handle;
    std::shared_ptr<gpu::VertexShader> decl;
    if (IsNativeInputShader(handle)) {
        decl = NativeInputDeclaration(handle);
    } else {
        auto it = g_vs.find(handle);
        if (it != g_vs.end()) decl = it->second;
    }
    if (!decl) return false;
    auto vs = LoadedProgram(decl, start);
    if (!vs) return false;
    g_program_decl = decl;
    g_program_start = start;
    g_program_revision = g_program_slots.Revision();
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = handle;
    g_state.vs = std::move(vs);
    if (XDK_CDevice_VertexShader != xdk::kUnknown) {
        SetDev(XDK_CDevice_VertexShader + 4, handle);
        SetDev(XDK_CDevice_VertexShader + 8, start);
    }
    return true;
}

bool ApplyVertexProgramMethod(uint32_t method, uint32_t value) {
    if (g_program_slots.Method(method, value)) return true;
    if (method == 0x1EA0) {
        SelectLoadedProgram(0, value);
        return true;
    }
    if (method == 0x1EA4) {
        g_constant_load = value;
        return true;
    }
    if (method >= 0x0B80 && method < 0x0C00 && !(method & 3)) {
        uint32_t lane = ((method - 0x0B80) / 4) & 3;
        if (g_constant_load < g_state.vsc.size()) {
            gpu::TouchVsc(g_state, g_constant_load);
            std::memcpy(&g_state.vsc[g_constant_load][lane], &value, 4);
            if (lane == 3) ++g_constant_load;
        }
        return true;
    }
    return false;
}

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000, "D3D push buffer");
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000, "D3D surface");
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) {
        uint32_t v = R32(kRenderState + i * 4);
        if (v != g_state.rs[i]) TOUCH(g_state.rs[i]), g_state.rs[i] = v;
    }
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) {
            uint32_t v = R32(kTextureState + (s * kStageStates + i) * 4);
            if (v != g_state.tss[s][i]) TOUCH(g_state.tss[s][i]), g_state.tss[s][i] = v;
        }
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    TOUCH(g_state.zscale);
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        gpu::TouchVsc(g_state, 58, 2);
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    TOUCH(g_state.rs[rs]);
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGR("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X swap effect %u", w, h,
         fmt, count, auto_depth, depth_fmt, interval, R32(pp + 0x28), R32(pp + 0x14));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend), &g_state);
    }
    g_state = gpu::State{};
    g_program_slots = {};
    g_constant_load = 0;
    g_program_start = gpu::VertexProgramSlots::kSlots;
    g_program_revision = 0;
    g_program_decl.reset();
    g_loaded_programs.clear();
    g_input_declarations.clear();
    LOGR("VINPUT fix v1: native effect declarations enabled");
    LOGR("VPSLOT fix v1: uploaded vertex programs and SelectVertexShader enabled");
    gpu::TouchAllState();
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    TOUCH(g_state.rt);
    TOUCH(g_state.zs);
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

// RTFAST v1: the title uses the native three-argument fast path for its effect
// surfaces. Its raw COLOR/ZETA_OFFSET packets do not update the HLE surfaces.
// Run the original body to preserve internal references, fences and push state,
// then mirror its chosen headers. Unlike SetRenderTarget, this preserves the
// viewport and its shader constants; the title changes those separately.
#if defined(HAS_D3DDevice_SetRenderTargetFast) && HAS_D3DDevice_SetRenderTargetFast
X86_EXTERN_FUNC(GUEST_ORIG(D3DDevice_SetRenderTargetFast));
X86_FUNC(GUEST(D3DDevice_SetRenderTargetFast)) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // queued work still belongs to the previous target
    GUEST_ORIG(D3DDevice_SetRenderTargetFast)(ctx, base);
    TOUCH(g_state.rt);
    TOUCH(g_state.zs);
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    g_state.zs = SurfaceOf(Dev(kDevDepth));
    static unsigned reports = 0;
    if (reports < 16) {
        ++reports;
        LOGR("RTFAST target=%08X data=%08X depth=%08X viewport=%ux%u",
             g_state.rt.header, g_state.rt.data, g_state.zs.header,
             g_state.viewport.w, g_state.viewport.h);
    }
}
#endif

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget), zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    TOUCH(g_state.rt);
    TOUCH(g_state.zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    TOUCH(g_state.viewport);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    static unsigned boot_swaps = 0;
    const bool trace_boot = std::getenv("STREET_CAPTURE_BOOT") && boot_swaps++ < 120;
    if (trace_boot) LOGR("STREET swap: enter");
    app::SplashFirstFrame();
    if (trace_boot) LOGR("STREET swap: splash complete");
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // pacing: a frame of the title thread (swap to swap) over 50 ms, the game's own time included; the render thread's
    // presents are logged apart (deko.cpp), so a hitch says which side it came from
    {
        static auto last = std::chrono::steady_clock::now();
        static unsigned reported = 0;
        static const double limit = [] {  // STREET_HITCH_MS lowers it on a fast PC
            const char* v = std::getenv("STREET_HITCH_MS");
            return v ? std::atof(v) : 50.0;
        }();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (ms > limit && g_presents > 2 && reported < 400) {
            reported++;
            LOGR("pacing: title thread frame of %.0f ms (swap %u)", ms, g_presents.load());
        }
    }
    // diagnostic: STREET_SHOT_SWAPS=N captures each of the first N frames and logs what made it
    static const int shot_swaps = [] {
        const char* v = std::getenv("STREET_SHOT_SWAPS");
        return v ? std::atoi(v) : 0;
    }();
    if (int(g_presents.load()) <= shot_swaps) {
        static uint64_t draws_before = 0;
        uint64_t draws = gpu::g_perf_draws.load();
        LOGR("d3d: swap %u: %llu draws, render target %08X, back buffer %08X", g_presents.load(),
             (unsigned long long)(draws - draws_before), Dev(kDevRenderTarget), R32(g_buffers[1] + 4));
        draws_before = draws;
        gpu::g_screenshot.store(true);
    }
    static uint32_t last_flags = ~0u;
    static unsigned flag_reports = 0;
    if (a[0] != last_flags && flag_reports < 16) {
        last_flags = a[0];
        ++flag_reports;
        LOGR("d3d: Swap flags %X from swap %u", a[0], g_presents.load());
    }
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (trace_boot) LOGR("STREET swap: walk complete, presenting");
    const uint32_t shown = R32(g_buffers[1] + 4);
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    if (trace_boot) LOGR("STREET swap: present complete");
    app::Pace();
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    if (gpu::g_swap_keeps_back_buffer && g_backend && g_buffer_count > 1) {
        // the frame just shown, wherever its memory went, into the new back buffer
        for (uint32_t i = 0; i < g_buffer_count; i++)
            if (i != 1 && R32(g_buffers[i] + 4) == shown) {
                g_backend->Copy(SurfaceOf(g_buffers[i]), SurfaceOf(g_buffers[1]), 0, 0, 0);
                break;
            }
    }
    TOUCH(g_state.rt);  // the same header, its memory rotated
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    // STREET XBE 00537DD8 tests EBP, 00537DE1 jumps to the common
    // update at 00537FC3 when null; preserve the existing viewport.
    TOUCH(g_state.viewport);
    TOUCH(g_state.scissor_count);
    if (v)
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TOUCH(g_state.scissor_count);
    TOUCH(g_state.scissors);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    TOUCH(g_state.ss_offset_x);
    TOUCH(g_state.ss_offset_y);
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

#if HAS_D3DDevice_SetBackBufferScale
// The XDK stores the scale times the multisample factors in the device and then rebuilds
// its frame buffer setup through driver state the host does not make (it read a null pointer). The host draws the
// back buffer at its own size, so the scale is kept for the library code that reads it and nothing else is done.
HLE_STDCALL(D3DDevice_SetBackBufferScale, 2) {
    float x = ArgF(a, 0), y = ArgF(a, 1);
    LOG_FIRST(8, "d3d: SetBackBufferScale %.3f %.3f (kept in the device, not applied)", x, y);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &x, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &y, 4);
    }
    return 0;
}
#endif

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    gpu::TouchSimple(g_state, method >> 2);
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        gpu::TouchSimple(g_state, method >> 2);
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    TOUCH(g_state.tss[a[0] & 3]);
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        TOUCH(g_state.tss[stage][type]);
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    TOUCH(g_state.transforms[pc]);
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    TOUCH(g_state.material);
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        TOUCH(g_state.lights[i]);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        TOUCH(g_state.light_enable);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush)); // Submit queued draws before replacing their texture state.
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    Release(ctx, old);
    TOUCH(g_state.textures[stage]);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush)); // Preserve palette state of queued indexed-font draws.
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    TOUCH(g_state.palettes[stage]);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    TOUCH(g_state.textures[stage]);  // the same header, new fields
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    Release(ctx, old);
    TOUCH(g_state.streams[n]);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    TOUCH(g_state.index_buffer);
    TOUCH(g_state.index_data);
    TOUCH(g_state.base_vertex);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t create_return = logx::kVerbose ? R32(ctx.esp) : 0;
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    static unsigned create_trace_count = 0;
    if (logx::kVerbose && create_trace_count++ < 128) {
        uint32_t hdr = func ? R32(func) : 0;
        LOGR("VSCREATE ret=%08X handle=%08X decl=%08X func=%08X out=%08X usage=%08X hdr=%08X type=%04X instructions=%u",
             create_return, vs->handle, decl, func, out, a[3], hdr, hdr & 0xFFFF, hdr >> 16);
    }
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        for (auto p = g_loaded_programs.begin(); p != g_loaded_programs.end();) {
            if (p->first.first == it->second) p = g_loaded_programs.erase(p);
            else ++p;
        }
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush)); // Queued vertices retain the declaration used when submitted.
    g_program_decl.reset();
    uint32_t h = a[0];
    // QUIETDRAW v6: legacy mesh probes run only in verbose builds.
    if constexpr (logx::kVerbose) {
        static const auto shader_trace_start = std::chrono::steady_clock::now();
        static bool target_shader_first_seen = false;
        static unsigned target_shader_late_trace_count = 0;
        const auto shader_trace_elapsed = std::chrono::steady_clock::now() - shader_trace_start;
        auto trace_it = g_vs.find(h);
        if (h == 0xB0102291 && !target_shader_first_seen) {
            target_shader_first_seen = true;
            LOGR("VSSELECT first ret=%08X handle=%08X known=%u instructions=%zu type=%04X",
                 R32(ctx.esp), h, unsigned(trace_it != g_vs.end()),
                 trace_it == g_vs.end() ? size_t(0) : trace_it->second->program.size() / 4,
                 trace_it == g_vs.end() ? 0 : trace_it->second->program_type);
        }
        if (h == 0xB0102291 && shader_trace_elapsed >= std::chrono::seconds(40) && target_shader_late_trace_count++ < 128) {
            LOGR("VSSELECT late ret=%08X handle=%08X known=%u instructions=%zu type=%04X",
                 R32(ctx.esp), h, unsigned(trace_it != g_vs.end()),
                 trace_it == g_vs.end() ? size_t(0) : trace_it->second->program.size() / 4,
                 trace_it == g_vs.end() ? 0 : trace_it->second->program_type);
        }
    }
    // STREET native SetVertexShader at 0053A134 stores the handle at device+0798.
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_VertexShader));
    SetDev(XDK_CDevice_VertexShader + 4, h);
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) gpu::TouchVsc(g_state, reg), g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

namespace {
void TraceVscWrite(X86Context& ctx, const char* api, uint32_t reg, uint32_t src, uint32_t count) {
    if constexpr (!logx::kVerbose) return;
    const int32_t signed_reg = static_cast<int32_t>(reg);
    const bool logical_high = signed_reg <= -96 && signed_reg >= -99;
    const bool physical_high = reg <= 99 && uint64_t(reg) + count > 96;
    if ((!logical_high && !physical_high) || !src) return;
    static std::unordered_map<uint64_t, std::array<uint32_t, 4>> last_values;
    static unsigned reports = 0;
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(src));
    auto report_value = [&](uint32_t slot, uint32_t byte_offset, const char* label) {
        if (reports >= 256) return;
        std::array<uint32_t, 4> bits{};
        std::memcpy(bits.data(), p + byte_offset, 16);
        uint64_t key = (uint64_t(R32(ctx.esp)) << 32) | slot;
        auto it = last_values.find(key);
        if (it != last_values.end() && it->second == bits) return;
        last_values[key] = bits;
        float v[4];
        std::memcpy(v, bits.data(), sizeof(v));
        LOGR("VSCWRITE ret=%08X api=%s reg=%08X signed=%d %s=(%.7g %.7g %.7g %.7g)",
             R32(ctx.esp), api, reg, signed_reg, label, v[0], v[1], v[2], v[3]);
        reports++;
    };
    if (physical_high) {
        for (uint32_t i = 0; i < count && i < 16; i++) {
            uint64_t index = uint64_t(reg) + i;
            if (index >= 96 && index <= 99) report_value(uint32_t(index), i * 16, ("c" + std::to_string(index)).c_str());
        }
    } else {
        report_value(0x80000000u | uint32_t(-signed_reg), 0, "signed-source");
    }
}
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    TraceVscWrite(ctx, "Constant1Fast", reg, a[1], 1);
    gpu::TouchVsc(g_state, reg);
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    TraceVscWrite(ctx, "Constant1", reg, a[1], 1);
    gpu::TouchVsc(g_state, reg);
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0], src = a[1];
    TraceVscWrite(ctx, "Constant4", reg, src, 4);
    gpu::TouchVsc(g_state, reg, 4);
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    TraceVscWrite(ctx, "ConstantNotInlineFast", reg, src, dwords / 4);
    gpu::TouchVsc(g_state, reg, dwords / 4);
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end() || !g_program_slots.Load(a[1], it->second->program)) return 0x8876086C;
    RefreshLoadedProgram();
    return 0;
}
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    return SelectLoadedProgram(a[0], a[1]) ? 0 : 0x8876086C;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) {
        gpu::TouchSimple(g_state, method >> 2);
        g_state.simple[method >> 2] = v;
    };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    TOUCH(g_state.psc);
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

// Diagnostic only: log the first draw seen at each game call site and vertex shader.
namespace {
void TraceDrawCaller(X86Context& ctx, const char* api, uint32_t prim, uint32_t count, uint32_t stride, uint32_t parent_offset) {
    if constexpr (!logx::kVerbose) return;
    static uint32_t logged = 0;
    const auto& s = State();
    uint32_t wrapper = R32(ctx.esp);
    if (wrapper != 0x001FA5A9 || s.vs_handle != 0xB0102291 || logged >= 16) return;
    uint32_t parent = R32(ctx.esp + parent_offset);
    uint32_t frame = ctx.ebp;
    uint32_t frame_ret = R32(frame + 4);
    uint32_t arg0 = R32(frame + 8);
    uint32_t arg1 = R32(frame + 12);
    LOGR("DRAWCALL n=%u parent=%08X wrapper=%08X frame=%08X frameRet=%08X arg0=%08X arg1=%08X api=%s vs=%08X rt=%08X prim=%u count=%u stride=%u vblend=%u cull=%u",
         logged++, parent, wrapper, frame, frame_ret, arg0, arg1, api, s.vs_handle, s.rt.data, prim, count, stride, s.rs[gpu::kRsVertexBlend], s.rs[gpu::kRsCullMode]);
}
}

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDrawCaller(ctx, "DrawVertices", a[0], a[2], 0, 16);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDrawCaller(ctx, "DrawIndexedVertices", a[0], a[1], 0, 16);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRingMax = 4u << 20, kUpRingMin = 256u << 10;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};
uint32_t g_up_ring_size = 0;
bool g_up_ring_system = false;

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!src || !bytes) return 0;

    // Each half is alternated while the GPU consumes the other. Never truncate a draw to
    // the half-ring size: the backend still uses the original vertex count and would read
    // past the bytes copied here. Grow the ring if a later character/scene mesh needs more.
    uint32_t required = bytes <= UINT32_MAX / 2 ? bytes * 2 : UINT32_MAX;
    uint32_t wanted = kUpRingMin;
    while (wanted < required && wanted < (32u << 20)) wanted <<= 1;
    if (wanted < required) {
        LOG_FIRST(1, "d3d: inline vertex draw too large (%u bytes); skipping", bytes);
        return 0;
    }

    if (!g_up_ring || g_up_ring_size < wanted) {
        // Earlier draws may still reference the old physical block. Drain them before
        // replacing it, then try a suitably sized contiguous allocation. Smaller blocks
        // are useful on the 64 MiB Xbox address space, but only if they fit this draw.
        if (g_up_ring) {
            uint32_t fence = g_backend ? g_backend->Fence() : 0;
            if (fence) g_backend->WaitFence(fence);
            if (g_up_ring_system) mem::FreeSystem(g_up_ring);
            else mem::FreeContiguous(g_up_ring);
            g_up_ring = g_up_ring_size = g_up_pos = 0;
            g_up_ring_system = false;
            g_up_fence[0] = g_up_fence[1] = 0;
        }
        // Xbox RAM is only 64 MiB and can be fragmented by resources before the first inline draw.
        // Try smaller rings when the full 4 MiB block is unavailable, but never below the draw size.
        for (uint32_t size = std::max(kUpRingMax, wanted); size >= wanted; size >>= 1) {
            g_up_ring = mem::AllocContiguous(size, 0, 0xFFFFFFFF, 0x1000, "DrawVerticesUP ring");
            if (g_up_ring) {
                g_up_ring_size = size;
                LOGR("d3d: inline vertex ring %u KiB (contiguous memory)", size >> 10);
                break;
            }
        }
        if (!g_up_ring) {
            // Deko reads guest memory through HostPtr, so it can use a system-pool block
            // when the Xbox physical pool is full or too fragmented for a contiguous ring.
            g_up_ring = mem::AllocSystem(wanted, 0x1000);
            if (g_up_ring) {
                g_up_ring_size = wanted;
                g_up_ring_system = true;
                LOGR("d3d: inline vertex ring %u KiB (system memory)", wanted >> 10);
            }
        }
        if (!g_up_ring) {
            static bool reported_unavailable = false;
            if (!reported_unavailable) {
                LOGR("d3d: inline vertex ring unavailable for %u-byte draw; skipping", bytes);
                reported_unavailable = true;
            }
            return 0;
        }
        if (!g_up_header) {
            g_up_header = mem::AllocSystem(20);
            if (!g_up_header) {
                if (g_up_ring_system) mem::FreeSystem(g_up_ring);
                else mem::FreeContiguous(g_up_ring);
                g_up_ring = g_up_ring_size = 0;
                g_up_ring_system = false;
                LOG_FIRST(1, "d3d: inline vertex header unavailable; skipping inline draw");
                return 0;
            }
            std::memset(HostPtr(g_up_header), 0, 20);
            W32(g_up_header, 1);  // a vertex buffer, one reference
        }
    }
    const uint32_t up_half = g_up_ring_size / 2;
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > g_up_ring_size ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / up_half : 1, to = (pos + room - 1) / up_half;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    auto stream = g_state.streams[0];
    TOUCH(g_state.streams[0]);
    uint32_t buffer = UpVertices(vertices, count * stride);
    if (!buffer) return;
    g_state.streams[0] = {buffer, stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    TOUCH(g_state.streams[0]);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

// NV097_INLINE_ARRAY data for XYZ streams is padded to a four-float slot by this title,
// even though the declaration's useful position components occupy only 12 bytes. The
// rejected batches in the Switch log are all 16-byte aligned (e.g. 2912, 3136, 3584), so
// keep the padding in the stream stride and let the declaration read its XYZ at offset 0.
uint32_t InlineStride(uint32_t bytes) {
    uint32_t stride = VertexBytes();
    if (stride && bytes % stride == 0) return stride;
    if (stride == 12 && bytes % 16 == 0) {
        static bool reported = false;
        if (!reported) {
            LOGR("inline draw: XYZ data uses 16-byte padded stream slots");
            reported = true;
        }
        return 16;
    }
    return 0;
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex
std::vector<uint32_t> g_im_inline;      // NV097_INLINE_ARRAY words across packets

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) gpu::TouchSimple(g_state, gpu::kMCountEnable >> 2), g_state.simple[gpu::kMCountEnable >> 2] = v;
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    RefreshLoadedProgram();
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (prim && !g_im_inline.empty()) {
        uint32_t bytes = uint32_t(g_im_inline.size()) * 4;
        uint32_t stride = InlineStride(bytes);
        if (stride) {
            static unsigned reports = 0;
            if (reports++ < 8) LOGR("inline draw: primitive=%u count=%u stride=%u", prim, bytes / stride, stride);
            DrawUp(prim, bytes / stride, g_im_inline.data(), stride);
        } else LOGR("inline draw rejected: bytes=%u stride=%u", bytes, stride);
        g_im_inline.clear();
        return;
    }
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = handle;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            ApplyVisibilityMethod(m, v);
            if (ApplyVertexProgramMethod(m, v)) continue;
            if (m == 0x1818) {
                if (g_im_prim) g_im_inline.push_back(v);
                continue;
            }
            if (m == 0x17FC) {  // begin/end, 0 ends
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                    g_im_inline.clear();
                } else {
                    EndPrimitive();
                }
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else {
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
    }
    g_walk = end;
    RefreshLoadedProgram();
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDrawCaller(ctx, "DrawVerticesUP", a[0], a[1], a[3], 20);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    // PUSHORDER v1: Godfather's native BeginPush (004A5230) keeps running the
    // XDK's deferred-state setup. Its EndPush (004A5250) used to only advance
    // the producer pointer, leaving immediate draws to a later Walk after the
    // title had already changed the pixel shader, blend state or render target.
    // Consume that native submission now, including per-attribute vertices.
    // Keep the producer pointer and allocation: native callers own this range.
    if (!g_push_begin) {
        uint32_t end = a[0];
        if (!g_walk || end < g_walk || uint64_t(end) > uint64_t(g_scratch_push) + g_scratch_size) {
            LOG_FIRST(4, "PUSHORDER invalid native EndPush end=%08X first=%08X", end, g_walk);
            return 0x8876086C;
        }
        static unsigned native_reports = 0;
        if (native_reports < 2) {
            ++native_reports;
            LOGR("PUSHORDER native EndPush: consuming %u bytes before the next pass", end - g_walk);
        }
        SetDev(kDevPush, end);
        Walk(end);
        return end;
    }
    TraceDrawCaller(ctx, "EndPush", 0, 0, 0, 8);
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (ApplyVertexProgramMethod(m, v)) continue;
            if (logx::kVerbose && m >= 0x1E80 && m <= 0x1E90) {
                static unsigned transform_trace_count = 0;
                if (transform_trace_count++ < 128)
                    LOGR("PUSHVS method=%04X value=%08X command=%u/%u", m, v, i, n);
            }
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) gpu::TouchSimple(g_state, m >> 2), g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                RefreshLoadedProgram();
                uint32_t bytes = uint32_t(vertices.size()) * 4;
                uint32_t stride = InlineStride(bytes);
                if (stride) DrawUp(prim, bytes / stride, vertices.data(), stride);
                else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    // EndPush has already consumed these commands. ScratchPush must not replay
    // their constant writes after the draw, or duplicate their inline vertices.
    g_walk = a[0];
    RefreshLoadedProgram();
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
