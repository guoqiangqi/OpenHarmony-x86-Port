#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
 */

#define LOG_PATH "/data/log/flpatch.log"
#define VADDR_OFF 0x37bca8u   /* virtual address of the backend-selection cmp/je */
#define FILE_OFF 0x37aca8u    /* file offset of the same instruction           */
#define WATCH_TICKS 12000     /* x 5 ms = 60 s */

typedef void *(*DlopenFn)(const char *, int);
typedef pid_t (*ForkFn)(void);

static DlopenFn real_dlopen;
static ForkFn real_fork;
static int log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

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
        log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
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

/* Patch through an already-open /proc/<pid>/mem fd. */
static int patch_fd(int fd, uintptr_t addr, int log_fail)
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
        log_line("PATCHED addr=0x%lx", (unsigned long)addr);
        return 1;
    }
    return 0;
}

#define MAX_CANDIDATES 8

/*
 * Enumerate candidate addresses of the dispatch instruction from the
 * libflutter.so mappings of `pid`. Uses FILE_OFF arithmetic (see header
 * comment). Returns candidate count.
 */
static int candidates_of(pid_t pid, uintptr_t *out, int max)
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
        {
            uintptr_t addr;
            if (offset == 0)
                addr = (uintptr_t)start + FILE_OFF; /* whole-file mapping */
            else if (offset <= FILE_OFF)
                addr = (uintptr_t)start + (FILE_OFF - offset);
            else
                continue;
            if (addr < (uintptr_t)start || addr >= (uintptr_t)end)
                continue;
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (out[i] == addr) { dup = 1; break; }
            if (!dup && n < max)
                out[n++] = addr;
        }
    }
    fclose(fp);
    return n;
}

/* ---- in-process watcher ------------------------------------------- */

static void *self_watcher(void *unused)
{
    uintptr_t cand[MAX_CANDIDATES];
    int fd, n, i;
    long t0 = now_ms();

    (void)unused;
    for (int tick = 0; tick < WATCH_TICKS; tick++) {
        n = candidates_of(getpid(), cand, MAX_CANDIDATES);
        if (n > 0) {
            fd = open("/proc/self/mem", O_RDWR);
            if (fd >= 0) {
                for (i = 0; i < n; i++) {
                    if (patch_fd(fd, cand[i], 1)) {
                        close(fd);
                        log_line("self watcher: done after %ld ms",
                                 now_ms() - t0);
                        return NULL;
                    }
                }
                close(fd);
            }
        }
        usleep(5000);
    }
    return NULL;
}

/* ---- child watcher arming ------------------------------------------ */

/*
 * appspawn forks (no exec) when spawning app processes, so the app
 * inherits this library without re-running the constructor. Re-arm the
 * in-process watcher in the fork child. pthread_atfork child handlers
 * run in a fully valid pthread environment (unlike raw clone), which
 * the earlier crash with clone-hooking proved.
 */
static void arm_child_watcher(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, self_watcher, NULL) == 0)
        (void)pthread_detach(thread);
}

pid_t fork(void)
{
    pid_t pid;

    if (!real_fork)
        real_fork = (ForkFn)dlsym(RTLD_NEXT, "fork");
    if (!real_fork) {
        errno = ENOSYS;
        return -1;
    }
    pid = real_fork();
    if (pid == 0)
        arm_child_watcher();
    return pid;
}

/* ---- parent-side scanner (backup) ---------------------------------- */

static int try_patch_process(pid_t pid)
{
    uintptr_t cand[MAX_CANDIDATES];
    char path[64];
    int fd, n, i;

    n = candidates_of(pid, cand, MAX_CANDIDATES);
    if (n <= 0)
        return 0;
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    fd = open(path, O_RDWR);
    if (fd < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (patch_fd(fd, cand[i], 0)) {
            close(fd);
            return 1;
        }
    }
    close(fd);
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

    log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    log_line("ctor: watchers starting");

    /*
     * The in-process watcher matters in spawned app processes. They are
     * forked (no exec) from appspawn, so the constructor does not rerun
     * there; the fork() wrapper below re-arms the watcher per child.
     */
    (void)pthread_atfork(NULL, NULL, arm_child_watcher);
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
