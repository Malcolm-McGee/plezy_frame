#ifndef PLEZY_LINUX_XR_UI_PANEL_H_
#define PLEZY_LINUX_XR_UI_PANEL_H_

#include <GLES3/gl3.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "ui_layout.h"

namespace xr {

struct UiTile {
  std::string id;
  std::string title;
  std::string subtitle;
  std::string image;  // path to a JPEG/PNG poster, or empty
};

// The VR menu: a poster grid with a title and a back button, drawn into an OpenGL texture that the
// renderer shows on a quad in front of the viewer. All methods must be called with the GL context current.
class UiPanel {
 public:
  static constexpr int kNone = -1;
  static constexpr int kBack = -2;

  bool Init(std::string* error);
  void Shutdown();

  // Replace the screen. Tiles are added between BeginScreen and EndScreen.
  void BeginScreen(const std::string& title, bool can_back);
  void AddTile(UiTile tile);
  void EndScreen();
  // Show a message instead of tiles ("Loading...", an error).
  void SetStatus(const std::string& title, bool can_back, const std::string& message);
  // Replace the tiles with a message, keeping the title and back button.
  void ShowMessage(const std::string& message);
  // A message drawn over the whole panel (e.g. "Loading..." while a film starts); empty clears it.
  void SetOverlay(const std::string& text);

  // What a panel point (pixels, origin top-left) is over: a tile index, kBack, or kNone.
  int HitTest(float px, float py) const;
  void SetHover(int target);
  int hover() const { return hover_; }
  void ScrollBy(float pixels);

  bool can_back() const { return can_back_; }
  int tile_count() const { return static_cast<int>(tiles_.size()); }
  const UiTile& tile(int i) const { return tiles_[static_cast<size_t>(i)]; }

  // Draws the panel into its texture if anything changed. Also decodes a few pending posters per call.
  void Render();
  GLuint texture() const { return color_tex_; }
  bool DumpPng(const std::string& path);

 private:
  struct Font {
    GLuint texture = 0;
    float size = 0;
    std::vector<unsigned char> baked;  // stbtt_bakedchar array, opaque here
  };
  struct Image {
    GLuint texture = 0;
    int w = 0, h = 0;
    bool failed = false;
  };

  bool BakeFont(Font* font, const std::string& path, float size, std::string* error);
  float TextWidth(const Font& font, const std::string& text) const;
  std::string Fit(const Font& font, const std::string& text, float max_width) const;
  void Text(const Font& font, const std::string& text, float x, float baseline, const float color[4]);
  void Quad(float x, float y, float w, float h, const float color[4]);
  void ImageQuad(GLuint texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1);
  void Draw(const float* vertices, int count, int mode, GLuint texture, const float color[4]);
  void LoadPendingImages(int budget);
  void DrawTile(int index);
  void Invalidate() { dirty_ = true; }

  GLuint program_ = 0, vao_ = 0, vbo_ = 0, fbo_ = 0, color_tex_ = 0;
  GLint u_size_ = -1, u_mode_ = -1, u_color_ = -1, u_tex_ = -1;
  Font font_title_, font_caption_, font_small_, font_big_;
  std::unordered_map<std::string, Image> images_;

  std::string title_ = "Plezy";
  std::string status_;
  std::string overlay_;
  bool can_back_ = false;
  std::vector<UiTile> tiles_;
  GridMetrics grid_;
  float scroll_ = 0.f;
  int hover_ = kNone;
  bool dirty_ = true;
};

}  // namespace xr

#endif  // PLEZY_LINUX_XR_UI_PANEL_H_
