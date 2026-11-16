// p01-app - Mali-400 OpenGL ES 2.0 renderer (EGL fbdev)
#include "renderer.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common.hpp"
#include "font_atlas.hpp"

namespace p01 {

namespace {

// Native window type of the libMali fbdev backend.
struct fbdev_window {
  unsigned short width;
  unsigned short height;
};
fbdev_window g_win;

// Pixel coordinates (top-left origin) -> clip space.
const char* kVsPixel = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_col;
uniform vec2 u_screen;
varying vec2 v_uv;
varying vec4 v_col;
void main() {
  vec2 p = a_pos / u_screen * 2.0 - 1.0;
  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
  v_uv = a_uv;
  v_col = a_col;
})";

// YUV -> RGB: BT.601 limited range, as the camera / p01_preproc.
// The Mali-400 fragment shader computes in fp16, so no arithmetic on texture coordinates: every
// lookup uses the varying directly (exact texel addressing in the texture unit).
//
// Split mode (dma-buf): luma from the YUYV buffer imported as GR88 (w x h, R = Y of each pixel,
// bilinear), chroma from the same buffer imported as ABGR8888 (w/2 x h, G = U, A = V, bilinear).
const char* kFsVideoSplit = R"(
precision mediump float;
uniform sampler2D u_luma;
uniform sampler2D u_chroma;
varying vec2 v_uv;
varying vec4 v_col;
void main() {
  float y = 1.164 * (texture2D(u_luma, v_uv).r - 0.0627);
  vec4  c = texture2D(u_chroma, v_uv);
  float u = c.g - 0.502;
  float v = c.a - 0.502;
  gl_FragColor = vec4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);
})";

// Packed mode (upload, or no GR88 import): YUYV texel = (Y0, U, Y1, V), nearest; whether a pixel is
// Y0 or Y1 comes from a w x 1 parity texture (0, 1, 0, 1, ...) looked up with the same coordinate.
const char* kFsVideo = R"(
precision mediump float;
uniform sampler2D u_tex;
uniform sampler2D u_parity;
varying vec2 v_uv;
varying vec4 v_col;
void main() {
  vec4  t   = texture2D(u_tex, v_uv);
  float odd = texture2D(u_parity, v_uv).a;
  float y = 1.164 * (mix(t.r, t.b, odd) - 0.0627);
  float u = t.g - 0.502;
  float v = t.a - 0.502;
  gl_FragColor = vec4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);
})";

// RGB texture (scaled video from the offscreen pass), bilinear.
const char* kFsRgb = R"(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_col;
void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); })";

const char* kFsSolid = R"(
precision mediump float;
varying vec2 v_uv;
varying vec4 v_col;
void main() { gl_FragColor = v_col; })";

const char* kFsText = R"(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_col;
void main() { gl_FragColor = vec4(v_col.rgb, v_col.a * texture2D(u_tex, v_uv).a); })";

GLuint shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char msg[512];
    glGetShaderInfoLog(s, sizeof(msg), nullptr, msg);
    log("gles: shader error: %s", msg);
  }
  return s;
}

void push_quad(std::vector<float>& v, float x0, float y0, float x1, float y1, float u0, float v0,
               float u1, float v1, Rgba c) {
  const float q[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                         {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
  for (const auto& p : q) v.insert(v.end(), {p[0], p[1], p[2], p[3], c.r, c.g, c.b, c.a});
}

constexpr int kStride = 8 * sizeof(float);

constexpr EGLint kFourccABGR8888 = 0x34324241;  // DRM_FORMAT_ABGR8888 'AB24': bytes R, G, B, A
constexpr EGLint kFourccGR88     = 0x38385247;  // DRM_FORMAT_GR88 'GR88': bytes R, G

bool has_ext(const char* list, const char* name) {
  if (!list) return false;
  const size_t n = std::strlen(name);
  for (const char* p = std::strstr(list, name); p; p = std::strstr(p + 1, name)) {
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
  }
  return false;
}

}  // namespace

Renderer::~Renderer() {
  if (dpy_ != EGL_NO_DISPLAY) {
    for (EGLImageKHR img : slot_img_) {
      if (img != EGL_NO_IMAGE_KHR && destroy_image_) destroy_image_(dpy_, img);
    }
    for (EGLImageKHR img : slot_imgy_) {
      if (img != EGL_NO_IMAGE_KHR && destroy_image_) destroy_image_(dpy_, img);
    }
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
    if (surf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surf_);
    eglTerminate(dpy_);
  }
}

bool Renderer::init() {
  // Screen size from the framebuffer driver (xlnxdrmfb on the PS DisplayPort)
  const int fb = open("/dev/fb0", O_RDONLY);
  fb_var_screeninfo vi{};
  if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &vi) < 0) {
    log("gles: cannot query /dev/fb0");
    if (fb >= 0) close(fb);
    return false;
  }
  close(fb);
  width_  = int(vi.xres);
  height_ = int(vi.yres);
  g_win   = {static_cast<unsigned short>(width_), static_cast<unsigned short>(height_)};

  dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, &major, &minor)) {
    log("gles: eglInitialize failed");
    return false;
  }
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, int(vi.red.length), EGL_GREEN_SIZE, int(vi.green.length),
                             EGL_BLUE_SIZE, int(vi.blue.length), EGL_NONE};
  EGLConfig cfg;
  EGLint    n = 0;
  if (!eglChooseConfig(dpy_, cfg_attr, &cfg, 1, &n) || n < 1) {
    log("gles: no EGL config for %d bpp", int(vi.bits_per_pixel));
    return false;
  }
  surf_ = eglCreateWindowSurface(dpy_, cfg, reinterpret_cast<EGLNativeWindowType>(&g_win), nullptr);
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  ctx_ = eglCreateContext(dpy_, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (surf_ == EGL_NO_SURFACE || ctx_ == EGL_NO_CONTEXT || !eglMakeCurrent(dpy_, surf_, surf_, ctx_)) {
    log("gles: surface / context creation failed (0x%x)", eglGetError());
    return false;
  }
  eglSwapInterval(dpy_, 1);
  log("gles: EGL %d.%d, %s, %s, %dx%d %d bpp", major, minor, glGetString(GL_RENDERER),
      glGetString(GL_VERSION), width_, height_, int(vi.bits_per_pixel));

  const char* egl_ext = eglQueryString(dpy_, EGL_EXTENSIONS);
  const char* gl_ext  = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  create_image_  = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  image_target_  = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  dmabuf_ok_ = has_ext(egl_ext, "EGL_EXT_image_dma_buf_import") && has_ext(gl_ext, "GL_OES_EGL_image") &&
               create_image_ && destroy_image_ && image_target_;
  log("gles: EGL_EXT_image_dma_buf_import %s, GL_OES_EGL_image %s", has_ext(egl_ext, "EGL_EXT_image_dma_buf_import") ? "yes" : "no",
      has_ext(gl_ext, "GL_OES_EGL_image") ? "yes" : "no");

  prog_video_ = program(kVsPixel, kFsVideo);
  prog_split_ = program(kVsPixel, kFsVideoSplit);
  prog_rgb_   = program(kVsPixel, kFsRgb);
  prog_solid_ = program(kVsPixel, kFsSolid);
  prog_text_  = program(kVsPixel, kFsText);

  glGenTextures(1, &tex_video_);
  tex_current_ = tex_video_;
  glBindTexture(GL_TEXTURE_2D, tex_video_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &tex_font_);
  glBindTexture(GL_TEXTURE_2D, tex_font_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, font::kAtlasW, font::kAtlasH, 0, GL_ALPHA,
               GL_UNSIGNED_BYTE, font::kAtlas);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  glViewport(0, 0, width_, height_);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  return true;
}

GLuint Renderer::program(const char* vs, const char* fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
  glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(p, 0, "a_pos");
  glBindAttribLocation(p, 1, "a_uv");
  glBindAttribLocation(p, 2, "a_col");
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) log("gles: program link failed");
  return p;
}

void Renderer::make_parity(int w) {
  if (tex_parity_ && parity_w_ == w) return;
  std::vector<uint8_t> p(static_cast<size_t>(w));
  for (int x = 0; x < w; x++) p[size_t(x)] = (x & 1) ? 255 : 0;
  if (!tex_parity_) glGenTextures(1, &tex_parity_);
  glBindTexture(GL_TEXTURE_2D, tex_parity_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, p.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  parity_w_ = w;
}

void Renderer::upload_yuyv(const uint8_t* yuyv, int w, int h) {
  make_parity(w);
  tex_current_ = tex_video_;
  tex_luma_    = 0;
  glBindTexture(GL_TEXTURE_2D, tex_video_);
  if (w != video_w_ || h != video_h_) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w / 2, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, yuyv);
    video_w_ = w;
    video_h_ = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h, GL_RGBA, GL_UNSIGNED_BYTE, yuyv);
  }
}

bool Renderer::import_yuyv(int slot, int dmabuf_fd, int w, int h, int stride) {
  if (!dmabuf_ok_) return false;
  if (int(slot_tex_.size()) <= slot) {
    slot_tex_.resize(size_t(slot) + 1, 0);
    slot_img_.resize(size_t(slot) + 1, EGL_NO_IMAGE_KHR);
    slot_luma_.resize(size_t(slot) + 1, 0);
    slot_imgy_.resize(size_t(slot) + 1, EGL_NO_IMAGE_KHR);
  }
  const EGLint attr[] = {EGL_WIDTH, w / 2, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, kFourccABGR8888,
                         EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                         EGL_DMA_BUF_PLANE0_PITCH_EXT, stride, EGL_NONE};
  // Chroma / packed YUYV view: w/2 x h ABGR8888
  GLuint t = 0;
  EGLImageKHR img = make_image_texture(attr, t);
  if (img == EGL_NO_IMAGE_KHR) {
    log("gles: dma-buf import of slot %d failed (0x%x)", slot, eglGetError());
    return false;
  }
  // Luma view: w x h GR88 (R = Y of every pixel). Optional: without it the packed shader is used.
  GLuint      ty = 0;
  EGLImageKHR imgy = EGL_NO_IMAGE_KHR;
  if (slot == 0 || split_ok_) {
    const EGLint attr_y[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, kFourccGR88,
                             EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT, stride, EGL_NONE};
    imgy = make_image_texture(attr_y, ty);
    if (slot == 0) {
      split_ok_ = imgy != EGL_NO_IMAGE_KHR;
      log("gles: luma view GR88 %s", split_ok_ ? "imported: bilinear luma + chroma" : "not supported: packed YUYV + parity");
    }
    if (imgy == EGL_NO_IMAGE_KHR) split_ok_ = false;
  }
  const GLint filter = split_ok_ ? GL_LINEAR : GL_NEAREST;
  for (GLuint tex : {t, ty}) {
    if (!tex) continue;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  }
  slot_tex_[size_t(slot)]  = t;
  slot_img_[size_t(slot)]  = img;
  slot_luma_[size_t(slot)] = ty;
  slot_imgy_[size_t(slot)] = imgy;
  make_parity(w);
  video_w_ = w;
  video_h_ = h;
  return true;
}

EGLImageKHR Renderer::make_image_texture(const EGLint* attr, GLuint& tex) {
  EGLImageKHR img = create_image_(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
  if (img == EGL_NO_IMAGE_KHR) return img;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  image_target_(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &tex);
    tex = 0;
    destroy_image_(dpy_, img);
    return EGL_NO_IMAGE_KHR;
  }
  return img;
}

void Renderer::use_slot(int slot) {
  tex_current_ = slot_tex_[size_t(slot)];
  tex_luma_    = split_ok_ ? slot_luma_[size_t(slot)] : 0;
}

void Renderer::begin() {
  glClearColor(0.04f, 0.05f, 0.07f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  solid_.clear();
  text_.clear();
}

// Draws the current YUV texture(s) as RGB into the rectangle of a target of size (tw, th).
void Renderer::draw_yuv(float x, float y, float w, float h, float tw, float th) {
  std::vector<float> v;
  push_quad(v, x, y, x + w, y + h, 0.f, 0.f, 1.f, 1.f, {1, 1, 1, 1});
  const GLuint prog = tex_luma_ ? prog_split_ : prog_video_;
  glUseProgram(prog);
  glUniform2f(glGetUniformLocation(prog, "u_screen"), tw, th);
  if (tex_luma_) {
    glUniform1i(glGetUniformLocation(prog, "u_luma"), 0);
    glUniform1i(glGetUniformLocation(prog, "u_chroma"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_current_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_luma_);
  } else {
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(prog, "u_parity"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_parity_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_current_);
  }
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, v.data());
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, v.data() + 2);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, v.data() + 4);
  for (int i = 0; i < 3; i++) glEnableVertexAttribArray(i);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}

// 1:1 (e.g. 1080p source on the 1080p screen): direct. Scaled: YUV -> RGB at the source size into
// an offscreen texture (exact texel per pixel), then a bilinear draw of that RGB texture.
void Renderer::video(float x, float y, float w, float h) {
  if (!video_w_) return;
  if (int(w + 0.5f) == video_w_ && int(h + 0.5f) == video_h_) {
    draw_yuv(x, y, w, h, float(width_), float(height_));
    return;
  }
  if (!fbo_ || fbo_w_ != video_w_ || fbo_h_ != video_h_) {
    if (!fbo_) {
      glGenFramebuffers(1, &fbo_);
      glGenTextures(1, &tex_fbo_);
    }
    glBindTexture(GL_TEXTURE_2D, tex_fbo_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, video_w_, video_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_fbo_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      log("gles: offscreen framebuffer incomplete, drawing the video directly");
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &fbo_);
      fbo_ = 0;
      draw_yuv(x, y, w, h, float(width_), float(height_));
      return;
    }
    fbo_w_ = video_w_;
    fbo_h_ = video_h_;
  }
  // pass 1: source-size RGB (rows flipped by the pixel-to-clip mapping, undone in pass 2)
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, video_w_, video_h_);
  glDisable(GL_BLEND);
  draw_yuv(0, 0, float(video_w_), float(video_h_), float(video_w_), float(video_h_));
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
  // pass 2: bilinear scale to the screen
  std::vector<float> v;
  push_quad(v, x, y, x + w, y + h, 0.f, 1.f, 1.f, 0.f, {1, 1, 1, 1});
  glUseProgram(prog_rgb_);
  glUniform2f(glGetUniformLocation(prog_rgb_, "u_screen"), float(width_), float(height_));
  glUniform1i(glGetUniformLocation(prog_rgb_, "u_tex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_fbo_);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, v.data());
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, v.data() + 2);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, v.data() + 4);
  for (int i = 0; i < 3; i++) glEnableVertexAttribArray(i);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glEnable(GL_BLEND);
}

void Renderer::rect(float x, float y, float w, float h, Rgba c) {
  push_quad(solid_, x, y, x + w, y + h, 0, 0, 0, 0, c);
}

void Renderer::frame_rect(float x, float y, float w, float h, float t, Rgba c) {
  rect(x, y, w, t, c);
  rect(x, y + h - t, w, t, c);
  rect(x, y + t, t, h - 2 * t, c);
  rect(x + w - t, y + t, t, h - 2 * t, c);
}

float Renderer::text_width(const std::string& s, float scale) const {
  return float(s.size()) * font::kCellW * scale * 0.9f;
}

float Renderer::line_height(float scale) const { return font::kCellH * scale; }

void Renderer::text(float x, float y, const std::string& s, float scale, Rgba c) {
  const float cw = font::kCellW * scale, ch = font::kCellH * scale, adv = cw * 0.9f;
  for (char chr : s) {
    const int code = static_cast<unsigned char>(chr);
    if (code > font::kFirst && code <= font::kLast) {
      const int   i  = code - font::kFirst;
      // half-texel inset: linear filtering never samples the neighbouring cell
      const float u0 = (float((i % font::kCols) * font::kCellW) + 0.5f) / font::kAtlasW;
      const float v0 = (float((i / font::kCols) * font::kCellH) + 0.5f) / font::kAtlasH;
      const float u1 = u0 + float(font::kCellW - 1) / font::kAtlasW;
      const float v1 = v0 + float(font::kCellH - 1) / font::kAtlasH;
      push_quad(text_, x, y, x + cw, y + ch, u0, v0, u1, v1, c);
    }
    x += adv;
  }
}

void Renderer::text_shadow(float x, float y, const std::string& s, float scale, Rgba c) {
  const float d = std::max(1.f, 2.f * scale);
  const Rgba  k{0.f, 0.f, 0.f, 0.75f * c.a};
  text(x - d, y - d, s, scale, k);
  text(x + d, y - d, s, scale, k);
  text(x - d, y + d, s, scale, k);
  text(x + d, y + d, s, scale, k);
  text(x, y, s, scale, c);
}

void Renderer::flush_solid() {
  if (solid_.empty()) return;
  glUseProgram(prog_solid_);
  glUniform2f(glGetUniformLocation(prog_solid_, "u_screen"), float(width_), float(height_));
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, solid_.data());
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, solid_.data() + 2);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, solid_.data() + 4);
  for (int i = 0; i < 3; i++) glEnableVertexAttribArray(i);
  glDrawArrays(GL_TRIANGLES, 0, GLsizei(solid_.size() / 8));
}

void Renderer::flush_text() {
  if (text_.empty()) return;
  glUseProgram(prog_text_);
  glUniform2f(glGetUniformLocation(prog_text_, "u_screen"), float(width_), float(height_));
  glUniform1i(glGetUniformLocation(prog_text_, "u_tex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_font_);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, text_.data());
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, text_.data() + 2);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, text_.data() + 4);
  for (int i = 0; i < 3; i++) glEnableVertexAttribArray(i);
  glDrawArrays(GL_TRIANGLES, 0, GLsizei(text_.size() / 8));
}

void Renderer::end(const std::string& shot_path) {
  flush_solid();
  flush_text();
  if (!shot_path.empty() && !save_ppm(shot_path)) log("gles: cannot write %s", shot_path.c_str());
  eglSwapBuffers(dpy_, surf_);
}

// Reads the composed back buffer (call before eglSwapBuffers).
bool Renderer::save_ppm(const std::string& path) {
  std::vector<uint8_t> rgba(size_t(width_) * height_ * 4);
  glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width_, height_);
  std::vector<uint8_t> row(size_t(width_) * 3);
  for (int y = height_ - 1; y >= 0; y--) {
    const uint8_t* s = rgba.data() + size_t(y) * width_ * 4;
    for (int x = 0; x < width_; x++) {
      row[3 * x] = s[4 * x];
      row[3 * x + 1] = s[4 * x + 1];
      row[3 * x + 2] = s[4 * x + 2];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace p01
