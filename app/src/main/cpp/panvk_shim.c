/*
 * panvk_shim.c — minimal loader shim between LWJGL/SDL/Mesa-Zink and a
 * Vulkan ICD (PanVK).
 *
 * Original author: d4urw (PlazasCristofher) — Basalt Driver project
 *   https://github.com/PlazasCristofher/Basalt-Driver
 * The original design and implementation are his work; this file is a
 * modified derivative and keeps the original Apache-2.0 license.
 *
 * Modified by xzvince2 (v5):
 *   - English comments/logging.
 *   - ANativeWindow acquire/release on shim-created surfaces (lifetime fix).
 *   - Surface registry: only free shim-created surfaces, forward the rest.
 *   - vkDestroyInstance wrapper resets cached instance/device-proc state.
 *   - Cached instance/gdpa are atomic; ICD interface negotiation falls back
 *     to vk_icdGetInstanceProcAddr (ICD interface v7); physical-device proc
 *     fallback via vk_icdGetPhysicalDeviceProcAddr (v4+).
 *   - Surface layout static asserts; VK_ICD_WSI_PLATFORM_ANDROID verified
 *     against Khronos vk_icd.h (MIR,WAYLAND,WIN32,XCB,XLIB,ANDROID => 5).
 *   - PROF: frame-time p50/p99, approx 1% low fps, stutter count, acquire
 *     result counters; logs ICD device/driver version once.
 *
 * The handle owner (LWJGL, SDL, patched Pojav-style Zink) dlsym's:
 *   vkGetInstanceProcAddr / vkGetDeviceProcAddr
 * The ICD (libvulkan_panfrost.so) only exports vk_icd* entry points, so this
 * shim exports exactly those two symbols and does the minimal loader job:
 * ICD interface negotiation, instance layer/extension enumeration,
 * instance creation, and the Android surface path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "PanVKShim"

/* PANVK_SHIM_DIAG=1 (default): trace, PROF timers, stderr logging.
 * PANVK_SHIM_DIAG=0: clean build — no timing wrappers, no stderr, no trace;
 * correctness fixes (surface lifetime, instance reset) stay active. */
#ifndef PANVK_SHIM_DIAG
#define PANVK_SHIM_DIAG 1
#endif

static void shim_log(int prio, const char *lvl, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(prio, TAG, "%s", buf);
#if PANVK_SHIM_DIAG
    if (!getenv("PANVK_SHIM_QUIET")) {
        char line[1100];
        snprintf(line, sizeof(line), "[" TAG " %s] %s\n", lvl, buf);
        fputs(line, stderr);
    }
#else
    (void)lvl;
#endif
}

#define LOGI(...) shim_log(ANDROID_LOG_INFO,  "I", __VA_ARGS__)
#define LOGW(...) shim_log(ANDROID_LOG_WARN,  "W", __VA_ARGS__)
#define LOGE(...) shim_log(ANDROID_LOG_ERROR, "E", __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

#define ICD_FILE_NAME "libvulkan_panfrost.so"
#define ICD_ENV_PATH  "PANVK_ICD_PATH"
#define WANTED_ICD_INTERFACE_VERSION 5u

/* ---- Loader/ICD surface structures (mirrors Khronos vk_icd.h) ------------
 * VkIcdWsiPlatform order: MIR, WAYLAND, WIN32, XCB, XLIB, ANDROID, ...
 * so ANDROID == 5 (verified against Vulkan-Headers vk_icd.h). The ICD reads
 * VkIcdSurfaceBase.platform at offset 0 and the ANativeWindow* right after,
 * with normal struct alignment — identical layout to the real header. */
#define SHIM_ICD_WSI_PLATFORM_ANDROID 5

typedef struct {
    int platform; /* VkIcdWsiPlatform is an int-sized enum */
} ShimIcdSurfaceBase;

typedef struct {
    ShimIcdSurfaceBase base;
    ANativeWindow *window;
} ShimIcdSurfaceAndroid;

_Static_assert(sizeof(ShimIcdSurfaceBase) == sizeof(int),
               "surface base must be int-sized");
_Static_assert(offsetof(ShimIcdSurfaceAndroid, base) == 0,
               "surface base must sit at offset 0");

/* ---- State --------------------------------------------------------------- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_icd = NULL;
static PFN_vkGetInstanceProcAddr g_icd_gipa = NULL;
static PFN_vkVoidFunction (*g_icd_pdpa)(VkInstance, const char *) = NULL;
static uint32_t g_icd_version = 0;
static VkExtensionProperties *g_exts = NULL;
static uint32_t g_ext_count = 0;
static int g_icd_has_android_surface = 0;
static int g_trace = PANVK_SHIM_DIAG;
static int g_prof = PANVK_SHIM_DIAG;

/* Real ICD gdpa, resolved once a valid instance exists. Atomic: read/written
 * from multiple threads. */
static _Atomic(PFN_vkGetDeviceProcAddr) g_real_gdpa = NULL;
static _Atomic(VkInstance) g_last_instance = VK_NULL_HANDLE;

typedef VkResult (VKAPI_PTR *PFN_shim_negotiate)(uint32_t *);

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL +
           (unsigned long long)ts.tv_nsec;
}

__attribute__((unused)) static int flag_default_on(const char *name)
{
    const char *v = getenv(name);
    return !(v && v[0] == '0');
}

static void resolve_icd_path(char *out, size_t n)
{
    const char *env = getenv(ICD_ENV_PATH);
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return;
    }
    /* Default: same directory as the shim itself. */
    Dl_info info;
    if (dladdr((void *)&resolve_icd_path, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            snprintf(out, n, "%.*s/%s", (int)(slash - info.dli_fname),
                     info.dli_fname, ICD_FILE_NAME);
            return;
        }
    }
    snprintf(out, n, "%s", ICD_FILE_NAME);
}

static void build_extension_cache(void)
{
    PFN_vkEnumerateInstanceExtensionProperties fn =
        (PFN_vkEnumerateInstanceExtensionProperties)g_icd_gipa(
            NULL, "vkEnumerateInstanceExtensionProperties");
    if (!fn) {
        LOGE("ICD does not resolve vkEnumerateInstanceExtensionProperties");
        return;
    }
    uint32_t n = 0;
    if (fn(NULL, &n, NULL) != VK_SUCCESS) {
        LOGE("vkEnumerateInstanceExtensionProperties(count) failed");
        return;
    }
    g_exts = calloc((size_t)n + 1, sizeof(*g_exts));
    if (!g_exts)
        return;
    if (fn(NULL, &n, g_exts) < 0) {
        LOGE("vkEnumerateInstanceExtensionProperties(list) failed");
        free(g_exts);
        g_exts = NULL;
        return;
    }
    g_ext_count = n;
    int has_surface = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(g_exts[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME))
            has_surface = 1;
        if (!strcmp(g_exts[i].extensionName,
                    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
            g_icd_has_android_surface = 1;
    }
    /* Only advertise android_surface if the ICD understands VK_KHR_surface
     * (the vkGetPhysicalDeviceSurface* functions belong to it). */
    if (has_surface && !g_icd_has_android_surface) {
        VkExtensionProperties *e = &g_exts[g_ext_count++];
        snprintf(e->extensionName, sizeof(e->extensionName), "%s",
                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        e->specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
        LOGI("ICD lacks %s: advertised by the shim",
             VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
    LOGI("instance extensions: ICD=%u advertised=%u ICD-native android_surface=%d",
         n, g_ext_count, g_icd_has_android_surface);
}

static void load_once(void)
{
#if PANVK_SHIM_DIAG
    g_trace = flag_default_on("PANVK_SHIM_TRACE");
    g_prof = flag_default_on("PANVK_SHIM_PROF");
#else
    g_trace = 0;
    g_prof = 0;
#endif
    char path[PATH_MAX];
    resolve_icd_path(path, sizeof(path));
    g_icd = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_icd) {
        LOGE("dlopen(%s) failed: %s", path, dlerror());
        return;
    }
    g_icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_icd,
                                                  "vk_icdGetInstanceProcAddr");
    if (!g_icd_gipa) {
        LOGE("vk_icdGetInstanceProcAddr not found in %s: %s", path, dlerror());
        dlclose(g_icd);
        g_icd = NULL;
        return;
    }

    /* Interface negotiation. v7 ICDs may expose this only through gipa. */
    PFN_shim_negotiate neg = (PFN_shim_negotiate)dlsym(
        g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!neg)
        neg = (PFN_shim_negotiate)g_icd_gipa(
            NULL, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) {
        uint32_t v = WANTED_ICD_INTERFACE_VERSION;
        VkResult r = neg(&v);
        if (r == VK_SUCCESS)
            g_icd_version = v;
        else
            LOGW("ICD interface negotiation failed (VkResult=%d)", (int)r);
    } else {
        LOGW("ICD does not expose vk_icdNegotiateLoaderICDInterfaceVersion");
    }

    /* Physical-device proc fallback (ICD interface v4+). */
    g_icd_pdpa = (PFN_vkVoidFunction (*)(VkInstance, const char *))dlsym(
        g_icd, "vk_icdGetPhysicalDeviceProcAddr");
    if (!g_icd_pdpa)
        g_icd_pdpa = (PFN_vkVoidFunction (*)(VkInstance, const char *))
            g_icd_gipa(NULL, "vk_icdGetPhysicalDeviceProcAddr");

    LOGI("ICD loaded: %s (negotiated interface v%u, gipa=%p) build=%s trace=%d prof=%d",
         path, g_icd_version, (void *)g_icd_gipa,
         PANVK_SHIM_DIAG ? "diag" : "clean", g_trace, g_prof);
    build_extension_cache();
}

/* ---- TRACE: each requested name, logged once ----------------------------- */
#define TRACE_MAX 1024
static char *g_seen[TRACE_MAX];
static unsigned g_seen_n = 0;
static pthread_mutex_t g_seen_mu = PTHREAD_MUTEX_INITIALIZER;

static int first_time(const char *kind, const char *name)
{
    char key[160];
    snprintf(key, sizeof(key), "%s:%s", kind, name);
    int res = 0;
    pthread_mutex_lock(&g_seen_mu);
    unsigned i;
    for (i = 0; i < g_seen_n; i++)
        if (!strcmp(g_seen[i], key))
            break;
    if (i == g_seen_n && g_seen_n < TRACE_MAX) {
        g_seen[g_seen_n++] = strdup(key);
        res = 1;
    }
    pthread_mutex_unlock(&g_seen_mu);
    return res;
}

static PFN_vkVoidFunction traced(const char *kind, const void *handle,
                                 const char *name, PFN_vkVoidFunction r)
{
    if (g_trace && first_time(kind, name))
        LOGI("TRACE %s(%s%s) -> %s", kind, handle ? "" : "NULL,", name,
             r ? "ok" : "NULL");
    return r;
}

/* ---- PROF: per-call statistics ------------------------------------------- */
enum {
    ST_SUBMIT, ST_PRESENT, ST_ACQUIRE, ST_WAITFENCE, ST_WAITSEM,
    ST_QWAIT, ST_DWAIT, ST_COUNT
};
static const char *const st_name[ST_COUNT] = {
    "submit", "present", "acquire", "waitFences", "waitSem",
    "queueWaitIdle", "deviceWaitIdle"
};

typedef struct { atomic_ullong n, tot, max; } Stat;
static Stat g_st[ST_COUNT];
static atomic_ullong g_frames, g_frame_tot, g_frame_max;
static atomic_ullong g_last_report, g_last_present;

/* Frame-time histogram: 1ms buckets, last bucket = overflow (>=63ms). */
#define FT_BUCKETS 64
static atomic_ullong g_ft_hist[FT_BUCKETS];
static atomic_ullong g_stutters;      /* frames slower than 33ms */
static atomic_ullong g_acq_timeout, g_acq_notready, g_acq_subopt, g_acq_err;
static atomic_ullong g_present_subopt;

static void atomic_max_ull(atomic_ullong *a, unsigned long long v)
{
    unsigned long long cur = atomic_load_explicit(a, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(
               a, &cur, v, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void record(int idx, unsigned long long dt)
{
    atomic_fetch_add_explicit(&g_st[idx].n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_st[idx].tot, dt, memory_order_relaxed);
    atomic_max_ull(&g_st[idx].max, dt);
}

static void note_acquire_result(VkResult r)
{
    switch (r) {
    case VK_TIMEOUT:       atomic_fetch_add(&g_acq_timeout, 1);  break;
    case VK_NOT_READY:     atomic_fetch_add(&g_acq_notready, 1); break;
    case VK_SUBOPTIMAL_KHR:atomic_fetch_add(&g_acq_subopt, 1);   break;
    default: if (r < 0)    atomic_fetch_add(&g_acq_err, 1);      break;
    }
}

static void maybe_report(unsigned long long now)
{
    unsigned long long last = atomic_load(&g_last_report);
    if (last == 0) {
        atomic_compare_exchange_strong(&g_last_report, &last, now);
        return;
    }
    if (now - last < 1000000000ULL)
        return;
    if (!atomic_compare_exchange_strong(&g_last_report, &last, now))
        return;

    double elapsed_ms = (double)(now - last) / 1e6;
    unsigned long long frames = atomic_exchange(&g_frames, 0);
    unsigned long long ftot = atomic_exchange(&g_frame_tot, 0);
    unsigned long long fmax = atomic_exchange(&g_frame_max, 0);
    unsigned long long stut = atomic_exchange(&g_stutters, 0);

    unsigned long long hist[FT_BUCKETS];
    for (int b = 0; b < FT_BUCKETS; b++)
        hist[b] = atomic_exchange(&g_ft_hist[b], 0);

    int p50 = 0, p99 = 0;
    double low_fps = 0.0;
    if (frames) {
        unsigned long long cum = 0;
        for (int b = 0; b < FT_BUCKETS; b++) {
            cum += hist[b];
            if (!p50 && cum * 2 >= frames) p50 = b + 1;
            if (!p99 && cum * 100 >= frames * 99) { p99 = b + 1; break; }
        }
        /* Approx 1% low fps: mean of the worst 1% of frames (bucket centers). */
        unsigned long long k = frames / 100; if (!k) k = 1;
        unsigned long long acc = 0; double sum_ms = 0.0;
        for (int b = FT_BUCKETS - 1; b >= 0 && acc < k; b--) {
            unsigned long long take = hist[b];
            if (take > k - acc) take = k - acc;
            if (take) { sum_ms += (b + 0.5) * (double)take; acc += take; }
        }
        if (acc && sum_ms > 0.0) low_fps = 1000.0 * (double)acc / sum_ms;
    }

    char buf[1200];
    int o = snprintf(buf, sizeof(buf),
                     "PROF %.0fms frames=%llu fps=%.1f frame avg=%.1fms max=%.1fms "
                     "p50~%dms p99~%dms 1%%low=%.1ffps stut=%llu",
                     elapsed_ms, frames,
                     elapsed_ms > 0 ? frames * 1000.0 / elapsed_ms : 0.0,
                     frames ? ftot / 1e6 / (double)frames : 0.0, fmax / 1e6,
                     p50, p99, low_fps, stut);

    unsigned long long ato = atomic_exchange(&g_acq_timeout, 0);
    unsigned long long anr = atomic_exchange(&g_acq_notready, 0);
    unsigned long long asu = atomic_exchange(&g_acq_subopt, 0);
    unsigned long long aer = atomic_exchange(&g_acq_err, 0);
    unsigned long long psu = atomic_exchange(&g_present_subopt, 0);
    if ((ato || anr || asu || aer || psu) && o > 0 && (size_t)o < sizeof(buf))
        o += snprintf(buf + o, sizeof(buf) - (size_t)o,
                      " | acq(to=%llu,nr=%llu,sub=%llu,err=%llu) psub=%llu",
                      ato, anr, asu, aer, psu);

    for (int i = 0; i < ST_COUNT && o > 0 && (size_t)o < sizeof(buf); i++) {
        unsigned long long n = atomic_exchange(&g_st[i].n, 0);
        unsigned long long tot = atomic_exchange(&g_st[i].tot, 0);
        unsigned long long mx = atomic_exchange(&g_st[i].max, 0);
        if (!n)
            continue;
        o += snprintf(buf + o, sizeof(buf) - (size_t)o,
                      " | %s n=%llu avg=%.2fms max=%.1fms sum=%.0fms",
                      st_name[i], n, tot / 1e6 / (double)n, mx / 1e6, tot / 1e6);
    }
    LOGI("%s", buf);
}

#define TIMED(NAME, ST, RET, PARAMS, ARGS)                                     \
    static PFN_vk##NAME real_##NAME;                                           \
    static VKAPI_ATTR RET VKAPI_CALL timed_##NAME PARAMS                       \
    {                                                                          \
        unsigned long long t0 = now_ns();                                      \
        RET r = real_##NAME ARGS;                                              \
        record(ST, now_ns() - t0);                                             \
        return r;                                                              \
    }

TIMED(QueueSubmit, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f), (q, n, i, f))
TIMED(QueueSubmit2, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f), (q, n, i, f))
TIMED(QueueSubmit2KHR, ST_SUBMIT, VkResult,
      (VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f), (q, n, i, f))
TIMED(WaitForFences, ST_WAITFENCE, VkResult,
      (VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t t),
      (d, n, f, all, t))
TIMED(WaitSemaphores, ST_WAITSEM, VkResult,
      (VkDevice d, const VkSemaphoreWaitInfo *info, uint64_t t), (d, info, t))
TIMED(WaitSemaphoresKHR, ST_WAITSEM, VkResult,
      (VkDevice d, const VkSemaphoreWaitInfo *info, uint64_t t), (d, info, t))
TIMED(QueueWaitIdle, ST_QWAIT, VkResult, (VkQueue q), (q))
TIMED(DeviceWaitIdle, ST_DWAIT, VkResult, (VkDevice d), (d))

static PFN_vkAcquireNextImageKHR real_AcquireNextImageKHR;
static VKAPI_ATTR VkResult VKAPI_CALL
timed_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t,
                         VkSemaphore sem, VkFence f, uint32_t *idx)
{
    unsigned long long t0 = now_ns();
    VkResult r = real_AcquireNextImageKHR(d, s, t, sem, f, idx);
    record(ST_ACQUIRE, now_ns() - t0);
    note_acquire_result(r);
    return r;
}

static PFN_vkAcquireNextImage2KHR real_AcquireNextImage2KHR;
static VKAPI_ATTR VkResult VKAPI_CALL
timed_AcquireNextImage2KHR(VkDevice d, const VkAcquireNextImageInfoKHR *info,
                          uint32_t *idx)
{
    unsigned long long t0 = now_ns();
    VkResult r = real_AcquireNextImage2KHR(d, info, idx);
    record(ST_ACQUIRE, now_ns() - t0);
    note_acquire_result(r);
    return r;
}

static PFN_vkQueuePresentKHR real_QueuePresentKHR;
static atomic_uint g_present_errs;
static VKAPI_ATTR VkResult VKAPI_CALL
timed_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info)
{
    unsigned long long t0 = now_ns();
    VkResult r = real_QueuePresentKHR(q, info);
    unsigned long long t1 = now_ns();
    record(ST_PRESENT, t1 - t0);

    unsigned long long prev = atomic_exchange(&g_last_present, t1);
    if (prev) {
        unsigned long long dt = t1 - prev;
        atomic_fetch_add(&g_frame_tot, dt);
        atomic_max_ull(&g_frame_max, dt);
        unsigned long long ms = dt / 1000000ULL;
        int b = ms >= FT_BUCKETS ? FT_BUCKETS - 1 : (int)ms;
        atomic_fetch_add(&g_ft_hist[b], 1);
        if (dt > 33000000ULL)
            atomic_fetch_add(&g_stutters, 1);
    }
    atomic_fetch_add(&g_frames, 1);
    if (r == VK_SUBOPTIMAL_KHR)
        atomic_fetch_add(&g_present_subopt, 1);
    else if (r != VK_SUCCESS && atomic_fetch_add(&g_present_errs, 1) < 5)
        LOGW("vkQueuePresentKHR -> VkResult=%d", (int)r);
    maybe_report(t1);
    return r;
}

/* ---- Result-logging wrappers (diag, always installed) -------------------- */
static void join_names(char *out, size_t n, uint32_t count,
                       const char *const *names)
{
    size_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) {
        int w = snprintf(out + o, n - o, "%s%s", i ? "," : "", names[i]);
        if (w < 0 || (size_t)w >= n - o) {
            snprintf(out + n - 4, 4, "...");
            break;
        }
        o += (size_t)w;
    }
}

static PFN_vkEnumeratePhysicalDevices real_EnumeratePhysicalDevices;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_EnumeratePhysicalDevices(VkInstance inst, uint32_t *pCount,
                                VkPhysicalDevice *pDevs)
{
    VkResult r = real_EnumeratePhysicalDevices(inst, pCount, pDevs);
    LOGI("vkEnumeratePhysicalDevices(%s) -> VkResult=%d count=%u",
         pDevs ? "list" : "count", (int)r, pCount ? *pCount : 0);
    return r;
}

static PFN_vkCreateDevice real_CreateDevice;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *pInfo,
                    const VkAllocationCallbacks *pAlloc, VkDevice *pDev)
{
    VkResult r = real_CreateDevice(pd, pInfo, pAlloc, pDev);
    char exts[700];
    join_names(exts, sizeof(exts), pInfo ? pInfo->enabledExtensionCount : 0,
               pInfo ? pInfo->ppEnabledExtensionNames : NULL);
    LOGI("vkCreateDevice -> VkResult=%d extensions(%u)=[%s]", (int)r,
         pInfo ? pInfo->enabledExtensionCount : 0, exts);
    return r;
}

static PFN_vkCreateSwapchainKHR real_CreateSwapchainKHR;
static VKAPI_ATTR VkResult VKAPI_CALL
logged_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *pInfo,
                          const VkAllocationCallbacks *pAlloc,
                          VkSwapchainKHR *pSc)
{
    VkResult r = real_CreateSwapchainKHR(d, pInfo, pAlloc, pSc);
    if (pInfo)
        LOGI("vkCreateSwapchainKHR -> VkResult=%d images=%u format=%d "
             "extent=%ux%u presentMode=%d usage=0x%x",
             (int)r, pInfo->minImageCount, (int)pInfo->imageFormat,
             pInfo->imageExtent.width, pInfo->imageExtent.height,
             (int)pInfo->presentMode, (unsigned)pInfo->imageUsage);
    else
        LOGI("vkCreateSwapchainKHR -> VkResult=%d (no info)", (int)r);
    return r;
}

static PFN_vkGetPhysicalDeviceProperties real_GetPhysicalDeviceProperties;
static atomic_int g_props_logged;
static VKAPI_ATTR void VKAPI_CALL
logged_GetPhysicalDeviceProperties(VkPhysicalDevice pd,
                                   VkPhysicalDeviceProperties *pProps)
{
    real_GetPhysicalDeviceProperties(pd, pProps);
    if (pProps && atomic_exchange(&g_props_logged, 1) == 0) {
        uint32_t dv = pProps->driverVersion;
        LOGI("ICD device: name=%s api=%u.%u.%u driverVersion=0x%08x (%u.%u.%u) "
             "vendorID=0x%x",
             pProps->deviceName,
             VK_API_VERSION_MAJOR(pProps->apiVersion),
             VK_API_VERSION_MINOR(pProps->apiVersion),
             VK_API_VERSION_PATCH(pProps->apiVersion),
             dv, VK_API_VERSION_MAJOR(dv), VK_API_VERSION_MINOR(dv),
             VK_API_VERSION_PATCH(dv), pProps->vendorID);
    }
}

/* ---- Correctness wrapper: installed in EVERY build ----------------------- */
static PFN_vkDestroyInstance real_DestroyInstance;

/* ---- Surface registry (shim-created surfaces only) ----------------------- */
#define SURF_MAX 32
typedef struct {
    VkSurfaceKHR handle;
    VkInstance instance;
    ANativeWindow *window;
} ShimSurface;
static ShimSurface g_surfaces[SURF_MAX];
static pthread_mutex_t g_surf_mu = PTHREAD_MUTEX_INITIALIZER;

static void surfaces_purge_instance(VkInstance instance)
{
    pthread_mutex_lock(&g_surf_mu);
    for (int i = 0; i < SURF_MAX; i++) {
        if (g_surfaces[i].handle != VK_NULL_HANDLE &&
            g_surfaces[i].instance == instance) {
            if (g_surfaces[i].window)
                ANativeWindow_release(g_surfaces[i].window);
            free((void *)(uintptr_t)g_surfaces[i].handle);
            g_surfaces[i].handle = VK_NULL_HANDLE;
            g_surfaces[i].instance = VK_NULL_HANDLE;
            g_surfaces[i].window = NULL;
        }
    }
    pthread_mutex_unlock(&g_surf_mu);
}

static VKAPI_ATTR void VKAPI_CALL
wrapped_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAlloc)
{
    if (instance != VK_NULL_HANDLE) {
        surfaces_purge_instance(instance);
        VkInstance expected = instance;
        if (atomic_compare_exchange_strong(&g_last_instance, &expected,
                                           VK_NULL_HANDLE))
            atomic_store(&g_real_gdpa, NULL);
    }
    if (real_DestroyInstance)
        real_DestroyInstance(instance, pAlloc);
}

/* ---- Wrapper table -------------------------------------------------------- */
typedef struct {
    const char *name;
    void *real;
    PFN_vkVoidFunction wrapper;
    int always;      /* diag: install even when PROF is off */
    int correctness; /* install in every build, ignores diag gate */
} WrapEnt;

#define W(n, always_) \
    {"vk" #n, (void **)&real_##n, (PFN_vkVoidFunction)timed_##n, always_, 0}
#define WL(n) \
    {"vk" #n, (void **)&real_##n, (PFN_vkVoidFunction)logged_##n, 1, 0}
#define WC(n) \
    {"vk" #n, (void **)&real_##n, (PFN_vkVoidFunction)wrapped_##n, 1, 1}

static const WrapEnt g_wraps[] = {
    W(QueueSubmit, 0),         W(QueueSubmit2, 0),
    W(QueueSubmit2KHR, 0),     W(QueuePresentKHR, 0),
    W(AcquireNextImageKHR, 0), W(AcquireNextImage2KHR, 0),
    W(WaitForFences, 0),       W(WaitSemaphores, 0),
    W(WaitSemaphoresKHR, 0),   W(QueueWaitIdle, 0),
    W(DeviceWaitIdle, 0),
    WL(EnumeratePhysicalDevices), WL(CreateDevice),
    WL(CreateSwapchainKHR),       WL(GetPhysicalDeviceProperties),
    WC(DestroyInstance),
};

static PFN_vkVoidFunction maybe_wrap(const char *name, PFN_vkVoidFunction real)
{
    if (!real)
        return real;
    for (size_t i = 0; i < sizeof(g_wraps) / sizeof(g_wraps[0]); i++) {
        if (strcmp(g_wraps[i].name, name))
            continue;
        if (!g_wraps[i].correctness) {
            if (!PANVK_SHIM_DIAG)
                return real;
            if (!g_wraps[i].always && !g_prof)
                return real;
        }
        *(void **)g_wraps[i].real = (void *)real;
        return g_wraps[i].wrapper;
    }
    return real;
}

/* ---- Functions the shim implements instead of the loader ----------------- */
static VKAPI_ATTR VkResult VKAPI_CALL
shim_EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProps)
{
    (void)pProps;
    if (!pCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                          uint32_t *pCount,
                                          VkExtensionProperties *pProps)
{
    if (pLayerName)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pCount || !g_exts)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProps) {
        *pCount = g_ext_count;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < g_ext_count ? *pCount : g_ext_count;
    memcpy(pProps, g_exts, (size_t)n * sizeof(*g_exts));
    *pCount = n;
    return n < g_ext_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance)
{
    PFN_vkCreateInstance real =
        (PFN_vkCreateInstance)g_icd_gipa(NULL, "vkCreateInstance");
    if (!real)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r;
    if (g_icd_has_android_surface || !pCreateInfo ||
        pCreateInfo->enabledExtensionCount == 0) {
        r = real(pCreateInfo, pAllocator, pInstance);
    } else {
        /* ICD does not know VK_KHR_android_surface: strip it. */
        const char **names =
            malloc((size_t)pCreateInfo->enabledExtensionCount * sizeof(char *));
        if (!names)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t n = 0;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            const char *name = pCreateInfo->ppEnabledExtensionNames[i];
            if (strcmp(name, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) != 0)
                names[n++] = name;
        }
        VkInstanceCreateInfo copy = *pCreateInfo;
        copy.enabledExtensionCount = n;
        copy.ppEnabledExtensionNames = names;
        r = real(&copy, pAllocator, pInstance);
        free(names);
    }
    if (r == VK_SUCCESS && pInstance)
        atomic_store(&g_last_instance, *pInstance);

    if (PANVK_SHIM_DIAG && pCreateInfo) {
        char exts[700];
        join_names(exts, sizeof(exts), pCreateInfo->enabledExtensionCount,
                   pCreateInfo->ppEnabledExtensionNames);
        const VkApplicationInfo *ai = pCreateInfo->pApplicationInfo;
        LOGI("vkCreateInstance -> VkResult=%d app=%s apiVersion=%u.%u.%u "
             "extensions(%u)=[%s]",
             (int)r, ai && ai->pApplicationName ? ai->pApplicationName : "(null)",
             ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0,
             ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0,
             ai ? VK_API_VERSION_PATCH(ai->apiVersion) : 0,
             pCreateInfo->enabledExtensionCount, exts);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
shim_CreateAndroidSurfaceKHR(VkInstance instance,
                             const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkSurfaceKHR *pSurface)
{
    (void)pAllocator;
    if (!pCreateInfo || !pCreateInfo->window || !pSurface) {
        LOGE("vkCreateAndroidSurfaceKHR: invalid arguments");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    ShimIcdSurfaceAndroid *s = calloc(1, sizeof(*s));
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = SHIM_ICD_WSI_PLATFORM_ANDROID;
    s->window = pCreateInfo->window;
    /* Hold a reference for the surface lifetime (the ICD's own surface path
     * would do this; we bypass it, so the shim must). */
    ANativeWindow_acquire(s->window);

    pthread_mutex_lock(&g_surf_mu);
    int slot = -1;
    for (int i = 0; i < SURF_MAX; i++)
        if (g_surfaces[i].handle == VK_NULL_HANDLE) { slot = i; break; }
    if (slot >= 0) {
        g_surfaces[slot].handle = (VkSurfaceKHR)(uintptr_t)s;
        g_surfaces[slot].instance = instance;
        g_surfaces[slot].window = s->window;
    }
    pthread_mutex_unlock(&g_surf_mu);

    if (slot < 0) {
        ANativeWindow_release(s->window);
        free(s);
        LOGE("vkCreateAndroidSurfaceKHR: surface registry full");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    if (PANVK_SHIM_DIAG)
        LOGI("vkCreateAndroidSurfaceKHR -> ok window=%p", (void *)s->window);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
shim_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                       const VkAllocationCallbacks *pAllocator)
{
    if (surface == VK_NULL_HANDLE)
        return;
    ANativeWindow *window = NULL;
    int found = 0;
    pthread_mutex_lock(&g_surf_mu);
    for (int i = 0; i < SURF_MAX; i++) {
        if (g_surfaces[i].handle == surface) {
            window = g_surfaces[i].window;
            g_surfaces[i].handle = VK_NULL_HANDLE;
            g_surfaces[i].instance = VK_NULL_HANDLE;
            g_surfaces[i].window = NULL;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_surf_mu);

    if (found) {
        if (window)
            ANativeWindow_release(window);
        free((void *)(uintptr_t)surface);
        return;
    }
    /* Not shim-created: forward to the ICD if it can destroy surfaces. */
    PFN_vkDestroySurfaceKHR real =
        (PFN_vkDestroySurfaceKHR)g_icd_gipa(instance, "vkDestroySurfaceKHR");
    if (real)
        real(instance, surface, pAllocator);
    else
        LOGW("vkDestroySurfaceKHR: foreign surface %p, ICD has no destroyer",
             (void *)(uintptr_t)surface);
}

/* ---- Exported symbols ----------------------------------------------------- */
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    pthread_once(&g_once, load_once);
    if (!g_icd_gipa || !pName)
        return NULL;
    if (instance != VK_NULL_HANDLE &&
        atomic_load(&g_last_instance) == VK_NULL_HANDLE)
        atomic_store(&g_last_instance, instance);

#define MAP(name, fn)                                                          \
    do {                                                                       \
        if (!strcmp(pName, name))                                              \
            return traced("gipa", instance, name, (PFN_vkVoidFunction)(fn));   \
    } while (0)

    MAP("vkGetInstanceProcAddr", vkGetInstanceProcAddr);
    MAP("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
    MAP("vkEnumerateInstanceLayerProperties",
        shim_EnumerateInstanceLayerProperties);
    MAP("vkEnumerateInstanceExtensionProperties",
        shim_EnumerateInstanceExtensionProperties);
    MAP("vkCreateInstance", shim_CreateInstance);
    if (!g_icd_has_android_surface) {
        MAP("vkCreateAndroidSurfaceKHR", shim_CreateAndroidSurfaceKHR);
        MAP("vkDestroySurfaceKHR", shim_DestroySurfaceKHR);
    }
#undef MAP

    PFN_vkVoidFunction r = g_icd_gipa(instance, pName);
    /* v4+ ICDs route unknown instance-level extension functions here. */
    if (!r && instance != VK_NULL_HANDLE && g_icd_pdpa)
        r = g_icd_pdpa(instance, pName);
    r = maybe_wrap(pName, r);
    return traced("gipa", instance, pName, r);
}

/* Zink (upstream Mesa) dlsym's vkGetDeviceProcAddr on the same handle and
 * aborts with "ZINK: failed to get proc address" if it is missing. The ICD
 * does not export it; resolve it through a valid instance. */
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    pthread_once(&g_once, load_once);
    if (!g_icd_gipa || !pName)
        return NULL;
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return traced("gdpa", device, pName,
                      (PFN_vkVoidFunction)vkGetDeviceProcAddr);

    PFN_vkGetDeviceProcAddr real = atomic_load(&g_real_gdpa);
    if (!real) {
        VkInstance inst = atomic_load(&g_last_instance);
        if (inst != VK_NULL_HANDLE) {
            real = (PFN_vkGetDeviceProcAddr)g_icd_gipa(inst,
                                                       "vkGetDeviceProcAddr");
            if (real)
                atomic_store(&g_real_gdpa, real);
        }
    }
    if (!real) {
        if (g_trace && first_time("gdpa-noreal", pName))
            LOGW("vkGetDeviceProcAddr(%s): ICD has no instance yet", pName);
        return NULL;
    }
    PFN_vkVoidFunction r = real(device, pName);
    r = maybe_wrap(pName, r);
    return traced("gdpa", device, pName, r);
}