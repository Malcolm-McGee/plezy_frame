#include "ui_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// stb is third-party and not warning-clean under -Wall -Wextra.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_TGA
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace xr {

namespace {

constexpr int kFirstChar = 32;
constexpr int kCharCount = 224;  // ASCII + Latin-1
constexpr int kAtlas = 1024;

const char* kVert = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
uniform vec2 u_size;
out vec2 v_uv;
void main() {
  v_uv = a_uv;
  // Row 0 of the texture is the top of the panel, matching the video texture convention.
  gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, a_pos.y / u_size.y * 2.0 - 1.0, 0.0, 1.0);
})";

const char* kFrag = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in vec2 v_uv;
uniform int u_mode;  // 0 solid colour, 1 RGBA image, 2 font alpha mask
uniform vec4 u_color;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
  if (u_mode == 0) {
    o_color = u_color;
  } else if (u_mode == 1) {
    o_color = texture(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_color.a);
  } else {
    o_color = vec4(u_color.rgb, u_color.a * texture(u_tex, v_uv).r);
  }
})";

GLuint CompileShader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "plezy-xr: ui shader error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

// Next code point of a UTF-8 string; invalid bytes come back as '?'.
unsigned Utf8Next(const std::string& s, size_t* i) {
  const unsigned char c = static_cast<unsigned char>(s[*i]);
  if (c < 0x80) {
    ++*i;
    return c;
  }
  int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
  if (extra < 0 || *i + static_cast<size_t>(extra) >= s.size() + 0) {
    ++*i;
    return '?';
  }
  unsigned cp = c & (0x3F >> extra);
  for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[*i + static_cast<size_t>(k)]) & 0x3F);
  *i += static_cast<size_t>(extra) + 1;
  return cp;
}

// Typographic punctuation a movie title is likely to contain, mapped to what the Latin-1 atlas has.
unsigned Fold(unsigned cp) {
  switch (cp) {
    case 0x2018:
    case 0x2019:
      return '\'';
    case 0x201C:
    case 0x201D:
      return '"';
    case 0x2013:
    case 0x2014:
      return '-';
    case 0x2026:
      return '.';
    case 0x00A0:
      return ' ';
  }
  return (cp >= kFirstChar && cp < kFirstChar + kCharCount) ? cp : '?';
}

const float kPanelBg[4] = {0.045f, 0.047f, 0.06f, 1.f};
const float kTileBg[4] = {0.13f, 0.14f, 0.17f, 1.f};
const float kAccent[4] = {0.33f, 0.62f, 1.f, 1.f};
const float kWhite[4] = {0.96f, 0.96f, 0.98f, 1.f};
const float kGrey[4] = {0.62f, 0.64f, 0.70f, 1.f};

std::string ReadFile(const std::string& path) {
  std::string out;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
  }
  return out;
}

}  // namespace

bool UiPanel::BakeFont(Font* font, const std::string& path, float size, std::string* error) {
  const std::string ttf = ReadFile(path);
  if (ttf.empty()) {
    *error = "cannot read font " + path;
    return false;
  }
  std::vector<unsigned char> bitmap(static_cast<size_t>(kAtlas) * kAtlas);
  font->baked.assign(sizeof(stbtt_bakedchar) * kCharCount, 0);
  const int rows = stbtt_BakeFontBitmap(
      reinterpret_cast<const unsigned char*>(ttf.data()), 0, size, bitmap.data(), kAtlas, kAtlas, kFirstChar,
      kCharCount, reinterpret_cast<stbtt_bakedchar*>(font->baked.data()));
  if (rows <= 0) {
    *error = "font atlas too small for " + path;
    return false;
  }
  font->size = size;
  glGenTextures(1, &font->texture);
  glBindTexture(GL_TEXTURE_2D, font->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlas, kAtlas, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return true;
}

bool UiPanel::Init(std::string* error) {
  GLuint v = CompileShader(GL_VERTEX_SHADER, kVert), f = CompileShader(GL_FRAGMENT_SHADER, kFrag);
  if (!v || !f) {
    *error = "menu shaders failed to compile";
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, v);
  glAttachShader(program_, f);
  glLinkProgram(program_);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    *error = "menu shaders failed to link";
    return false;
  }
  u_size_ = glGetUniformLocation(program_, "u_size");
  u_mode_ = glGetUniformLocation(program_, "u_mode");
  u_color_ = glGetUniformLocation(program_, "u_color");
  u_tex_ = glGetUniformLocation(program_, "u_tex");

  // Noto Sans ships with SteamOS; the others cover other systems.
  const char* regular[] = {
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/TTF/Hack-Regular.ttf"};
  const char* bold[] = {
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/TTF/Hack-Bold.ttf"};
  std::string regular_path, bold_path;
  for (const char* p : regular)
    if (!ReadFile(p).empty()) {
      regular_path = p;
      break;
    }
  for (const char* p : bold)
    if (!ReadFile(p).empty()) {
      bold_path = p;
      break;
    }
  if (regular_path.empty()) {
    *error = "no usable font found (looked for Noto Sans, DejaVu Sans, Liberation Sans)";
    return false;
  }
  if (bold_path.empty()) bold_path = regular_path;
  if (!BakeFont(&font_title_, bold_path, 56.f, error) || !BakeFont(&font_big_, bold_path, 96.f, error) ||
      !BakeFont(&font_caption_, regular_path, 30.f, error) || !BakeFont(&font_small_, regular_path, 24.f, error)) {
    return false;
  }

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));

  // 4 mip levels let the headset sample the panel down smoothly when it is far away.
  glGenTextures(1, &color_tex_);
  glBindTexture(GL_TEXTURE_2D, color_tex_);
  glTexStorage2D(GL_TEXTURE_2D, 6, GL_RGBA8, static_cast<GLsizei>(kPanelWidth), static_cast<GLsizei>(kPanelHeight));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!complete) {
    *error = "menu framebuffer is incomplete";
    return false;
  }
  return true;
}

void UiPanel::Shutdown() {
  for (auto& kv : images_)
    if (kv.second.texture) glDeleteTextures(1, &kv.second.texture);
  images_.clear();
  for (Font* f : {&font_title_, &font_big_, &font_caption_, &font_small_})
    if (f->texture) glDeleteTextures(1, &f->texture), f->texture = 0;
  if (color_tex_) glDeleteTextures(1, &color_tex_), color_tex_ = 0;
  if (fbo_) glDeleteFramebuffers(1, &fbo_), fbo_ = 0;
  if (vbo_) glDeleteBuffers(1, &vbo_), vbo_ = 0;
  if (vao_) glDeleteVertexArrays(1, &vao_), vao_ = 0;
  if (program_) glDeleteProgram(program_), program_ = 0;
}

void UiPanel::BeginScreen(const std::string& title, bool can_back) {
  title_ = title;
  can_back_ = can_back;
  tiles_.clear();
  status_.clear();
  scroll_ = 0.f;
  hover_ = kNone;
  Invalidate();
}

void UiPanel::AddTile(UiTile tile) { tiles_.push_back(std::move(tile)); }

void UiPanel::EndScreen() {
  scroll_ = ClampScroll(scroll_, tile_count(), grid_);
  // Keep the decoded poster cache bounded: drop images no tile on this screen uses.
  std::unordered_map<std::string, Image> keep;
  for (const UiTile& t : tiles_) {
    auto it = images_.find(t.image);
    if (!t.image.empty() && it != images_.end()) keep.emplace(t.image, it->second), it->second.texture = 0;
  }
  for (auto& kv : images_)
    if (kv.second.texture) glDeleteTextures(1, &kv.second.texture);
  images_ = std::move(keep);
  Invalidate();
}

void UiPanel::SetStatus(const std::string& title, bool can_back, const std::string& message) {
  BeginScreen(title, can_back);
  status_ = message;
  EndScreen();
}

void UiPanel::ShowMessage(const std::string& message) {
  tiles_.clear();
  status_ = message;
  scroll_ = 0.f;
  hover_ = kNone;
  Invalidate();
}

void UiPanel::SetOverlay(const std::string& text) {
  if (text == overlay_) return;
  overlay_ = text;
  Invalidate();
}

int UiPanel::HitTest(float px, float py) const {
  if (can_back_ && BackButtonRect().Contains(px, py)) return kBack;
  const int i = HitTile(px, py, tile_count(), scroll_, grid_);
  return i >= 0 ? i : kNone;
}

void UiPanel::SetHover(int target) {
  if (target == hover_) return;
  hover_ = target;
  Invalidate();
}

void UiPanel::ScrollBy(float pixels) {
  const float next = ClampScroll(scroll_ + pixels, tile_count(), grid_);
  if (next == scroll_) return;
  scroll_ = next;
  Invalidate();
}

float UiPanel::TextWidth(const Font& font, const std::string& text) const {
  const auto* baked = reinterpret_cast<const stbtt_bakedchar*>(font.baked.data());
  float w = 0;
  for (size_t i = 0; i < text.size();) w += baked[Fold(Utf8Next(text, &i)) - kFirstChar].xadvance;
  return w;
}

std::string UiPanel::Fit(const Font& font, const std::string& text, float max_width) const {
  if (TextWidth(font, text) <= max_width) return text;
  const float dots = TextWidth(font, "...");
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    const size_t before = i;
    Utf8Next(text, &i);
    const std::string next = text.substr(0, i);
    if (TextWidth(font, next) + dots > max_width) {
      i = before;
      break;
    }
    out = next;
  }
  return out + "...";
}

void UiPanel::Draw(const float* vertices, int count, int mode, GLuint texture, const float color[4]) {
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count) * 4 * sizeof(float), vertices, GL_STREAM_DRAW);
  glUniform1i(u_mode_, mode);
  glUniform4fv(u_color_, 1, color);
  if (texture) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(u_tex_, 0);
  }
  glDrawArrays(GL_TRIANGLES, 0, count);
}

void UiPanel::Quad(float x, float y, float w, float h, const float color[4]) {
  const float v[] = {x, y, 0, 0, x + w, y, 1, 0, x, y + h, 0, 1, x + w, y, 1, 0, x + w, y + h, 1, 1, x, y + h, 0, 1};
  Draw(v, 6, 0, 0, color);
}

void UiPanel::ImageQuad(GLuint texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
  const float v[] = {x,     y, u0, v0, x + w, y,     u1, v0, x, y + h, u0, v1,
                     x + w, y, u1, v0, x + w, y + h, u1, v1, x, y + h, u0, v1};
  const float opaque[4] = {1, 1, 1, 1};
  Draw(v, 6, 1, texture, opaque);
}

void UiPanel::Text(const Font& font, const std::string& text, float x, float baseline, const float color[4]) {
  if (text.empty()) return;
  const auto* baked = reinterpret_cast<const stbtt_bakedchar*>(font.baked.data());
  std::vector<float> verts;
  verts.reserve(text.size() * 24);
  float cx = x, cy = baseline;
  for (size_t i = 0; i < text.size();) {
    const unsigned cp = Fold(Utf8Next(text, &i));
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(baked, kAtlas, kAtlas, static_cast<int>(cp) - kFirstChar, &cx, &cy, &q, 1);
    const float quad[] = {q.x0, q.y0, q.s0, q.t0, q.x1, q.y0, q.s1, q.t0, q.x0, q.y1, q.s0, q.t1,
                          q.x1, q.y0, q.s1, q.t0, q.x1, q.y1, q.s1, q.t1, q.x0, q.y1, q.s0, q.t1};
    verts.insert(verts.end(), std::begin(quad), std::end(quad));
  }
  Draw(verts.data(), static_cast<int>(verts.size() / 4), 2, font.texture, color);
}

void UiPanel::LoadPendingImages(int budget) {
  // Visible rows first, so the posters you can see appear before those below the fold.
  for (int i = 0; i < tile_count() && budget > 0; ++i) {
    const std::string& path = tiles_[static_cast<size_t>(i)].image;
    if (path.empty() || images_.count(path)) continue;
    const Rect r = TileRect(i, scroll_, grid_);
    if (r.y + r.h < grid_.top - grid_.RowPitch() || r.y > grid_.bottom + grid_.RowPitch()) continue;
    --budget;
    Image img;
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!pixels) {
      img.failed = true;
    } else {
      glGenTextures(1, &img.texture);
      glBindTexture(GL_TEXTURE_2D, img.texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      glGenerateMipmap(GL_TEXTURE_2D);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      stbi_image_free(pixels);
      img.w = w;
      img.h = h;
    }
    images_.emplace(path, img);
    Invalidate();
  }
}

void UiPanel::DrawTile(int index) {
  const UiTile& t = tiles_[static_cast<size_t>(index)];
  const Rect r = TileRect(index, scroll_, grid_);
  if (r.y + r.h < grid_.top || r.y > grid_.bottom) return;
  const bool hot = hover_ == index;

  if (hot) {
    const float pad = 8.f;
    Quad(r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, kAccent);
  }
  Quad(r.x, r.y, r.w, r.h, kTileBg);

  auto it = t.image.empty() ? images_.end() : images_.find(t.image);
  if (it != images_.end() && it->second.texture) {
    // Fill the 2:3 poster slot, cropping the overflow instead of squashing the picture.
    const Image& img = it->second;
    const float slot = grid_.tile_w / grid_.poster_h;
    const float aspect = static_cast<float>(img.w) / static_cast<float>(img.h);
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (aspect > slot) {
      const float keep = slot / aspect;
      u0 = (1 - keep) / 2;
      u1 = u0 + keep;
    } else {
      const float keep = aspect / slot;
      v0 = (1 - keep) / 2;
      v1 = v0 + keep;
    }
    ImageQuad(it->second.texture, r.x, r.y, grid_.tile_w, grid_.poster_h, u0, v0, u1, v1);
  } else {
    // No poster: a tinted block with the title's first letter.
    unsigned hash = 2166136261u;
    for (unsigned char c : t.title) hash = (hash ^ c) * 16777619u;
    const float tint[4] = {
        0.18f + (hash & 0xFF) / 1400.f, 0.2f + ((hash >> 8) & 0xFF) / 1400.f, 0.26f + ((hash >> 16) & 0xFF) / 1400.f,
        1.f};
    Quad(r.x, r.y, grid_.tile_w, grid_.poster_h, tint);
    std::string letter = t.title.empty() ? "?" : t.title.substr(0, 1);
    const float lw = TextWidth(font_big_, letter);
    Text(font_big_, letter, r.x + (grid_.tile_w - lw) / 2, r.y + grid_.poster_h / 2 + 34.f, kWhite);
  }
  Text(
      font_caption_, Fit(font_caption_, t.title, grid_.tile_w - 6.f), r.x + 3.f, r.y + grid_.poster_h + 30.f,
      hot ? kWhite : kWhite);
  Text(font_small_, Fit(font_small_, t.subtitle, grid_.tile_w - 6.f), r.x + 3.f, r.y + grid_.poster_h + 58.f, kGrey);
}

void UiPanel::Render() {
  LoadPendingImages(3);
  if (!dirty_) return;
  dirty_ = false;

  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, static_cast<GLsizei>(kPanelWidth), static_cast<GLsizei>(kPanelHeight));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(kPanelBg[0], kPanelBg[1], kPanelBg[2], 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(program_);
  glUniform2f(u_size_, kPanelWidth, kPanelHeight);

  for (int i = 0; i < tile_count(); ++i) DrawTile(i);

  // Header and footer cover tiles scrolled under them.
  Quad(0, 0, kPanelWidth, grid_.top - 6.f, kPanelBg);
  Quad(0, grid_.bottom, kPanelWidth, kPanelHeight - grid_.bottom, kPanelBg);

  const float title_max = kPanelWidth - 2 * grid_.left - (can_back_ ? 280.f : 0.f);
  Text(font_title_, Fit(font_title_, title_, title_max), grid_.left, 118.f, kWhite);
  if (can_back_) {
    const Rect b = BackButtonRect();
    const bool hot = hover_ == kBack;
    Quad(b.x, b.y, b.w, b.h, hot ? kAccent : kTileBg);
    const std::string label = "< Back";
    Text(font_caption_, label, b.x + (b.w - TextWidth(font_caption_, label)) / 2, b.y + 47.f, kWhite);
  }

  if (tile_count() == 0) {
    const std::string& message = status_.empty() ? std::string("Nothing here") : status_;
    const std::string shown = Fit(font_title_, message, kPanelWidth - 2 * grid_.left);
    Text(font_title_, shown, (kPanelWidth - TextWidth(font_title_, shown)) / 2, kPanelHeight / 2, kGrey);
  }

  const float max_scroll = MaxScroll(tile_count(), grid_);
  if (max_scroll > 0.f) {
    const float track_x = kPanelWidth - 70.f, track_h = grid_.AreaHeight();
    const float thumb_h = std::max(60.f, track_h * track_h / (track_h + max_scroll));
    const float thumb_y = grid_.top + (track_h - thumb_h) * (scroll_ / max_scroll);
    const float track[4] = {1, 1, 1, 0.08f}, thumb[4] = {1, 1, 1, 0.45f};
    Quad(track_x, grid_.top, 8.f, track_h, track);
    Quad(track_x, thumb_y, 8.f, thumb_h, thumb);
  }
  if (!overlay_.empty()) {
    const float dim[4] = {0.f, 0.f, 0.f, 0.72f};
    glEnable(GL_BLEND);
    Quad(0, 0, kPanelWidth, kPanelHeight, dim);
    Text(font_title_, overlay_, (kPanelWidth - TextWidth(font_title_, overlay_)) / 2, kPanelHeight / 2, kWhite);
  }
  const std::string hint = "Point and pull the trigger     Stick: scroll     Menu button: back";
  Text(font_small_, hint, (kPanelWidth - TextWidth(font_small_, hint)) / 2, kPanelHeight - 14.f, kGrey);

  glDisable(GL_BLEND);
  glBindTexture(GL_TEXTURE_2D, color_tex_);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool UiPanel::DumpPng(const std::string& path) {
  Render();
  std::vector<unsigned char> pixels(static_cast<size_t>(kPanelWidth) * static_cast<size_t>(kPanelHeight) * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glReadPixels(
      0, 0, static_cast<GLsizei>(kPanelWidth), static_cast<GLsizei>(kPanelHeight), GL_RGBA, GL_UNSIGNED_BYTE,
      pixels.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return stbi_write_png(
             path.c_str(), static_cast<int>(kPanelWidth), static_cast<int>(kPanelHeight), 4, pixels.data(),
             static_cast<int>(kPanelWidth) * 4) != 0;
}

}  // namespace xr
