#!/usr/bin/env python3
"""Surfaceless EGL + GLES2 on the first render node: print the renderer and time a simple draw loop."""
import ctypes as C, time, sys
egl = C.CDLL("libEGL.so.1"); gles = C.CDLL("libGLESv2.so.2")
EGL_PLATFORM_SURFACELESS_MESA = 0x31DD; EGL_NONE = 0x3038; EGL_OPENGL_ES_API = 0x30A0
EGL_RENDERABLE_TYPE = 0x3040; EGL_OPENGL_ES2_BIT = 4; EGL_SURFACE_TYPE = 0x3033; EGL_PBUFFER_BIT = 1
EGL_CONTEXT_CLIENT_VERSION = 0x3098; EGL_NO_SURFACE = C.c_void_p(0)
egl.eglGetProcAddress.restype = C.c_void_p
gpd = C.CFUNCTYPE(C.c_void_p, C.c_uint, C.c_void_p, C.POINTER(C.c_int))(egl.eglGetProcAddress(b"eglGetPlatformDisplayEXT"))
dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, None, None)
assert dpy, "no display"
maj, mnr = C.c_int(), C.c_int()
assert egl.eglInitialize(C.c_void_p(dpy), C.byref(maj), C.byref(mnr)), "eglInitialize"
egl.eglQueryString.restype = C.c_char_p
print("EGL", maj.value, mnr.value, egl.eglQueryString(C.c_void_p(dpy), 0x3053).decode())
egl.eglBindAPI(EGL_OPENGL_ES_API)
attribs = (C.c_int * 7)(EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE, 0, 0)
cfg = C.c_void_p(); n = C.c_int()
assert egl.eglChooseConfig(C.c_void_p(dpy), attribs, C.byref(cfg), 1, C.byref(n)) and n.value, "config"
cattr = (C.c_int * 3)(EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE)
egl.eglCreateContext.restype = C.c_void_p
ctx = egl.eglCreateContext(C.c_void_p(dpy), cfg, None, cattr); assert ctx, "context"
assert egl.eglMakeCurrent(C.c_void_p(dpy), EGL_NO_SURFACE, EGL_NO_SURFACE, C.c_void_p(ctx)), "makecurrent"
gles.glGetString.restype = C.c_char_p
for name, e in (("VENDOR", 0x1F00), ("RENDERER", 0x1F01), ("VERSION", 0x1F02)):
    print(name, gles.glGetString(e).decode())
# draw loop into a 1024x1024 renderbuffer FBO
fbo = C.c_uint(); rb = C.c_uint()
gles.glGenFramebuffers(1, C.byref(fbo)); gles.glGenRenderbuffers(1, C.byref(rb))
gles.glBindRenderbuffer(0x8D41, rb); gles.glRenderbufferStorage(0x8D41, 0x8058, 1024, 1024)  # RGBA8
gles.glBindFramebuffer(0x8D40, fbo); gles.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
print("FBO status", hex(gles.glCheckFramebufferStatus(0x8D40)))
gles.glViewport(0, 0, 1024, 1024)
t = time.time(); frames = 0
while time.time() - t < 3:
    gles.glClearColor(C.c_float(frames % 2), C.c_float(0.5), C.c_float(0.2), C.c_float(1.0)); gles.glClear(0x4000)
    gles.glFinish(); frames += 1
print(f"{frames} clears+finish of 1024x1024 in 3 s = {frames/3:.0f}/s")

buf = (C.c_ubyte * 16)()
gles.glReadPixels(0, 0, 2, 2, 0x1908, 0x1401, buf)  # RGBA, UNSIGNED_BYTE
print("pixel readback:", list(buf[:4]))
