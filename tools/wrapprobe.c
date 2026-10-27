// End-to-end reproduction of RenderContext::InitializeEglContext +
// SetUpGpuContext through the OHOS EGL wrapper (platformsdk/libEGL.so +
// libGLESv3.so), instead of direct mesa. This mirrors SystemUI's exact call
// sequence so whatever makes glGetString() return NULL there should reproduce
// here, where we can print every intermediate value.
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_EXTENSIONS 0x3055
#define EGL_GL_VERSION 0x3098
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00

typedef EGLDisplay (*pfn_GetDisplay)(void *);
typedef EGLBoolean (*pfn_Initialize)(EGLDisplay, EGLint *, EGLint *);
typedef const char *(*pfn_QueryString)(EGLDisplay, EGLint);
typedef EGLBoolean (*pfn_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext (*pfn_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface (*pfn_CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLBoolean (*pfn_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint (*pfn_GetError)(void);
typedef void *(*pfn_GetProcAddress)(const char *);
typedef const unsigned char *(*pfn_GetString)(unsigned int);

int main(void)
{
    printf("== wrapprobe: full wrapper path ==\n");
    void *hEgl = dlopen("/system/lib64/platformsdk/libEGL.so", 2 /*RTLD_NOW*/ | 0x100 /*RTLD_GLOBAL*/);
    printf("[dlopen libEGL.so(wrapper)] = %p err=%s\n", hEgl, hEgl ? "-" : dlerror());
    if (!hEgl) return 1;
    void *hGl = dlopen("/system/lib64/platformsdk/libGLESv3.so", 2 | 0x100);
    printf("[dlopen libGLESv3.so(wrapper)] = %p err=%s\n", hGl, hGl ? "-" : dlerror());

    pfn_GetDisplay eglGetDisplay = (pfn_GetDisplay)dlsym(hEgl, "eglGetDisplay");
    pfn_Initialize eglInitialize = (pfn_Initialize)dlsym(hEgl, "eglInitialize");
    pfn_QueryString eglQueryString = (pfn_QueryString)dlsym(hEgl, "eglQueryString");
    pfn_ChooseConfig eglChooseConfig = (pfn_ChooseConfig)dlsym(hEgl, "eglChooseConfig");
    pfn_CreateContext eglCreateContext = (pfn_CreateContext)dlsym(hEgl, "eglCreateContext");
    pfn_CreatePbuffer eglCreatePbufferSurface = (pfn_CreatePbuffer)dlsym(hEgl, "eglCreatePbufferSurface");
    pfn_MakeCurrent eglMakeCurrent = (pfn_MakeCurrent)dlsym(hEgl, "eglMakeCurrent");
    pfn_GetError eglGetError = (pfn_GetError)dlsym(hEgl, "eglGetError");
    pfn_GetProcAddress eglGetProcAddress = (pfn_GetProcAddress)dlsym(hEgl, "eglGetProcAddress");
    pfn_GetString glGetString = (pfn_GetString)dlsym(hGl, "glGetString");
    printf("[symbols] getDisplay=%p init=%p queryString=%p chooseConfig=%p createContext=%p "
           "createPbuffer=%p makeCurrent=%p getError=%p getProcAddress=%p glGetString=%p\n",
           eglGetDisplay, eglInitialize, eglQueryString, eglChooseConfig, eglCreateContext,
           eglCreatePbufferSurface, eglMakeCurrent, eglGetError, eglGetProcAddress, glGetString);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("[1] eglGetDisplay(default) = %p err=0x%x\n", dpy, eglGetError());
    if (dpy == EGL_NO_DISPLAY) return 2;

    EGLint major = 0, minor = 0;
    EGLBoolean ok = eglInitialize(dpy, &major, &minor);
    printf("[2] eglInitialize = %d ver=%d.%d err=0x%x\n", ok, major, minor, eglGetError());
    if (!ok) return 3;

    const char *ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("[3] eglQueryString(NO_DISPLAY, EXTENSIONS) = %s err=0x%x\n", ext ? ext : "(null)", eglGetError());

    EGLint cfgAttribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig cfg = 0; EGLint cnt = 0;
    ok = eglChooseConfig(dpy, cfgAttribs, &cfg, 1, &cnt);
    printf("[4] eglChooseConfig(W|PB,ES3) = %d cnt=%d cfg=%p err=0x%x\n", ok, cnt, cfg, eglGetError());
    if (!ok || cnt < 1) return 4;

    EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    printf("[5] eglCreateContext(ES3) = %p err=0x%x\n", ctx, eglGetError());
    if (ctx == EGL_NO_CONTEXT) return 5;

    EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttribs);
    printf("[6] eglCreatePbufferSurface(1x1) = %p err=0x%x\n", pb, eglGetError());

    ok = eglMakeCurrent(dpy, pb, pb, ctx);
    printf("[7] eglMakeCurrent = %d err=0x%x\n", ok, eglGetError());
    if (!ok) return 7;

    // dump loaded GL-related libraries to check for duplicate glapi/mesa instances
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        char seen[16][256];
        int nseen = 0;
        printf("[maps]\n");
        while (fgets(line, sizeof(line), maps)) {
            if (!strstr(line, "/") || strstr(line, ".so") == NULL) continue;
            if (!strstr(line, "glapi") && !strstr(line, "EGL") && !strstr(line, "GLES") &&
                !strstr(line, "gallium") && !strstr(line, "dri/") && !strstr(line, "drm")) continue;
            char path[256] = "";
            char *tab = strrchr(line, '/');
            if (!tab) continue;
            snprintf(path, sizeof(path), "%s", tab + 1);
            char *nl = strchr(path, '\n'); if (nl) *nl = 0;
            int dup = 0;
            for (int i = 0; i < nseen; i++) if (strcmp(seen[i], path) == 0) dup = 1;
            if (!dup && nseen < 16) {
                snprintf(seen[nseen++], 256, "%s", path);
                // full path: find the first '/' after the perms column
                char *p = strchr(line, '/');
                char full[300] = "";
                if (p) { snprintf(full, sizeof(full), "%s", p); nl = strchr(full, '\n'); if (nl) *nl = 0; }
                printf("  %s\n", full);
            }
        }
        fclose(maps);
    }

    const unsigned char *v = glGetString(GL_VERSION);
    const unsigned char *r = glGetString(GL_RENDERER);
    printf("[8] glGetString(wrapper) VERSION=%s RENDERER=%s err=0x%x\n",
           v ? (const char *)v : "(null)", r ? (const char *)r : "(null)", eglGetError());

    // Discriminating experiment: three independent sources of glGetString
    // a) mesa libEGL_impl's own eglGetProcAddress (bypasses wrapper entirely)
    // b) dlsym from vendor libGLESv2_impl copy
    // c) dlsym from vendor libGLESv3_impl copy (what wrapper table3 actually uses)
    void *hEglImpl = dlopen("/vendor/lib64/chipsetsdk/libEGL_impl.so", 2);
    printf("[dlopen libEGL_impl] = %p\n", hEglImpl);
    if (hEglImpl) {
        pfn_GetProcAddress mesaGetProc = (pfn_GetProcAddress)dlsym(hEglImpl, "eglGetProcAddress");
        pfn_GetString g1 = (pfn_GetString)mesaGetProc("glGetString");
        printf("[11] mesa eglGetProcAddress(glGetString) = %p\n", g1);
        if (g1) {
            const unsigned char *m = g1(GL_VERSION);
            printf("[12] via libEGL_impl entry: VERSION=%s\n", m ? (const char *)m : "(null)");
        }
    }
    void *hGl2Impl = dlopen("/vendor/lib64/chipsetsdk/libGLESv2_impl.so", 2);
    if (hGl2Impl) {
        pfn_GetString g2 = (pfn_GetString)dlsym(hGl2Impl, "glGetString");
        const unsigned char *m2 = g2 ? g2(GL_VERSION) : 0;
        printf("[13] via libGLESv2_impl dlsym: glGetString=%p VERSION=%s\n", g2, m2 ? (const char *)m2 : "(null)");
    }
    void *hGl3Impl = dlopen("/vendor/lib64/chipsetsdk/libGLESv3_impl.so", 2);
    if (hGl3Impl) {
        pfn_GetString g3 = (pfn_GetString)dlsym(hGl3Impl, "glGetString");
        const unsigned char *m3 = g3 ? g3(GL_VERSION) : 0;
        printf("[14] via libGLESv3_impl dlsym (wrapper's table3 source): glGetString=%p VERSION=%s\n", g3, m3 ? (const char *)m3 : "(null)");
    }
    // check glapi state directly
    void *hGlapi = dlopen("/system/lib64/libglapi.so.0", 2);
    if (hGlapi) {
        const char *(*getDispatch)() = (const char *(*)())dlsym(hGlapi, "_glapi_get_dispatch");
        const char *(*getContext)() = (const char *(*)())dlsym(hGlapi, "_glapi_get_context");
        printf("[15] libglapi: get_dispatch=%p -> %p, get_context=%p -> %p\n",
               getDispatch, getDispatch ? (void *)getDispatch() : 0, getContext, getContext ? (void *)getContext() : 0);
    }

    if (!v && eglGetProcAddress) {
        pfn_GetString direct = (pfn_GetString)eglGetProcAddress("glGetString");
        printf("[9] eglGetProcAddress(glGetString) = %p\n", direct);
        if (direct) {
            const unsigned char *v2 = direct(GL_VERSION);
            printf("[10] direct VERSION=%s\n", v2 ? (const char *)v2 : "(null)");
        }
    }
    printf("== wrapprobe done ==\n");
    return 0;
}
