// Minimal EGL probe: dlopen the exact mesa impl used by SystemUI and walk the
// full EGL init chain, printing every return value. Links only libc/libdl so
// it can run anywhere (static glibc build still works on the musl guest).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLConfig;
typedef void* EGLNativeDisplayType;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef void (*(__attribute__((stdcall)) *GlGetStringFn))(int); // placeholder, fixed below

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_OPENGL_ES_API 0x30A0
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_VENDOR_ 0x1F00
#define GL_EXTENSIONS 0x1F03

typedef EGLDisplay (*GetDisplayFn)(EGLNativeDisplayType);
typedef EGLBoolean (*InitializeFn)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean (*BindAPIFn)(EGLenum);
typedef const char* (*QueryStringFn)(EGLDisplay, EGLint);
typedef EGLBoolean (*ChooseConfigFn)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext (*CreateContextFn)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLSurface (*CreatePbufferFn)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLBoolean (*MakeCurrentFn)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef void* (*GetProcFn)(const char*);
typedef const unsigned char* (*GlStrFn)(EGLenum);

static const char* implPath;

static void* must(const char* name)
{
    void* f = dlsym(RTLD_DEFAULT, name);
    if (!f) {
        printf("[MISS] %s\n", name);
    }
    return f;
}

int main(int argc, char** argv)
{
    implPath = argc > 1 ? argv[1] : "/vendor/lib64/chipsetsdk/libEGL_impl.so";
    printf("== eglprobe: dlopen(%s)\n", implPath);
    void* h = dlopen(implPath, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("dlopen ok\n");

    GetDisplayFn eglGetDisplay = (GetDisplayFn)dlsym(h, "eglGetDisplay");
    InitializeFn eglInitialize = (InitializeFn)dlsym(h, "eglInitialize");
    BindAPIFn eglBindAPI = (BindAPIFn)dlsym(h, "eglBindAPI");
    QueryStringFn eglQueryString = (QueryStringFn)dlsym(h, "eglQueryString");
    ChooseConfigFn eglChooseConfig = (ChooseConfigFn)dlsym(h, "eglChooseConfig");
    CreateContextFn eglCreateContext = (CreateContextFn)dlsym(h, "eglCreateContext");
    CreatePbufferFn eglCreatePbufferSurface = (CreatePbufferFn)dlsym(h, "eglCreatePbufferSurface");
    MakeCurrentFn eglMakeCurrent = (MakeCurrentFn)dlsym(h, "eglMakeCurrent");
    GetProcFn eglGetProcAddress = (GetProcFn)dlsym(h, "eglGetProcAddress");
    if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglCreateContext || !eglMakeCurrent) {
        printf("missing core EGL symbols\n");
        return 1;
    }

    printf("LIBGL_ALWAYS_SOFTWARE=%s\n", getenv("LIBGL_ALWAYS_SOFTWARE"));
    printf("MESA_LOADER_DRIVER_OVERRIDE=%s\n", getenv("MESA_LOADER_DRIVER_OVERRIDE"));

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay(default) = %p\n", dpy);
    if (dpy == EGL_NO_DISPLAY) return 1;

    EGLint maj = 0, min = 0;
    EGLBoolean ok = eglInitialize(dpy, &maj, &min);
    printf("eglInitialize = %d ver=%d.%d\n", ok, maj, min);
    if (!ok) return 1;
    if (eglQueryString) {
        printf("EGL_VENDOR=%s\n", eglQueryString(dpy, EGL_VENDOR));
        printf("EGL_VERSION=%s\n", eglQueryString(dpy, EGL_VERSION));
        const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
        printf("EGL_EXTENSIONS(120c)=%.120s\n", ext ? ext : "(null)");
    }
    if (eglBindAPI) {
        printf("eglBindAPI(ES) = %d\n", eglBindAPI(EGL_OPENGL_ES_API));
    }

    EGLint cfgAttribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig cfg = NULL;
    EGLint n = 0;
    ok = eglChooseConfig(dpy, cfgAttribs, &cfg, 1, &n);
    printf("eglChooseConfig(W|PB, ES3) = %d n=%d cfg=%p\n", ok, n, cfg);
    if (!ok || n < 1) return 1;

    EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    printf("eglCreateContext(ES3) = %p\n", ctx);
    if (ctx == EGL_NO_CONTEXT) return 1;

    EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pbAttribs);
    printf("eglCreatePbufferSurface = %p\n", pb);

    EGLSurface draw = pb ? pb : EGL_NO_SURFACE;
    ok = eglMakeCurrent(dpy, draw, draw, ctx);
    printf("eglMakeCurrent(%s) = %d\n", pb ? "pbuffer" : "surfaceless", ok);

    GlStrFn glGetString = NULL;
    if (eglGetProcAddress) {
        glGetString = (GlStrFn)eglGetProcAddress("glGetString");
    }
    if (!glGetString) {
        glGetString = (GlStrFn)dlsym(h, "glGetString");
    }
    printf("glGetString fn = %p\n", glGetString);
    if (glGetString) {
        const unsigned char* v = glGetString(GL_VERSION);
        const unsigned char* r = glGetString(GL_RENDERER);
        const unsigned char* ven = glGetString(GL_VENDOR_);
        printf("GL_VERSION = %s\n", v ? (const char*)v : "(null)");
        printf("GL_RENDERER = %s\n", r ? (const char*)r : "(null)");
        printf("GL_VENDOR = %s\n", ven ? (const char*)ven : "(null)");
    }
    printf("== probe done\n");
    return 0;
}
