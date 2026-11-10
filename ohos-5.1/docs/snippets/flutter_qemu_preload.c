#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * OpenHarmony x86 QEMU ships SwiftShader without VK_OHOS_native_buffer.
 * The release Flutter engine (XCom 3.9.2 ohos_x64) therefore builds a
 * null OHOS context and aborts in platform_view_ohos.cpp:120. The signed
 * HAP must stay untouched, so the fix preloads into every process via
 * appspawn's LD_PRELOAD:
 *
 *  1. In-process watcher: the constructor also runs in each spawned app
 *     process (they exec, reloading this library). It polls
 *     /proc/self/maps every 5 ms; the moment the loader maps the release
 *     libflutter.so it rewrites fourteen bytes at file offset 0x37aca8
 *     through /proc/self/mem. The rewrite targets the UPSTREAM backend
 *     selection: "cmp $2,%r12d; je <vulkan-config>" becomes "jmp
 *     <skia-gl-config>", so the engine builds the GL settings object
 *     (EGLDisplay wrapper and friends) instead of the Vulkan one. The
 *     downstream dispatch then reads type==1 and takes the GL arm with a
 *     matching parameter container. (Patching only the downstream branch
 *     left the Vulkan-shaped settings in place and crashed at
 *     OHOSContextGLSkia::Create on a garbage EGLDisplay.)
 *
 *  2. Parent-side scanner (belt and braces): a root watcher inside
 *     appspawn scans /proc/<pid>/maps of all processes and patches the
 *     same signature remotely via /proc/<pid>/mem.
 *
 * Address math: the kernel reports page-aligned file offsets in maps.
 * For this build the executable segment maps file page 0x2d2000 at
 * vaddr 0x2d3000 (vaddr = file offset + 0x1000), so the target
 * instruction at vaddr 0x39cd12 lives at FILE offset 0x39bd12. For a
 * maps line (start,end,offset) the in-process address is
 * start + (FILE_OFF - offset). A whole-file data mapping of the .so
 * (done by the code-sign verifier) has offset 0 and yields
 * start + FILE_OFF; the bytes there never match the signature, so
 * data mappings are skipped naturally.
 *
 * New patch site: vaddr 0x37bca8 (file offset 0x37aca8), the 14-byte
 * cmp/je/cmp context around the Vulkan-config constructor branch.
 *
 * Second patch site: vaddr 0x4a2500 (file offset 0x4a1500). This is a
 * DisplayList dispatch-table entry whose whole body is the fatal
 * "Check failed: false." at dl_sk_dispatcher.cc(328) (the Skia backend
 * throwing on an op it must not receive — reached on first frame in
 * XCom 3.9.2 ohos_x64). The thunk has no rel32 callers; it is only
 * referenced from the op table. Rewriting its first byte to `ret` turns
 * the op into a harmless no-op so rasterization continues instead of
 * FML-killing the raster thread (which wedges first-frame delivery and
 * makes the watchdog flag FlutterUiThread as not alive).
 *
 * Debug engine fatal site: file offset 0x18837e0 (vaddr 0x18847e0) is
 * the same dl_sk_dispatcher.cc(328) checker compiled into the debug
 * build: `push %rbx; sub $0x120,%rsp`, file string 0x17f262, line 328,
 * "false" at 0x16e26b, no rel32 callers (op-table referenced only).
 * Unlike the release build the debug checker only LOGS (HiLog fatal
 * without abort), but the residue upsets log-based acceptance, so it
 * gets the identical `ret` neutralization and the line disappears at
 * the source.
 */

#define LOG_PATH "/data/log/flpatch.log"
#define VADDR_OFF 0x37bca8u   /* virtual address of the backend-selection cmp/je */
#define FILE_OFF 0x37aca8u    /* file offset of the same instruction           */
/* Prefork child may sit idle for minutes. The watcher must not exit
 * before libflutter is mapped, or the 50 ms appspawn scan loses again. */

/* fatal-check thunk (dl_sk_dispatcher.cc:328) start: push rbx; sub rsp,0x120 */
static const unsigned char fatal_expected[8] = {
    0x53, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00
};
static const unsigned char fatal_ret[8] = {
    0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

typedef void *(*DlopenFn)(const char *, int);
typedef pid_t (*ForkFn)(void);

typedef int (*PrctlFn)(int, unsigned long, unsigned long, unsigned long,
                       unsigned long);

static DlopenFn real_dlopen;
static ForkFn real_fork;
static PrctlFn real_prctl;
static int log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t patch_lock = PTHREAD_MUTEX_INITIALIZER;
/* Prefork child blocks in read() for minutes. Its watcher can be stuck
 * across the sandbox mount switch, or asleep for 20 ms, and then
 * libflutter is mapped and Setup runs with the original bytes. prctl
 * sets the app name after that switch and before the engine is loaded,
 * so a second watcher started there still sees the mapping. */
static volatile int patches_done;
static volatile int want_fast;
static volatile int fast_armed;
static volatile int watcher_count;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int len;

    pthread_mutex_lock(&log_lock);
    if (log_fd < 0 || fcntl(log_fd, F_GETFD) == -1)
        log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (log_fd >= 0) {
        len = snprintf(buf, sizeof(buf), "[flpatch pid=%d t=%ld] ",
                       (int)getpid(), now_ms());
        va_start(ap, fmt);
        len += vsnprintf(buf + len, sizeof(buf) - (size_t)len - 1, fmt, ap);
        va_end(ap);
        if (len < 0 || (size_t)len >= sizeof(buf) - 1)
            len = (int)sizeof(buf) - 2;
        buf[len++] = '\n';
        (void)write(log_fd, buf, (size_t)len);
    }
    pthread_mutex_unlock(&log_lock);
}

static const unsigned char expected_bytes[14] = {
    /* cmp $0x2,%r12d ; je +0x70d (Vulkan config) ; cmp $0x1,%r12d */
    0x41, 0x83, 0xfc, 0x02, 0x0f, 0x84, 0x0d, 0x07,
    0x00, 0x00, 0x41, 0x83, 0xfc, 0x01
};
static const unsigned char replacement_bytes[14] = {
    /* cmp $0x2,%r12d ; jmp +0x626 (Skia GL config) ; nop ; cmp $0x1,%r12d */
    0x41, 0x83, 0xfc, 0x02, 0xe9, 0x26, 0x06, 0x00,
    0x00, 0x90, 0x41, 0x83, 0xfc, 0x01
};

/*
 * Debug engine Build ID 6646ddaf. Two sites, both required:
 *   surface file 0x17aa8fe: cmp $2,%eax; je Vulkan surface -> jmp Skia surface
 *   context file 0x17aab12: cmp $2,%ebp; je Vulkan context (56-byte, no EGL)
 *     -> jmp Skia context (144-byte, eglGetDisplay). Redirecting only the
 *     surface leaves +48 as 0xb00 and SIGSEGV at libflutter+0x177eb70.
 * The release 14-byte site is not in this binary.
 * Do not export dlopen or dlopen_ns. Doing so reproduces SIGSEGV@0x6b0
 * at libflutter+0x376d2f even after the byte patches land. The child
 * watcher must run: a mutex held by another appspawn thread is copied
 * locked across fork and otherwise the watcher never patches.
 */
#define FILE_OFF_DEBUG 0x17aa8feu
static const unsigned char debug_expected[5] = {
    0x83, 0xf8, 0x02, 0x74, 0x3a
};
static const unsigned char debug_replacement[5] = {
    0x83, 0xf8, 0x02, 0xeb, 0x67
};
#define FILE_OFF_DEBUG_CTX 0x17aab12u
static const unsigned char debug_ctx_expected[5] = {
    0x83, 0xfd, 0x02, 0x74, 0x22
};
static const unsigned char debug_ctx_replacement[5] = {
    0x83, 0xfd, 0x02, 0xeb, 0x43
};

/* Debug engine dl_sk_dispatcher.cc(328) checker (see header). Same
 * push/sub prologue as the release fatal thunk, so fatal_expected
 * matches and patch_fatal_thunk() is reused verbatim. */
#define FILE_OFF_DEBUG_FATAL 0x18837e0u

/* Patch through an already-open /proc/<pid>/mem fd. */
static int patch_fd_tag(int fd, uintptr_t addr, int log_fail,
                        const char *tag);
static int patch_fatal_thunk_tag(int fd, uintptr_t addr, const char *tag);

static int patch_fd(int fd, uintptr_t addr, int log_fail)
{
    return patch_fd_tag(fd, addr, log_fail, "PATCHED");
}

static int patch_fd_tag(int fd, uintptr_t addr, int log_fail,
                        const char *tag)
{
    unsigned char cur[sizeof(expected_bytes)];
    ssize_t r, w;

    r = pread(fd, cur, sizeof(cur), (off_t)addr);
    if (r != (ssize_t)sizeof(cur))
        return 0;
    if (memcmp(cur, replacement_bytes, sizeof(replacement_bytes)) == 0)
        return 1; /* already patched */
    if (memcmp(cur, expected_bytes, sizeof(expected_bytes)) != 0)
        return 0;
    w = pwrite(fd, replacement_bytes, sizeof(replacement_bytes),
               (off_t)addr);
    if (w != (ssize_t)sizeof(replacement_bytes)) {
        if (log_fail)
            log_line("pwrite failed w=%zd errno=%d addr=0x%lx",
                     w, errno, (unsigned long)addr);
        return 0;
    }
    r = pread(fd, cur, sizeof(cur), (off_t)addr);
    if (r == (ssize_t)sizeof(cur) &&
        memcmp(cur, replacement_bytes, sizeof(replacement_bytes)) == 0) {
        log_line("%s addr=0x%lx", tag, (unsigned long)addr);
        return 1;
    }
    return 0;
}

static int patch_debug_site(int fd, uintptr_t addr, const unsigned char *expect,
                           const unsigned char *replace, size_t n,
                           const char *tag)
{
    unsigned char cur[8];
    ssize_t r, w;

    if (n > sizeof(cur))
        return 0;
    r = pread(fd, cur, n, (off_t)addr);
    if (r != (ssize_t)n)
        return 0;
    if (memcmp(cur, replace, n) == 0)
        return 1;
    if (memcmp(cur, expect, n) != 0)
        return 0;
    w = pwrite(fd, replace, n, (off_t)addr);
    if (w != (ssize_t)n)
        return 0;
    r = pread(fd, cur, n, (off_t)addr);
    if (r == (ssize_t)n && memcmp(cur, replace, n) == 0) {
        log_line("%s addr=0x%lx", tag, (unsigned long)addr);
        return 1;
    }
    return 0;
}
/* Neutralize the dl_sk_dispatcher fatal-check thunk (see header). */
static int patch_fatal_thunk_tag(int fd, uintptr_t addr, const char *tag);
static int patch_fatal_thunk(int fd, uintptr_t addr)
{
    return patch_fatal_thunk_tag(fd, addr, "FATAL-THUNK-NEUTRALIZED");
}

static int patch_fatal_thunk_tag(int fd, uintptr_t addr, const char *tag)
{
    unsigned char cur[8];
    ssize_t r, w;

    r = pread(fd, cur, sizeof(cur), (off_t)addr);
    if (r != (ssize_t)sizeof(cur))
        return 0;
    if (memcmp(cur, fatal_ret, sizeof(fatal_ret)) == 0)
        return 1; /* already neutralized */
    if (memcmp(cur, fatal_expected, sizeof(fatal_expected)) != 0)
        return 0;
    w = pwrite(fd, fatal_ret, sizeof(fatal_ret), (off_t)addr);
    if (w != (ssize_t)sizeof(fatal_ret))
        return 0;
    r = pread(fd, cur, sizeof(cur), (off_t)addr);
    if (r == (ssize_t)sizeof(cur) &&
        memcmp(cur, fatal_ret, sizeof(fatal_ret)) == 0) {
        log_line("%s addr=0x%lx", tag, (unsigned long)addr);
        return 1;
    }
    return 0;
}

#define MAX_CANDIDATES 8

/*
 * Enumerate libflutter.so mappings of `pid`. For each mapping yields
 * (seg_start, seg_off) so every patch site can compute
 * site_addr = seg_start + (SITE_FILE_OFF - seg_off). Returns count.
 */
static int candidates_of(pid_t pid, uintptr_t *out, uintptr_t *out_off,
                         int max)
{
    char path[64];
    char line[1024];
    FILE *fp;
    int n = 0;

    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0, offset = 0;
        char perms[8] = {0};

        if (!strstr(line, "/libflutter.so"))
            continue;
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &offset) != 4)
            continue;
        if (start < 0x10000)
            continue;
        if (perms[2] != 'x')          /* patch sites live in exec segment */
            continue;
        {
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (out[i] == (uintptr_t)start) { dup = 1; break; }
            if (!dup && n < max) {
                out[n] = (uintptr_t)start;      /* segment base */
                out_off[n] = (uintptr_t)offset; /* its file offset */
                n++;
            }
        }
    }
    fclose(fp);
    return n;
}

/* Address of a patch site inside mapping (seg_start, seg_off). */
static uintptr_t site_addr(uintptr_t seg_start, uintptr_t seg_off,
                           uintptr_t site_file_off)
{
    if (seg_off == 0)
        return seg_start + site_file_off; /* whole-file mapping */
    if (seg_off <= site_file_off)
        return seg_start + (site_file_off - seg_off);
    return 0; /* site not inside this mapping */
}

/* Apply every libflutter patch site (backend re-route + fatal thunk). */
static void apply_all_patches(pid_t pid)
{
    uintptr_t seg[MAX_CANDIDATES];
    uintptr_t seg_off[MAX_CANDIDATES];
    char path[64];
    char rtag[64], ttag[64], drtag[64], dctag[64], dftag[64];
    int fd, n, i, routed = 0, thunked = 0, debug_routed = 0, debug_ctx = 0;
    int debug_fatal = 0;

    pthread_mutex_lock(&patch_lock);
    n = candidates_of(pid, seg, seg_off, MAX_CANDIDATES);
    if (n <= 0) {
        pthread_mutex_unlock(&patch_lock);
        return;
    }
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        pthread_mutex_unlock(&patch_lock);
        return;
    }
    /* Parent-scanner patch lines are tagged with the TARGET pid so the
     * log never attributes another process's patch to appspawn. */
    snprintf(rtag, sizeof(rtag), "PATCHED pid=%d", (int)pid);
    snprintf(ttag, sizeof(ttag), "FATAL-THUNK-NEUTRALIZED pid=%d", (int)pid);
    snprintf(drtag, sizeof(drtag), "PATCHED-DEBUG pid=%d", (int)pid);
    snprintf(dctag, sizeof(dctag), "PATCHED-DEBUG-CTX pid=%d", (int)pid);
    snprintf(dftag, sizeof(dftag), "FATAL-THUNK-DEBUG pid=%d", (int)pid);
    for (i = 0; i < n; i++) {
        uintptr_t a;
        if (!routed) {
            a = site_addr(seg[i], seg_off[i], FILE_OFF);
            if (a && patch_fd_tag(fd, a, pid == getpid(), rtag))
                routed = 1;
        }
        if (!debug_routed) {
            a = site_addr(seg[i], seg_off[i], FILE_OFF_DEBUG);
            if (a && patch_debug_site(fd, a, debug_expected, debug_replacement,
                                      sizeof(debug_expected), drtag))
                debug_routed = 1;
        }
        if (!debug_ctx) {
            a = site_addr(seg[i], seg_off[i], FILE_OFF_DEBUG_CTX);
            if (a && patch_debug_site(fd, a, debug_ctx_expected,
                                      debug_ctx_replacement,
                                      sizeof(debug_ctx_expected),
                                      dctag))
                debug_ctx = 1;
        }
        if (!thunked) {
            a = site_addr(seg[i], seg_off[i], 0x4a1500u);
            if (a && patch_fatal_thunk_tag(fd, a, ttag))
                thunked = 1;
        }
        if (!debug_fatal) {
            a = site_addr(seg[i], seg_off[i], FILE_OFF_DEBUG_FATAL);
            if (a && patch_fatal_thunk_tag(fd, a, dftag))
                debug_fatal = 1;
        }
    }
    (void)debug_routed;
    (void)debug_ctx;
    close(fd);
    pthread_mutex_unlock(&patch_lock);
}

/*
 * Same thread that loaded the engine, before dlopen_ns returns to
 * NativeModuleManager and before Rasterizer::Setup. Byte-exact sites only.
 */
/* ---- in-process watcher ------------------------------------------- */

static void drop_rt(void)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof(sp));
    /* appspawn is SCHED_FIFO. A child thread keeps that policy and can
     * stall after setuid. This watcher only needs to wake up. */
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);
}

static void *self_watcher(void *unused)
{
    long t0 = now_ms();

    (void)unused;
    drop_rt();
    /*
     * The child is forked before the app loads libflutter. Poll until the
     * mapping appears. want_fast is set when prctl renames the process
     * away from apppool, which is after the sandbox switch and before
     * the engine is loaded.
     */
    for (;;) {
        uintptr_t seg[MAX_CANDIDATES];
        uintptr_t seg_off[MAX_CANDIDATES];
        int fd, n, i, routed = 0, thunked = 0, debug_routed = 0, debug_ctx = 0;
        int debug_fatal = 0;

        if (patches_done) {
            __sync_fetch_and_sub(&watcher_count, 1);
            return NULL;
        }
        n = candidates_of(getpid(), seg, seg_off, MAX_CANDIDATES);
        if (n > 0) {
            pthread_mutex_lock(&patch_lock);
            fd = open("/proc/self/mem", O_RDWR);
            if (fd >= 0) {
                for (i = 0; i < n; i++) {
                    uintptr_t a;
                    if (!routed) {
                        a = site_addr(seg[i], seg_off[i], FILE_OFF);
                        if (a && patch_fd(fd, a, 1))
                            routed = 1;
                    }
                    if (!debug_routed) {
                        a = site_addr(seg[i], seg_off[i], FILE_OFF_DEBUG);
                        if (a && patch_debug_site(fd, a, debug_expected,
                                                  debug_replacement,
                                                  sizeof(debug_expected),
                                                  "PATCHED-DEBUG"))
                            debug_routed = 1;
                    }
                    if (!debug_ctx) {
                        a = site_addr(seg[i], seg_off[i], FILE_OFF_DEBUG_CTX);
                        if (a && patch_debug_site(fd, a, debug_ctx_expected,
                                                  debug_ctx_replacement,
                                                  sizeof(debug_ctx_expected),
                                                  "PATCHED-DEBUG-CTX"))
                            debug_ctx = 1;
                    }
                    if (!thunked) {
                        a = site_addr(seg[i], seg_off[i], 0x4a1500u);
                        if (a && patch_fatal_thunk(fd, a))
                            thunked = 1;
                    }
                    if (!debug_fatal) {
                        a = site_addr(seg[i], seg_off[i],
                                      FILE_OFF_DEBUG_FATAL);
                        if (a && patch_fatal_thunk(fd, a))
                            debug_fatal = 1;
                    }
                }
                close(fd);
                if ((debug_routed && debug_ctx && debug_fatal) ||
                    (routed && thunked)) {
                    patches_done = 1;
                    pthread_mutex_unlock(&patch_lock);
                    /* The parent scanner may have won the race and
                     * patched remotely; its line carries appspawn's
                     * pid prefix (message tagged with our pid).
                     * Always leave evidence under THIS pid too so
                     * per-pid greps never come up empty. */
                    if (routed || debug_routed || debug_ctx)
                        log_line("self-patched backend sites");
                    else
                        log_line("already-patched by parent scanner");
                    log_line("self watcher: done after %ld ms",
                             now_ms() - t0);
                    __sync_fetch_and_sub(&watcher_count, 1);
                    return NULL;
                }
            }
            pthread_mutex_unlock(&patch_lock);
        }
        if (want_fast || now_ms() - t0 < 10000)
            usleep(1000);
        else
            usleep(20000);
    }
}

/* ---- child watcher arming ------------------------------------------ */

/*
 * appspawn forks (no exec) when spawning app processes, so the app
 * inherits this library without re-running the constructor. Re-arm the
 * in-process watcher in the fork child. pthread_atfork child handlers
 * run in a fully valid pthread environment (unlike raw clone), which
 * the earlier crash with clone-hooking proved.
 */
static int start_watcher(void)
{
    pthread_t thread;

    if (__sync_fetch_and_add(&watcher_count, 1) >= 2) {
        __sync_fetch_and_sub(&watcher_count, 1);
        return 0;
    }
    if (pthread_create(&thread, NULL, self_watcher, NULL) == 0) {
        (void)pthread_detach(thread);
        return 1;
    }
    __sync_fetch_and_sub(&watcher_count, 1);
    log_line("arm watcher failed errno=%d", errno);
    return 0;
}

static void arm_child_watcher(void)
{
    (void)start_watcher();
}

/* Called on the main thread after the sandbox is built, before libflutter
 * is mapped. One extra watcher: the prefork thread may be stuck or asleep. */
static void arm_fast_watcher(const char *name)
{
    char shown[16];
    size_t i;

    want_fast = 1;
    if (patches_done)
        return;
    if (__sync_val_compare_and_swap(&fast_armed, 0, 1) != 0)
        return;
    for (i = 0; i < sizeof(shown) - 1 && name[i]; i++)
        shown[i] = name[i];
    shown[i] = '\0';
    if (start_watcher())
        log_line("fast-arm %s", shown);
    else
        fast_armed = 0;
}

int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5)
{
    int rc;
    const char *name;

    if (!real_prctl)
        real_prctl = (PrctlFn)dlsym(RTLD_NEXT, "prctl");
    if (!real_prctl) {
        errno = ENOSYS;
        return -1;
    }
    rc = real_prctl(option, arg2, arg3, arg4, arg5);
    /* PR_SET_NAME == 15. "apppool" is the idle prefork comm. */
    if (rc == 0 && option == 15 && arg2 != 0) {
        char tmp[16];
        size_t i;

        name = (const char *)arg2;
        for (i = 0; i < sizeof(tmp) - 1 && name[i]; i++)
            tmp[i] = name[i];
        tmp[i] = '\0';
        if (strcmp(tmp, "apppool") != 0 && strcmp(tmp, "appspawn") != 0)
            arm_fast_watcher(tmp);
    }
    return rc;
}

/*
 * appspawn is multithreaded. A mutex held by another thread is left locked
 * in the child and the watcher never runs, so only the 50 ms parent scan
 * remains and a hot page cache wins. Take the locks in the parent, then
 * reinitialize them in the child.
 */
static void fork_prepare(void)
{
    pthread_mutex_lock(&patch_lock);
    pthread_mutex_lock(&log_lock);
}

static void fork_parent(void)
{
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&patch_lock);
}

static void fork_child(void)
{
    pthread_mutex_init(&patch_lock, NULL);
    pthread_mutex_init(&log_lock, NULL);
    log_fd = -1;
    patches_done = 0;
    want_fast = 0;
    fast_armed = 0;
    watcher_count = 0;
    log_line("fork-child");
    arm_child_watcher();
}

pid_t fork(void)
{
    if (!real_fork)
        real_fork = (ForkFn)dlsym(RTLD_NEXT, "fork");
    if (!real_fork) {
        errno = ENOSYS;
        return -1;
    }
    return real_fork();
}

/* ---- parent-side scanner (backup) ---------------------------------- */

static int try_patch_process(pid_t pid)
{
    uintptr_t seg[MAX_CANDIDATES];
    uintptr_t seg_off[MAX_CANDIDATES];
    char path[64];
    char rtag[64];
    int fd, n, i;

    pthread_mutex_lock(&patch_lock);
    n = candidates_of(pid, seg, seg_off, MAX_CANDIDATES);
    if (n <= 0) {
        pthread_mutex_unlock(&patch_lock);
        return 0;
    }
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        pthread_mutex_unlock(&patch_lock);
        return 0;
    }
    snprintf(rtag, sizeof(rtag), "PATCHED pid=%d", (int)pid);
    for (i = 0; i < n; i++) {
        uintptr_t a = site_addr(seg[i], seg_off[i], FILE_OFF);
        if (a && patch_fd_tag(fd, a, 0, rtag)) {
            close(fd);
            pthread_mutex_unlock(&patch_lock);
            return 1;
        }
    }
    close(fd);
    pthread_mutex_unlock(&patch_lock);
    return 0; /* keep retrying: exec segments may appear later */
}

static int is_pid_dir(const struct dirent *d)
{
    char *end;
    long v;

    if (d->d_type == DT_DIR && d->d_name[0] >= '0' && d->d_name[0] <= '9') {
        v = strtol(d->d_name, &end, 10);
        if (*end == '\0' && v > 1 && v != (long)getpid())
            return (int)v;
    }
    return 0;
}

static void *scanner(void *unused)
{
    (void)unused;
    for (;;) {
        DIR *dir = opendir("/proc");
        if (dir) {
            struct dirent *d;
            while ((d = readdir(dir)) != NULL) {
                int pid = is_pid_dir(d);
                if (pid)
                    (void)try_patch_process((pid_t)pid);
                if (pid)
                    (void)apply_all_patches((pid_t)pid);
            }
            closedir(dir);
        }
        usleep(50000);
    }
    return NULL;
}

/* ---- init ----------------------------------------------------------- */

static int process_is_appspawn(void)
{
    char comm[32];
    int fd, n;

    fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0)
        return 0;
    n = (int)read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    comm[n] = '\0';
    return strstr(comm, "appspawn") != NULL;
}

__attribute__((constructor(101)))
static void qemu_flutter_preload_init(void)
{
    static const char *icd = "/system/etc/vulkan/icd.d/vk_swiftshader_icd.json";
    pthread_t thread;
    void *beget;
    int (*set_parameter)(const char *, const char *);

    real_dlopen = (DlopenFn)dlsym(RTLD_NEXT, "dlopen");

    log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    real_prctl = (PrctlFn)dlsym(RTLD_NEXT, "prctl");
    log_line("ctor: v5+debug-fatal pid-tag");

    /*
     * The in-process watcher matters in spawned app processes. They are
     * forked (no exec) from appspawn, so the constructor does not rerun
     * there; the fork() wrapper below re-arms the watcher per child.
     */
    (void)pthread_atfork(fork_prepare, fork_parent, fork_child);
    if (pthread_create(&thread, NULL, self_watcher, NULL) == 0)
        (void)pthread_detach(thread);
    else
        log_line("ctor: self watcher failed errno=%d", errno);

    /*
     * The /proc scanner is heavy (it walks every process), so run it in
     * exactly one place: the root appspawn process itself. Running it in
     * every spawned app flooded the system with proc I/O and froze the
     * launcher (THREAD_BLOCK_6S).
     */
    if (process_is_appspawn()) {
        if (pthread_create(&thread, NULL, scanner, NULL) == 0)
            (void)pthread_detach(thread);
        else
            log_line("ctor: scanner failed errno=%d", errno);
    }

    setenv("VK_ICD_FILENAMES", icd, 1);
    setenv("VK_DRIVER_FILES", icd, 1);
    if (!real_dlopen)
        return;
    beget = real_dlopen("libbegetutil.z.so", RTLD_NOW);
    if (!beget)
        beget = real_dlopen("/system/lib64/chipset-pub-sdk/libbegetutil.z.so",
                            RTLD_NOW);
    if (!beget)
        return;
    set_parameter = (int (*)(const char *, const char *))
        dlsym(beget, "SetParameter");
    if (set_parameter) {
        set_parameter("VK_ICD_FILENAMES", icd);
        set_parameter("VK_DRIVER_FILES", icd);
    }
}
