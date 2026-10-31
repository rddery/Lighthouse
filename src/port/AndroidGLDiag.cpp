// Texture upload diagnostics for Android builds made with -DLIGHTHOUSE_GL_DIAG=ON.
//
// Wraps glTexImage2D: libultraship's calls bind to this definition, and the real driver entry point is
// called through dlsym. For the first uploads it saves the decoded pixels as PNGs, reads the texture
// back from the GPU to check the upload, and logs GL errors, to <app files>/diag/. The Android
// launcher copies that folder to Download/ on the next start.
#if defined(__ANDROID__) && defined(LIGHTHOUSE_GL_DIAG)

#include <GLES3/gl3.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" int stbi_write_png(char const* filename, int w, int h, int comp, const void* data, int stride_in_bytes);

namespace {
constexpr int kMaxDumps = 400;

using TexImage2DFn = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);

std::mutex sMutex;
int sUploads = 0;
int sDumped = 0;
FILE* sLog = nullptr;
std::string sDir;
GLuint sReadFbo = 0;

TexImage2DFn RealTexImage2D() {
    static TexImage2DFn fn = [] {
        void* lib = dlopen("libGLESv3.so", RTLD_NOW);
        return lib != nullptr ? (TexImage2DFn)dlsym(lib, "glTexImage2D") : nullptr;
    }();
    return fn;
}

bool OpenLog() {
    if (sLog != nullptr) {
        return true;
    }
    const char* base = SDL_AndroidGetExternalStoragePath();
    if (base == nullptr) {
        return false;
    }
    sDir = std::string(base) + "/diag";
    mkdir(sDir.c_str(), 0755);
    sLog = fopen((sDir + "/gl-diag.txt").c_str(), "w");
    if (sLog == nullptr) {
        return false;
    }
    fprintf(sLog, "GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\nGLSL: %s\n", glGetString(GL_VENDOR),
            glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint unpack[4] = {};
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack[0]);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack[1]);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack[2]);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack[3]);
    fprintf(sLog, "unpack alignment=%d row_length=%d skip_rows=%d skip_pixels=%d\n", unpack[0], unpack[1], unpack[2],
            unpack[3]);
    GLint pbo = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    fprintf(sLog, "pixel unpack buffer bound: %d\n\n", pbo);
    fprintf(sLog, "upload# tex level WxH internal/format/type glError readback\n");
    fflush(sLog);
    return true;
}

// Reads level `level` of the bound texture back through a framebuffer and counts pixels that differ.
std::string CheckReadback(GLuint tex, GLint level, GLsizei w, GLsizei h, const uint8_t* expected) {
    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    if (sReadFbo == 0) {
        glGenFramebuffers(1, &sReadFbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sReadFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    std::string result;
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        result = "fbo-incomplete(" + std::to_string(status) + ")";
    } else {
        std::vector<uint8_t> got((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
        size_t bad = 0, first = (size_t)-1;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            if (memcmp(&got[i * 4], &expected[i * 4], 4) != 0) {
                if (bad++ == 0) {
                    first = i;
                }
            }
        }
        if (bad == 0) {
            result = "ok";
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "MISMATCH %zu/%zu px, first at (%zu,%zu) sent %02x%02x%02x%02x got %02x%02x%02x%02x", bad,
                     (size_t)w * h, first % w, first / w, expected[first * 4], expected[first * 4 + 1],
                     expected[first * 4 + 2], expected[first * 4 + 3], got[first * 4], got[first * 4 + 1],
                     got[first * 4 + 2], got[first * 4 + 3]);
            result = buf;
        }
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    return result;
}
} // namespace

extern "C" GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                                    GLsizei height, GLint border, GLenum format, GLenum type,
                                                    const void* pixels) {
    TexImage2DFn real = RealTexImage2D();
    if (real == nullptr) {
        return;
    }
    // Errors raised by earlier calls (anything since the previous upload)
    std::string prior;
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
        char hex[16];
        snprintf(hex, sizeof(hex), " 0x%x", e);
        prior += hex;
    }
    real(target, level, internalformat, width, height, border, format, type, pixels);
    GLenum err = glGetError();

    std::lock_guard<std::mutex> lock(sMutex);
    int n = sUploads++;
    if (n >= kMaxDumps || !OpenLog()) {
        return;
    }
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    std::string readback = "skipped";
    const bool rgba8 = format == GL_RGBA && type == GL_UNSIGNED_BYTE && pixels != nullptr;
    if (rgba8 && err == GL_NO_ERROR) {
        readback = CheckReadback(tex, level, width, height, (const uint8_t*)pixels);
    }
    fprintf(sLog, "%d tex=%d level=%d %dx%d 0x%x/0x%x/0x%x err=0x%x %s%s%s\n", n, tex, level, width, height,
            internalformat, format, type, err, readback.c_str(),
            prior.empty() ? "" : " | earlier errors:", prior.c_str());
    fflush(sLog);
    if (rgba8 && level == 0 && width >= 4 && height >= 4 && sDumped < kMaxDumps) {
        char name[64];
        snprintf(name, sizeof(name), "/tex%03d_%dx%d.png", n, width, height);
        stbi_write_png((sDir + name).c_str(), width, height, 4, pixels, width * 4);
        sDumped++;
    }
}

#endif
