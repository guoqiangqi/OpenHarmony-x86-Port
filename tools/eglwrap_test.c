// Simulate the exact loading sequence of OHOS EGL wrapper's
// EglWrapperLoader::LoadVendorDriver() (egl_wrapper_loader.cpp):
//   1. dlopen vendor libEGL_impl.so, take eglGetProcAddress
//   2. dlopen vendor libGLESv3_impl.so (RTLD_NOW|RTLD_LOCAL)
//   3. dlsym(dlGl3, "glGetString"); if null fallback eglGetProcAddress("glGetString")
// Prints every step so we can see exactly where the wrapper's GL hook table
// ends up with a null glGetString (which makes glGetString() return 0).
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>

typedef void *(*getproc_t)(const char *);

static void probe_gl_lib(const char *path, getproc_t getproc)
{
    // musl: RTLD_LAZY=1 RTLD_NOW=2 RTLD_GLOBAL=0x100 RTLD_LOCAL=0 RTLD_NOLOAD=4
    void *h = dlopen(path, 2 /*RTLD_NOW | RTLD_LOCAL(musl=0)*/);
    printf("[lib] %s dlopen=%p err=%s\n", path, h, h ? "-" : dlerror());
    if (!h) return;
    const char *names[] = { "glGetString", "glShaderSource", "glClearColor",
                            "glGetIntegerv", "glReadPixels", "glFinish", NULL };
    for (int i = 0; names[i]; i++) {
        void *sym = dlsym(h, names[i]);
        printf("  dlsym(%-16s) = %p", names[i], sym);
        if (!sym && getproc) {
            void *fp = getproc(names[i]);
            printf("  fallback eglGetProcAddress = %p", fp);
        }
        printf("\n");
    }
    // also check a GLES1-only symbol via v1 lib
}

int main(void)
{
    printf("== eglwrap_test: simulate wrapper LoadVendorDriver ==\n");

    void *dlEgl = dlopen("/vendor/lib64/chipsetsdk/libEGL_impl.so", 2 /*RTLD_NOW*/);
    printf("[EGL] dlopen=%p err=%s\n", dlEgl, dlEgl ? "-" : dlerror());
    if (!dlEgl) return 1;

    getproc_t getproc = (getproc_t)dlsym(dlEgl, "eglGetProcAddress");
    printf("[EGL] dlsym(eglGetProcAddress)=%p\n", getproc);

    void *sym = dlsym(dlEgl, "eglGetProcAddress");
    (void)sym;

    probe_gl_lib("/vendor/lib64/chipsetsdk/libGLESv1_impl.so", getproc);
    probe_gl_lib("/vendor/lib64/chipsetsdk/libGLESv2_impl.so", getproc);
    probe_gl_lib("/vendor/lib64/chipsetsdk/libGLESv3_impl.so", getproc);

    // raw symbols exported by the EGL impl itself (for reference)
    const char *eglNames[] = { "eglGetDisplay", "eglInitialize", "eglCreateContext",
                               "eglMakeCurrent", "glGetString", NULL };
    printf("[EGL extra]\n");
    for (int i = 0; eglNames[i]; i++) {
        void *s = dlsym(dlEgl, eglNames[i]);
        printf("  dlsym(%-20s)=%p\n", eglNames[i], s);
    }
    if (getproc) {
        printf("[EGL getproc]\n");
        const char *q[] = { "glGetString", "eglGetProcAddress", "glShaderSource", NULL };
        for (int i = 0; q[i]; i++)
            printf("  eglGetProcAddress(%-18s)=%p\n", q[i], getproc(q[i]));
    }
    printf("== done ==\n");
    return 0;
}
