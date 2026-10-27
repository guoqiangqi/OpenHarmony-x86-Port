// Minimal virtio-gpu dumb buffer probe:
//   1. CREATE_DUMB 1280x832 (like DrmAllocator)
//   2. PRIME_HANDLE_TO_FD, DESTROY_DUMB (like DrmAllocator)
//   3. CPU mmap(fd) and write a Blue/Green gradient (like RS + mapper)
//   4. PRIME_FD_TO_HANDLE, ADDFB2 (like DrmGemBuffer)
//   5. legacy SETCRTC to scan the FB out (triggers transfer_to_host_2d)
// If the gradient shows on the QEMU display, the dumb/export/mmap/import/transfer
// chain is fine and the composer does something different; if not, the minimal
// repro is in hand and the offending step can be bisected via argv flags.
//
// usage: drmprobe [nommapprime]   (nommapprime: write via MAP_DUMB instead of dma-buf mmap)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#define CARD "/dev/dri/card0"

static void dump_conn_modes(int fd)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { printf("GETRESOURCES err %d\n", errno); return; }
    uint32_t *fbIds = calloc(res.count_fbs, sizeof(uint32_t));
    uint32_t *crtcIds = calloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t *encIds = calloc(res.count_encoders, sizeof(uint32_t));
    uint32_t *connIds = calloc(res.count_connectors, sizeof(uint32_t));
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbIds;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcIds;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encIds;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connIds;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { printf("GETRESOURCES2 err %d\n", errno); return; }

    for (unsigned c = 0; c < res.count_connectors; c++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = connIds[c];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) { printf("GETCONNECTOR err\n"); continue; }
        struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(*modes));
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.props_ptr = 0; conn.prop_values_ptr = 0; conn.encoders_ptr = 0;
        conn.count_props = 0; conn.count_encoders = 0;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) { printf("GETCONNECTOR2 err\n"); free(modes); continue; }
        printf("connector %u: %u modes, conn=%s\n", connIds[c], conn.count_modes,
            conn.connection == 1 ? "connected" : "disconnected");
        for (unsigned m = 0; m < conn.count_modes; m++) {
            printf("  mode[%u] %ux%u@%u type=%u\n", m, modes[m].hdisplay, modes[m].vdisplay,
                modes[m].vrefresh, modes[m].type);
        }
        free(modes);
    }
    free(connIds);
}

int main(int argc, char **argv)
{
    int usePrimeMmap = 1;
    int noExport = 0;
    if (argc > 1 && strcmp(argv[1], "nommapprime") == 0) usePrimeMmap = 0;
    if (argc > 1 && strcmp(argv[1], "noexport") == 0) { noExport = 1; usePrimeMmap = 0; }

    int fd = open(CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("open %s err %d\n", CARD, errno); return 1; }
    printf("opened %s fd=%d\n", CARD, fd);
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0) printf("SET_MASTER err %d\n", errno);
    else printf("SET_MASTER ok\n");
    dump_conn_modes(fd);

    // 1. create dumb 1280x832 like DrmAllocator (widthStride x heightStride)
    struct drm_mode_create_dumb dumb = {0};
    dumb.width = 1280; dumb.height = 800; dumb.bpp = 32;   // 800: exact mode size (fbcon-like)
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) { printf("CREATE_DUMB err %d\n", errno); return 1; }
    printf("dumb handle=%u pitch=%u size=%llu\n", dumb.handle, dumb.pitch, (unsigned long long)dumb.size);

    // 2. export prime fd then destroy dumb handle (DrmAllocator behaviour)
    struct drm_prime_handle ph;
    memset(&ph, 0, sizeof(ph));
    if (!noExport) {
        ph.handle = dumb.handle; ph.flags = DRM_CLOEXEC | DRM_RDWR;
        if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0) { printf("HANDLE_TO_FD err %d\n", errno); return 1; }
        printf("prime fd=%d\n", ph.fd);

        struct drm_mode_destroy_dumb dd; memset(&dd, 0, sizeof(dd)); dd.handle = dumb.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) < 0) printf("DESTROY_DUMB warn %d\n", errno);
    }

    // 3. cpu write gradient
    const uint32_t W = 1280, H = 800, PITCH = dumb.pitch;
    void *addr = MAP_FAILED;
    uint64_t mapOffset = 0;
    if (usePrimeMmap) {
        addr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, ph.fd, 0);
        if (addr == MAP_FAILED) { printf("prime mmap err %d\n", errno); return 1; }
        printf("cpu map via dma-buf mmap: %p\n", addr);
    } else if (noExport) {
        struct drm_mode_map_dumb md2; memset(&md2, 0, sizeof(md2)); md2.handle = dumb.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md2) < 0) { printf("MAP_DUMB(noexport) err %d\n", errno); return 1; }
        addr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md2.offset);
        if (addr == MAP_FAILED) { printf("drm mmap(noexport) err %d\n", errno); return 1; }
        printf("cpu map via MAP_DUMB(noexport): %p\n", addr);
    } else {
        struct drm_mode_map_dumb md; memset(&md, 0, sizeof(md)); md.handle = dumb.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
            printf("MAP_DUMB err %d (dumb handle was destroyed, expected)\n", errno); return 1;
        }
        mapOffset = md.offset;
        addr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapOffset);
        if (addr == MAP_FAILED) { printf("drm mmap err %d\n", errno); return 1; }
        printf("cpu map via MAP_DUMB offset %llu: %p\n", (unsigned long long)mapOffset, addr);
    }
    uint32_t *px = (uint32_t *)addr;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t b = x * 255u / W;
            uint32_t g = y * 255u / H;
            px[(size_t)y * (PITCH / 4u) + x] = 0xFF000000u | (b << 16) | (g << 8);
        }
    }
    printf("gradient written (BGRA: blue=x, green=y)\n");

    // 4. import prime fd -> gem handle, addfb2 (DrmGemBuffer behaviour)
    uint32_t useHandle = dumb.handle;
    if (!noExport) {
        struct drm_prime_handle pi; memset(&pi, 0, sizeof(pi));
        pi.fd = ph.fd; pi.flags = 0;
        if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &pi) < 0) { printf("FD_TO_HANDLE err %d\n", errno); return 1; }
        printf("imported gem handle=%u\n", pi.handle);
        useHandle = pi.handle;
    }

    uint32_t fb = 0;
    uint32_t handles[4] = {useHandle, 0, 0, 0};
    uint32_t pitches[4] = {PITCH, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    struct drm_mode_fb_cmd2 fc;
    memset(&fc, 0, sizeof(fc));
    fc.width = W; fc.height = H;
    fc.pixel_format = DRM_FORMAT_ARGB8888;
    memcpy(fc.handles, handles, sizeof(handles));
    memcpy(fc.pitches, pitches, sizeof(pitches));
    memcpy(fc.offsets, offsets, sizeof(offsets));
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fc) < 0) { printf("ADDFB2 err %d\n", errno); return 1; }
    fb = fc.fb_id;
    printf("fb id=%u\n", fb);

    // 4.5 decisive test: alloc buffer B and inspect its initial content.
    // If B starts with our gradient, A's mmap wrote ordinary buddy pages
    // (recycled into B), i.e. A's mmap is NOT backed by A's pinned sgt pages.
    {
        struct drm_mode_create_dumb dumb2 = {0};
        dumb2.width = 1280; dumb2.height = 832; dumb2.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb2) == 0) {
            struct drm_mode_map_dumb md2; memset(&md2, 0, sizeof(md2)); md2.handle = dumb2.handle;
            if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md2) == 0) {
                void *addr2 = mmap(NULL, dumb2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md2.offset);
                if (addr2 != MAP_FAILED) {
                    uint32_t *p2 = (uint32_t *)addr2;
                    long grad = 0, zero = 0, other = 0;
                    for (uint32_t i = 0; i < 1280 * 800; i += 61) {
                        uint32_t v = p2[i];
                        uint32_t x = i % 1280, y = i / 1280;
                        uint32_t b = x * 255u / 1280, g = y * 255u / 800;
                        if (v == (0xFF000000u | (b << 16) | (g << 8))) grad++;
                        else if (v == 0) zero++;
                        else other++;
                    }
                    printf("BUFFER-B content: gradient=%ld zero=%ld other=%ld\n", grad, zero, other);
                    munmap(addr2, dumb2.size);
                }
            }
            struct drm_mode_destroy_dumb dd2; memset(&dd2, 0, sizeof(dd2)); dd2.handle = dumb2.handle;
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd2);
        }
    }

    // 5. find a connected connector + encoder/crtc, then SETCRTC
    struct drm_mode_card_res res; memset(&res, 0, sizeof(res));
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t *connIds = calloc(res.count_connectors, sizeof(uint32_t));
    uint32_t *crtcIds = calloc(res.count_crtcs, sizeof(uint32_t));
    uint32_t *fbIds = calloc(res.count_fbs, sizeof(uint32_t));
    uint32_t *encIds = calloc(res.count_encoders, sizeof(uint32_t));
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbIds;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcIds;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encIds;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connIds;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    uint32_t connId = 0, crtcId = 0;
    struct drm_mode_modeinfo want; memset(&want, 0, sizeof(want));
    for (unsigned c = 0; c < res.count_connectors && connId == 0; c++) {
        struct drm_mode_get_connector conn; memset(&conn, 0, sizeof(conn));
        conn.connector_id = connIds[c];
        ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
        if (conn.connection != 1 || conn.count_modes == 0) continue;
        struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(*modes));
        conn.modes_ptr = (uint64_t)(uintptr_t)modes; conn.props_ptr = 0; conn.prop_values_ptr = 0; conn.encoders_ptr = 0;
        conn.count_props = 0; conn.count_encoders = 0;
        ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
        for (unsigned m = 0; m < conn.count_modes; m++) {
            if (modes[m].hdisplay == 1280 && modes[m].vdisplay == 800) {
                connId = connIds[c];
                want = modes[m];
                break;
            }
        }
        free(modes);
    }
    if (connId == 0) { printf("no 1280x800 connector found\n"); return 1; }
    crtcId = crtcIds[0];
    printf("using connector %u crtc %u mode %ux%u@%u\n", connId, crtcId, want.hdisplay, want.vdisplay, want.vrefresh);

    // setcrtc (dpms on first)
    struct drm_mode_crtc crtc; memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtcId;
    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc);
    crtc.fb_id = fb; crtc.count_connectors = 1;
    uint32_t conn4 = connId;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn4;
    crtc.mode_valid = 1; crtc.mode = want; crtc.x = 0; crtc.y = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) { printf("SETCRTC err %d\n", errno); return 1; }
    printf("SETCRTC ok - gradient should now be on screen\n");

    // redraw after 3s with inverted pattern to prove live updates work
    sleep(3);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t r = x * 255u / W;
            px[(size_t)y * (PITCH / 4u) + x] = 0xFF000000u | (r << 16); // red=x
        }
    }
    crtc.fb_id = fb; crtc.mode_valid = 1; crtc.x = 0; crtc.y = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) printf("second SETCRTC err %d\n", errno);
    printf("second pattern (red=x) written\n");
    sleep(10);
    printf("done\n");
    return 0;
}
