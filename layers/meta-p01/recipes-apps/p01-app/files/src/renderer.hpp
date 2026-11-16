// p01-app - Mali-400 OpenGL ES 2.0 composition on the DisplayPort framebuffer (EGL fbdev backend)
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <string>
#include <vector>

namespace p01 {

struct Rgba {
  float r, g, b, a;
};

class Renderer {
 public:
  ~Renderer();
  bool init();  // full-screen window on /dev/fb0
  int  width() const { return width_; }
  int  height() const { return height_; }

  // YUYV frame -> texture (w/2 x h RGBA texels); converted to RGB in the fragment shader.
  void upload_yuyv(const uint8_t* yuyv, int w, int h);

  // Zero-copy alternative: textures on each CMA buffer through EGL_EXT_image_dma_buf_import. The
  // GPU reads the buffer itself; no upload. Two views of the same YUYV bytes: w/2 x h ABGR8888
  // (chroma) and, if the driver accepts it, w x h GR88 (luma) for bilinear scaling.
  bool dmabuf_supported() const { return dmabuf_ok_; }
  bool import_yuyv(int slot, int dmabuf_fd, int w, int h, int stride);
  void use_slot(int slot);  // the next video() shows this imported buffer

  void begin();
  void video(float x, float y, float w, float h);
  void rect(float x, float y, float w, float h, Rgba c);
  void frame_rect(float x, float y, float w, float h, float t, Rgba c);
  void text(float x, float y, const std::string& s, float scale, Rgba c);
  // Text with a thin dark outline: readable on any video, no background box.
  void text_shadow(float x, float y, const std::string& s, float scale, Rgba c);
  float text_width(const std::string& s, float scale) const;
  float line_height(float scale) const;
  // Draws the batched geometry and swaps buffers; optionally saves the composed frame as PPM.
  void end(const std::string& shot_path = "");

 private:
  bool   save_ppm(const std::string& path);
  void   make_parity(int w);
  void   draw_yuv(float x, float y, float w, float h, float tw, float th);
  EGLImageKHR make_image_texture(const EGLint* attr, GLuint& tex);
  GLuint program(const char* vs, const char* fs);
  void   flush_solid();
  void   flush_text();

  EGLDisplay dpy_ = EGL_NO_DISPLAY;
  EGLSurface surf_ = EGL_NO_SURFACE;
  EGLContext ctx_ = EGL_NO_CONTEXT;
  int        width_ = 0, height_ = 0;
  GLuint     prog_video_ = 0, prog_split_ = 0, prog_rgb_ = 0, prog_solid_ = 0, prog_text_ = 0;
  GLuint     fbo_ = 0, tex_fbo_ = 0;  // scaled video: source-size RGB texture
  int        fbo_w_ = 0, fbo_h_ = 0;
  GLuint     tex_video_ = 0, tex_font_ = 0, tex_current_ = 0, tex_luma_ = 0, tex_parity_ = 0;
  int        video_w_ = 0, video_h_ = 0, parity_w_ = 0;
  bool       dmabuf_ok_ = false, split_ok_ = false;
  std::vector<GLuint>      slot_tex_, slot_luma_;
  std::vector<EGLImageKHR> slot_img_, slot_imgy_;
  PFNEGLCREATEIMAGEKHRPROC            create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC           destroy_image_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
  std::vector<float> solid_, text_;  // batched vertices
};

}  // namespace p01
