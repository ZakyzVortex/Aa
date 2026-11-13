#include <EGL/egl.h>
#include <GLES2/gl2.h>

extern "C" void test_android_graphics() {
    EGLContext context = eglGetCurrentContext();
    glBindTexture(GL_TEXTURE_2D, 0);
    (void)context;
}
