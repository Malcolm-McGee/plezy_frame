// plezy-xr: the VR cinema helper for Steam Frame (and any OpenXR runtime).
//
// A separate process rather than a mode of the Flutter runner: the runner's
// video plane is a Wayland subsurface tied to GTK, whereas an OpenXR session
// needs its own EGL context and frame loop. The app starts this helper with a
// resolved stream URL and drives it over stdin/stdout (see xr_protocol.h).
//
// One thread owns everything GL, OpenXR and mpv-render related. A second thread
// only reads stdin.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "environment.h"
#include "screen_geometry.h"
#include "stereo_layout.h"
#include "xr_math.h"
#include "xr_protocol.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string url;
  std::vector<std::string> headers;
  double start_seconds = 0.0;
  std::string environment = "cinema";
  std::string layout = "none";
  bool swap_eyes = false;
  int volume = -1;
  std::string title;
};

void Emit(const std::string& line) {
  std::cout << line << '\n';
  std::cout.flush();
}

void Fail(const std::string& message) {
  Emit(xr::FormatError(message));
  Emit("exit");
}

// ---------------------------------------------------------------- commands --

class CommandQueue {
 public:
  void Push(xr::Command c) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(c));
  }
  bool Pop(xr::Command* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    *out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

 private:
  std::mutex mutex_;
  std::deque<xr::Command> queue_;
};

// ----------------------------------------------------------------- shaders --

const char* kVersion = "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n";

const char* kEnvVert = R"(
out vec2 v_ndc;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_ndc = p * 2.0 - 1.0;
  gl_Position = vec4(v_ndc, 1.0, 1.0);
})";

// Sky gradient, optional stars, a floor plane lit by the screen's average
// colour. Everything is authored in gamma space; u_linear converts at the end
// when the swapchain is sRGB.
const char* kEnvFrag = R"(
in vec2 v_ndc;
uniform mat4 u_inv_view_proj;
uniform vec3 u_sky_top;
uniform vec3 u_sky_horizon;
uniform vec3 u_floor;
uniform float u_has_floor;
uniform float u_floor_y;
uniform float u_bleed;
uniform float u_stars;
uniform vec3 u_screen_pos;
uniform vec3 u_ambient;
uniform float u_linear;
uniform float u_alpha;
out vec4 o_color;

float hash(vec3 p) {
  p = fract(p * 0.3183099 + 0.1);
  p *= 17.0;
  return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main() {
  vec4 n = u_inv_view_proj * vec4(v_ndc, 1.0, 1.0);
  vec4 f = u_inv_view_proj * vec4(v_ndc, -1.0, 1.0);
  vec3 origin = f.xyz / f.w;
  vec3 dir = normalize(n.xyz / n.w - origin);
  vec3 col = mix(u_sky_horizon, u_sky_top, clamp(dir.y * 1.5, 0.0, 1.0));
  if (u_stars > 0.0) {
    vec3 cell = floor(dir * 140.0);
    float s = hash(cell);
    col += step(0.9965, s) * (0.4 + 0.6 * hash(cell + 7.0)) * u_stars;
  }
  if (u_has_floor > 0.5 && dir.y < -0.001) {
    float t = (u_floor_y - origin.y) / dir.y;
    if (t > 0.0) {
      vec3 hit = origin + dir * t;
      float dist = length(hit - u_screen_pos);
      vec3 lit = u_floor + u_ambient * u_bleed * (1.8 / (1.0 + 0.12 * dist * dist));
      col = mix(lit, u_sky_horizon, clamp(t / 40.0, 0.0, 1.0));
    }
  }
  if (u_linear > 0.5) col = pow(col, vec3(2.2));
  o_color = vec4(col, u_alpha);
})";

const char* kScreenVert = R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
  v_uv = a_uv;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
})";

const char* kScreenFrag = R"(
in vec2 v_uv;
uniform sampler2D u_video;
uniform vec4 u_eye_rect;
uniform float u_linear;
out vec4 o_color;
void main() {
  vec2 uv = mix(u_eye_rect.xy, u_eye_rect.zw, v_uv);
  vec3 c = textureLod(u_video, uv, 0.0).rgb;
  if (u_linear > 0.5) c = pow(c, vec3(2.2));
  o_color = vec4(c, 1.0);
})";

const char* kBarVert = R"(
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); })";

const char* kBarFrag = R"(
uniform vec4 u_color;
uniform float u_linear;
out vec4 o_color;
void main() {
  vec3 c = u_color.rgb;
  if (u_linear > 0.5) c = pow(c, vec3(2.2));
  o_color = vec4(c, u_color.a);
})";

GLuint Compile(GLenum type, const std::string& body) {
  const std::string src = std::string(kVersion) + body;
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "plezy-xr: shader error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint Link(const char* vert, const char* frag) {
  GLuint v = Compile(GL_VERTEX_SHADER, vert), f = Compile(GL_FRAGMENT_SHADER, frag);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// --------------------------------------------------------------------- EGL --

struct Egl {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;

  bool Init(std::string* error) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
      *error = "EGL: could not initialise a display";
      return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      *error = "EGL: OpenGL ES is unavailable";
      return false;
    }
    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(display, cfg_attr, &config, 1, &n) || n < 1) {
      *error = "EGL: no OpenGL ES 3 config";
      return false;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attr);
    if (context == EGL_NO_CONTEXT) {
      *error = "EGL: could not create an OpenGL ES 3 context";
      return false;
    }
    const EGLint pb_attr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, pb_attr);
    if (!eglMakeCurrent(display, surface, surface, context)) {
      *error = "EGL: could not make the context current";
      return false;
    }
    return true;
  }

  void Shutdown() {
    if (display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);
    display = EGL_NO_DISPLAY;
  }
};

void* GetProcAddress(void*, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

// --------------------------------------------------------------------- mpv --

class VideoPlayer {
 public:
  ~VideoPlayer() { Shutdown(); }

  bool Init(const Options& opt, std::string* error) {
    mpv_ = mpv_create();
    if (!mpv_) {
      *error = "mpv: could not create a player";
      return false;
    }
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");
    mpv_set_option_string(mpv_, "keep-open", "no");
    mpv_set_option_string(mpv_, "audio-client-name", "Plezy VR");
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "osc", "no");
    if (opt.start_seconds > 0.5) {
      char start[32];
      std::snprintf(start, sizeof start, "%.3f", opt.start_seconds);
      mpv_set_option_string(mpv_, "start", start);
    }
    if (opt.volume >= 0) {
      char vol[16];
      std::snprintf(vol, sizeof vol, "%d", opt.volume);
      mpv_set_option_string(mpv_, "volume", vol);
    }
    if (!opt.headers.empty()) {
      std::vector<mpv_node> items(opt.headers.size());
      for (size_t i = 0; i < opt.headers.size(); ++i) {
        items[i].format = MPV_FORMAT_STRING;
        items[i].u.string = const_cast<char*>(opt.headers[i].c_str());
      }
      mpv_node_list list{static_cast<int>(items.size()), items.data(), nullptr};
      mpv_node node{};
      node.format = MPV_FORMAT_NODE_ARRAY;
      node.u.list = &list;
      mpv_set_property(mpv_, "http-header-fields", MPV_FORMAT_NODE, &node);
    }
    if (mpv_initialize(mpv_) < 0) {
      *error = "mpv: initialisation failed";
      return false;
    }
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "video-params/w", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 0, "video-params/h", MPV_FORMAT_INT64);

    mpv_opengl_init_params gl_init{GetProcAddress, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&render_, mpv_, params) < 0) {
      *error = "mpv: could not create an OpenGL render context";
      return false;
    }
    mpv_render_context_set_update_callback(
        render_, [](void* ctx) { static_cast<std::atomic<bool>*>(ctx)->store(true); }, &frame_ready_);

    const char* cmd[] = {"loadfile", opt.url.c_str(), nullptr};
    if (mpv_command(mpv_, cmd) < 0) {
      *error = "mpv: could not queue the stream";
      return false;
    }
    return true;
  }

  void Shutdown() {
    if (render_) {
      mpv_render_context_set_update_callback(render_, nullptr, nullptr);
      mpv_render_context_free(render_);
      render_ = nullptr;
    }
    if (mpv_) {
      mpv_terminate_destroy(mpv_);
      mpv_ = nullptr;
    }
    if (fbo_) glDeleteFramebuffers(1, &fbo_), fbo_ = 0;
    if (tex_) glDeleteTextures(1, &tex_), tex_ = 0;
  }

  // Drain mpv events; returns true once playback has finished.
  bool PumpEvents(std::string* error) {
    bool ended = false;
    while (mpv_) {
      mpv_event* e = mpv_wait_event(mpv_, 0);
      if (e->event_id == MPV_EVENT_NONE) break;
      if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        auto* p = static_cast<mpv_event_property*>(e->data);
        if (p->format == MPV_FORMAT_DOUBLE && p->data) {
          if (!std::strcmp(p->name, "duration")) duration_ = *static_cast<double*>(p->data);
          if (!std::strcmp(p->name, "time-pos")) position_ = *static_cast<double*>(p->data);
        } else if (p->format == MPV_FORMAT_FLAG && p->data && !std::strcmp(p->name, "pause")) {
          paused_ = *static_cast<int*>(p->data) != 0;
        } else if (p->format == MPV_FORMAT_INT64 && p->data) {
          if (!std::strcmp(p->name, "video-params/w")) video_w_ = static_cast<int>(*static_cast<int64_t*>(p->data));
          if (!std::strcmp(p->name, "video-params/h")) video_h_ = static_cast<int>(*static_cast<int64_t*>(p->data));
        }
      } else if (e->event_id == MPV_EVENT_END_FILE) {
        auto* ef = static_cast<mpv_event_end_file*>(e->data);
        if (ef->reason == MPV_END_FILE_REASON_ERROR) *error = std::string("mpv: ") + mpv_error_string(ef->error);
        ended = true;
      } else if (e->event_id == MPV_EVENT_SHUTDOWN) {
        ended = true;
      }
    }
    return ended;
  }

  // Render a new frame into the shared video texture if mpv has one.
  void UpdateTexture() {
    if (!render_ || video_w_ < 1 || video_h_ < 1) return;
    const bool resized = video_w_ != tex_w_ || video_h_ != tex_h_;
    if (resized) {
      if (!tex_) glGenTextures(1, &tex_);
      glBindTexture(GL_TEXTURE_2D, tex_);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, video_w_, video_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (!fbo_) glGenFramebuffers(1, &fbo_);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
      tex_w_ = video_w_;
      tex_h_ = video_h_;
    }
    if (!resized && !frame_ready_.exchange(false)) return;
    frame_ready_.store(false);
    mpv_opengl_fbo fbo{static_cast<int>(fbo_), tex_w_, tex_h_, GL_RGBA8};
    // flip_y = 0: row 0 of the texture is the top of the picture, matching the
    // v = 0 at the top convention the screen mesh samples with.
    int flip_y = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(render_, params);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The ambient light samples the smallest mip as the screen's mean colour.
    glBindTexture(GL_TEXTURE_2D, tex_);
  }

  void Apply(const xr::Command& c) {
    if (!mpv_) return;
    switch (c.type) {
      case xr::CommandType::kPlay:
        SetPause(false);
        break;
      case xr::CommandType::kPause:
        SetPause(true);
        break;
      case xr::CommandType::kToggle:
        SetPause(!paused_);
        break;
      case xr::CommandType::kSeek:
        Seek(c.number, "absolute");
        break;
      case xr::CommandType::kSeekRelative:
        Seek(c.number, "relative");
        break;
      case xr::CommandType::kVolume: {
        double v = c.number;
        mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
        break;
      }
      default:
        break;
    }
  }

  void SetPause(bool pause) {
    int flag = pause ? 1 : 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &flag);
    paused_ = pause;
  }
  void Seek(double seconds, const char* mode) {
    char amount[32];
    std::snprintf(amount, sizeof amount, "%.3f", seconds);
    const char* cmd[] = {"seek", amount, mode, nullptr};
    mpv_command_async(mpv_, 0, cmd);
  }

  GLuint texture() const { return tex_; }
  bool has_frame() const { return tex_ != 0 && tex_w_ > 0; }
  int width() const { return tex_w_; }
  int height() const { return tex_h_; }
  double position() const { return position_; }
  double duration() const { return duration_; }
  bool paused() const { return paused_; }

 private:
  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  std::atomic<bool> frame_ready_{false};
  GLuint tex_ = 0, fbo_ = 0;
  int tex_w_ = 0, tex_h_ = 0, video_w_ = 0, video_h_ = 0;
  double position_ = 0, duration_ = 0;
  bool paused_ = false;
};

// ----------------------------------------------------------------- OpenXR --

#define XR_TRY(call)                                                                 \
  do {                                                                               \
    XrResult _r = (call);                                                            \
    if (XR_FAILED(_r)) {                                                             \
      char _s[XR_MAX_RESULT_STRING_SIZE] = {};                                       \
      if (instance_) xrResultToString(instance_, _r, _s);                            \
      *error = std::string(#call) + " failed: " + (_s[0] ? _s : std::to_string(_r)); \
      return false;                                                                  \
    }                                                                                \
  } while (0)

struct EyeSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  int32_t width = 0, height = 0;
  std::vector<XrSwapchainImageOpenGLESKHR> images;
  std::vector<GLuint> fbos;
};

class XrApp {
 public:
  XrApp(const Options& opt, CommandQueue* commands, VideoPlayer* video, Egl* egl)
      : opt_(opt), commands_(commands), video_(video), egl_(egl) {
    env_ = &xr::EnvironmentByWireName(opt.environment);
    layout_ = xr::StereoLayoutFromWireName(opt.layout);
    swap_eyes_ = opt.swap_eyes;
  }

  bool Init(std::string* error) {
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    auto has = [&](const char* n) {
      return std::any_of(
          exts.begin(), exts.end(), [&](const XrExtensionProperties& e) { return !std::strcmp(e.extensionName, n); });
    };
    if (!has(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
      *error = "the OpenXR runtime does not support OpenGL ES (XR_KHR_opengl_es_enable)";
      return false;
    }
    if (!has(XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
      *error = "the OpenXR runtime does not support EGL (XR_MNDX_egl_enable)";
      return false;
    }
    std::vector<const char*> enabled = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_MNDX_EGL_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ici.applicationInfo.applicationName, "Plezy VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ici.applicationInfo.engineName, "plezy-xr", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();
    XR_TRY(xrCreateInstance(&ici, &instance_));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_TRY(xrGetSystem(instance_, &sgi, &system_));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = nullptr;
    XR_TRY(xrGetInstanceProcAddr(
        instance_, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs)));
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_TRY(get_reqs(instance_, system_, &reqs));

    XrGraphicsBindingEGLMNDX binding{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress = eglGetProcAddress;
    binding.display = egl_->display;
    binding.config = egl_->config;
    binding.context = egl_->context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system_;
    XR_TRY(xrCreateSession(instance_, &sci, &session_));

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    XR_TRY(xrCreateReferenceSpace(session_, &rsci, &local_space_));
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_TRY(xrCreateReferenceSpace(session_, &rsci, &view_space_));

    if (!InitBlendModes(error)) return false;
    if (!InitSwapchains(error)) return false;
    if (!InitActions(error)) return false;
    if (!InitGl(error)) return false;
    return true;
  }

  // Runs until the session ends or the app asks to quit. Returns false on error.
  bool Run(std::string* error) {
    auto last_report = Clock::now();
    while (!exit_) {
      std::string video_error;
      if (video_->PumpEvents(&video_error)) {
        if (!video_error.empty()) {
          *error = video_error;
          return false;
        }
        Emit("ended");
        break;
      }
      xr::Command c;
      while (commands_->Pop(&c)) HandleCommand(c);
      PollEvents();
      if (session_running_) {
        if (!Frame(error)) return false;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (Clock::now() - last_report > std::chrono::milliseconds(500)) {
        last_report = Clock::now();
        Emit(xr::FormatPosition(
            video_->position(), video_->duration(), video_->paused(), env_->wire_name, WireLayout()));
      }
    }
    return true;
  }

  void Shutdown() {
    if (!instance_) return;
    for (auto& eye : eyes_) {
      for (GLuint f : eye.fbos) glDeleteFramebuffers(1, &f);
      if (eye.handle) xrDestroySwapchain(eye.handle);
    }
    if (actions_.set) xrDestroyActionSet(actions_.set);
    if (view_space_) xrDestroySpace(view_space_);
    if (local_space_) xrDestroySpace(local_space_);
    if (session_) xrDestroySession(session_);
    xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
  }

 private:
  struct Actions {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction toggle = XR_NULL_HANDLE, cycle_env = XR_NULL_HANDLE, cycle_layout = XR_NULL_HANDLE,
             recenter = XR_NULL_HANDLE, quit = XR_NULL_HANDLE, stick = XR_NULL_HANDLE;
  };

  bool InitBlendModes(std::string* error) {
    uint32_t n = 0;
    XR_TRY(xrEnumerateEnvironmentBlendModes(
        instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    std::vector<XrEnvironmentBlendMode> modes(n);
    XR_TRY(xrEnumerateEnvironmentBlendModes(
        instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data()));
    for (auto m : modes) {
      if (m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) opaque_ok_ = true;
      if (m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) alpha_ok_ = true;
      if (m == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE) additive_ok_ = true;
    }
    return true;
  }

  bool InitSwapchains(std::string* error) {
    uint32_t n = 0;
    XR_TRY(xrEnumerateViewConfigurationViews(
        instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_TRY(xrEnumerateViewConfigurationViews(
        instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, views.data()));
    if (n != 2) {
      *error = "expected a stereo (two view) display";
      return false;
    }
    uint32_t fc = 0;
    XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &fc, nullptr));
    std::vector<int64_t> formats(fc);
    XR_TRY(xrEnumerateSwapchainFormats(session_, fc, &fc, formats.data()));
    int64_t chosen = 0;
    for (int64_t pref : {static_cast<int64_t>(GL_SRGB8_ALPHA8), static_cast<int64_t>(GL_RGBA8)}) {
      if (std::find(formats.begin(), formats.end(), pref) != formats.end()) {
        chosen = pref;
        break;
      }
    }
    if (!chosen) {
      *error = "the runtime offers no RGBA8 swapchain format";
      return false;
    }
    srgb_ = chosen == GL_SRGB8_ALPHA8;

    for (uint32_t i = 0; i < 2; ++i) {
      EyeSwapchain& eye = eyes_[i];
      eye.width = static_cast<int32_t>(views[i].recommendedImageRectWidth);
      eye.height = static_cast<int32_t>(views[i].recommendedImageRectHeight);
      XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      sc.format = chosen;
      sc.sampleCount = 1;
      sc.width = static_cast<uint32_t>(eye.width);
      sc.height = static_cast<uint32_t>(eye.height);
      sc.faceCount = 1;
      sc.arraySize = 1;
      sc.mipCount = 1;
      XR_TRY(xrCreateSwapchain(session_, &sc, &eye.handle));
      uint32_t ic = 0;
      XR_TRY(xrEnumerateSwapchainImages(eye.handle, 0, &ic, nullptr));
      eye.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
      XR_TRY(xrEnumerateSwapchainImages(
          eye.handle, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())));
      eye.fbos.assign(ic, 0);
      glGenFramebuffers(static_cast<GLsizei>(ic), eye.fbos.data());
    }
    return true;
  }

  XrAction MakeAction(const char* name, XrActionType type) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    std::strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ai.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction a = XR_NULL_HANDLE;
    xrCreateAction(actions_.set, &ai, &a);
    return a;
  }

  XrPath Path(const char* p) {
    XrPath out = XR_NULL_PATH;
    xrStringToPath(instance_, p, &out);
    return out;
  }

  bool InitActions(std::string* error) {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "cinema");
    std::strcpy(si.localizedActionSetName, "Cinema");
    XR_TRY(xrCreateActionSet(instance_, &si, &actions_.set));
    actions_.toggle = MakeAction("toggle_play", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actions_.cycle_env = MakeAction("cycle_environment", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actions_.cycle_layout = MakeAction("cycle_3d_mode", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actions_.recenter = MakeAction("recenter", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actions_.quit = MakeAction("exit_cinema", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actions_.stick = MakeAction("seek_volume", XR_ACTION_TYPE_VECTOR2F_INPUT);

    // Suggest bindings for the common profiles; the runtime remaps for others
    // (Steam Frame controllers are rebindable from the SteamVR binding UI).
    auto suggest = [&](const char* profile, std::vector<std::pair<XrAction, const char*>> list) {
      std::vector<XrActionSuggestedBinding> b;
      for (auto& [action, path] : list) b.push_back({action, Path(path)});
      XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
      s.interactionProfile = Path(profile);
      s.suggestedBindings = b.data();
      s.countSuggestedBindings = static_cast<uint32_t>(b.size());
      xrSuggestInteractionProfileBindings(instance_, &s);
    };
    suggest(
        "/interaction_profiles/khr/simple_controller", {{actions_.toggle, "/user/hand/right/input/select/click"},
                                                        {actions_.cycle_env, "/user/hand/left/input/select/click"},
                                                        {actions_.quit, "/user/hand/right/input/menu/click"}});
    suggest(
        "/interaction_profiles/oculus/touch_controller", {{actions_.toggle, "/user/hand/right/input/trigger/value"},
                                                          {actions_.cycle_env, "/user/hand/right/input/b/click"},
                                                          {actions_.cycle_layout, "/user/hand/right/input/a/click"},
                                                          {actions_.recenter, "/user/hand/left/input/thumbstick/click"},
                                                          {actions_.quit, "/user/hand/left/input/menu/click"},
                                                          {actions_.stick, "/user/hand/right/input/thumbstick"}});
    suggest(
        "/interaction_profiles/valve/index_controller", {{actions_.toggle, "/user/hand/right/input/trigger/value"},
                                                         {actions_.cycle_env, "/user/hand/right/input/b/click"},
                                                         {actions_.cycle_layout, "/user/hand/right/input/a/click"},
                                                         {actions_.recenter, "/user/hand/left/input/thumbstick/click"},
                                                         {actions_.quit, "/user/hand/left/input/b/click"},
                                                         {actions_.stick, "/user/hand/right/input/thumbstick"}});

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &actions_.set;
    XR_TRY(xrAttachSessionActionSets(session_, &ai));
    return true;
  }

  bool InitGl(std::string* error) {
    env_prog_ = Link(kEnvVert, kEnvFrag);
    screen_prog_ = Link(kScreenVert, kScreenFrag);
    bar_prog_ = Link(kBarVert, kBarFrag);
    if (!env_prog_ || !screen_prog_ || !bar_prog_) {
      *error = "could not compile the renderer shaders";
      return false;
    }
    glGenVertexArrays(1, &vao_empty_);
    glGenVertexArrays(1, &vao_screen_);
    glGenBuffers(1, &vbo_screen_);
    glGenVertexArrays(1, &vao_bar_);
    glGenBuffers(1, &vbo_bar_);
    ApplyEnvironmentDefaults();
    return true;
  }

  // ---- state changes ----

  void ApplyEnvironmentDefaults() {
    screen_.distance_m = env_->screen_distance;
    screen_.width_m = env_->screen_width;
    screen_.curvature = env_->screen_curvature;
    screen_dirty_ = true;
  }

  const char* WireLayout() const {
    switch (layout_) {
      case xr::StereoLayout::kSideBySideHalf:
        return "sideBySideHalf";
      case xr::StereoLayout::kSideBySideFull:
        return "sideBySideFull";
      case xr::StereoLayout::kTopBottomHalf:
        return "topBottomHalf";
      case xr::StereoLayout::kTopBottomFull:
        return "topBottomFull";
      default:
        return "none";
    }
  }

  void SetEnvironment(const xr::EnvironmentSpec* spec) {
    env_ = spec;
    ApplyEnvironmentDefaults();
  }

  void CycleEnvironment() {
    const size_t n = xr::kEnvironments.size();
    for (size_t step = 1; step <= n; ++step) {
      const auto& next = xr::kEnvironments[(static_cast<size_t>(env_->id) + step) % n];
      if (next.passthrough && !(alpha_ok_ || additive_ok_)) continue;  // not available on this display
      SetEnvironment(&next);
      return;
    }
  }

  void CycleLayout() {
    static const xr::StereoLayout order[] = {
        xr::StereoLayout::kNone, xr::StereoLayout::kSideBySideHalf, xr::StereoLayout::kSideBySideFull,
        xr::StereoLayout::kTopBottomHalf, xr::StereoLayout::kTopBottomFull};
    size_t i = 0;
    while (i < 5 && order[i] != layout_) ++i;
    layout_ = order[(i + 1) % 5];
    screen_dirty_ = true;
  }

  void HandleCommand(const xr::Command& c) {
    switch (c.type) {
      case xr::CommandType::kQuit:
        exit_ = true;
        break;
      case xr::CommandType::kRecenter:
        recenter_pending_ = true;
        break;
      case xr::CommandType::kSwapEyes:
        swap_eyes_ = !swap_eyes_;
        break;
      case xr::CommandType::kEnvironment:
        SetEnvironment(&xr::EnvironmentByWireName(c.text));
        break;
      case xr::CommandType::kLayout:
        layout_ = xr::StereoLayoutFromWireName(c.text);
        screen_dirty_ = true;
        break;
      default:
        video_->Apply(c);
        break;
    }
  }

  // ---- session ----

  void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        state_ = s->state;
        if (s->state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) {
            session_running_ = true;
            Emit("ready");
          }
        } else if (s->state == XR_SESSION_STATE_STOPPING) {
          xrEndSession(session_);
          session_running_ = false;
        } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
          exit_ = true;
        }
      } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        exit_ = true;
      }
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    // Keep the picture paused while the headset is off the user's head.
    const bool visible = state_ == XR_SESSION_STATE_VISIBLE || state_ == XR_SESSION_STATE_FOCUSED;
    if (session_running_ && !visible && !auto_paused_ && !video_->paused()) {
      video_->SetPause(true);
      auto_paused_ = true;
    } else if (visible && auto_paused_) {
      video_->SetPause(false);
      auto_paused_ = false;
    }
  }

  bool BoolEdge(XrAction action) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &st))) return false;
    return st.isActive && st.currentState && st.changedSinceLastSync;
  }

  void PollInput() {
    if (state_ != XR_SESSION_STATE_FOCUSED) return;
    XrActiveActionSet active{actions_.set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &si))) return;
    bool touched = false;
    if (BoolEdge(actions_.toggle)) video_->SetPause(!video_->paused()), touched = true;
    if (BoolEdge(actions_.cycle_env)) CycleEnvironment(), touched = true;
    if (BoolEdge(actions_.cycle_layout)) CycleLayout(), touched = true;
    if (BoolEdge(actions_.recenter)) recenter_pending_ = true;
    if (BoolEdge(actions_.quit)) exit_ = true;

    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = actions_.stick;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
      const auto now = Clock::now();
      if (std::fabs(v.currentState.x) > 0.7f && now - last_seek_ > std::chrono::milliseconds(450)) {
        last_seek_ = now;
        video_->Seek(v.currentState.x > 0 ? 10 : -10, "relative");
        touched = true;
      }
      if (std::fabs(v.currentState.y) > 0.7f && now - last_volume_ > std::chrono::milliseconds(150)) {
        last_volume_ = now;
        volume_ = std::min(100.0, std::max(0.0, volume_ + (v.currentState.y > 0 ? 5.0 : -5.0)));
        video_->Apply({xr::CommandType::kVolume, volume_, {}});
        touched = true;
      }
    }
    if (touched) hud_until_ = Clock::now() + std::chrono::seconds(4);
  }

  // ---- frame ----

  bool Frame(std::string* error) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_TRY(xrWaitFrame(session_, nullptr, &fs));
    XR_TRY(xrBeginFrame(session_, nullptr));
    PollInput();

    video_->UpdateTexture();

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView proj_views[2]{};
    if (fs.shouldRender) {
      XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
      li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      li.displayTime = fs.predictedDisplayTime;
      li.space = local_space_;
      XrViewState vs{XR_TYPE_VIEW_STATE};
      XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
      uint32_t vc = 0;
      XR_TRY(xrLocateViews(session_, &li, &vs, 2, &vc, views));
      if (recenter_pending_ || !centered_) Recenter(fs.predictedDisplayTime);
      if (vc == 2 && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        for (uint32_t i = 0; i < 2; ++i) {
          if (!RenderEye(i, views[i], &proj_views[i], error)) return false;
        }
        layer.space = local_space_;
        layer.viewCount = 2;
        layer.views = proj_views;
        if (env_->passthrough && (alpha_ok_ || additive_ok_) && alpha_ok_) {
          layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        }
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
      }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = BlendMode();
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    XR_TRY(xrEndFrame(session_, &ei));
    return true;
  }

  XrEnvironmentBlendMode BlendMode() const {
    if (env_->passthrough) {
      if (alpha_ok_) return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
      if (additive_ok_) return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    }
    return opaque_ok_ ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE
                      : (alpha_ok_ ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
  }

  // Put the screen in front of where the user is looking and level with their
  // eyes; the floor sits 1.6 m below.
  void Recenter(XrTime time) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(view_space_, local_space_, time, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const auto& q = loc.pose.orientation;
    // Yaw of the forward (-Z) vector.
    const float fx = -2.f * (q.x * q.z + q.y * q.w);
    const float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    anchor_yaw_ = std::atan2(-fx, -fz);
    anchor_pos_[0] = loc.pose.position.x;
    anchor_pos_[1] = loc.pose.position.y;
    anchor_pos_[2] = loc.pose.position.z;
    centered_ = true;
    recenter_pending_ = false;
  }

  void UploadScreen() {
    const float aspect = xr::EyeDisplayAspect(layout_, video_->width(), video_->height());
    xr::ScreenParams p = screen_;
    p.aspect = aspect;
    auto strip = xr::BuildScreenStrip(p);
    screen_vertex_count_ = static_cast<GLsizei>(strip.size());
    glBindVertexArray(vao_screen_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_screen_);
    glBufferData(
        GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(strip.size() * sizeof(xr::ScreenVertex)), strip.data(),
        GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(xr::ScreenVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, sizeof(xr::ScreenVertex), reinterpret_cast<void*>(3 * sizeof(float)));
    screen_height_ = xr::ScreenHeight(p);
    screen_dirty_ = false;
  }

  void SetUniformMat4(GLuint prog, const char* name, const xr::Mat4& m) {
    glUniformMatrix4fv(glGetUniformLocation(prog, name), 1, GL_FALSE, m.data());
  }

  bool RenderEye(uint32_t index, const XrView& view, XrCompositionLayerProjectionView* out, std::string* error) {
    EyeSwapchain& eye = eyes_[index];
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_TRY(xrAcquireSwapchainImage(eye.handle, &ai, &image));
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XR_TRY(xrWaitSwapchainImage(eye.handle, &wi));

    glBindFramebuffer(GL_FRAMEBUFFER, eye.fbos[image]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[image].image, 0);
    glViewport(0, 0, eye.width, eye.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const bool passthrough = env_->passthrough && (alpha_ok_ || additive_ok_);
    glClearColor(0.f, 0.f, 0.f, passthrough ? 0.f : 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto& o = view.pose.orientation;
    const auto& p = view.pose.position;
    // Everything is drawn in the anchored frame: undo the recenter yaw/offset.
    const xr::Mat4 anchor =
        xr::Multiply(xr::Translation(anchor_pos_[0], anchor_pos_[1], anchor_pos_[2]), xr::RotationY(anchor_yaw_));
    const xr::Mat4 view_m = xr::InverseRigid(xr::PoseMatrix(o.x, o.y, o.z, o.w, p.x, p.y, p.z));
    const xr::Mat4 proj = xr::PerspectiveFromFov(
        view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown, 0.05f, 200.f);
    const xr::Mat4 view_proj = xr::Multiply(proj, view_m);
    // Model maps screen-local space (viewer at origin) into tracking space.
    const xr::Mat4 model = anchor;
    const xr::Mat4 mvp = xr::Multiply(view_proj, model);

    if (screen_dirty_ && video_->has_frame()) UploadScreen();

    const float lin = srgb_ ? 1.f : 0.f;
    if (!passthrough) {
      glUseProgram(env_prog_);
      SetUniformMat4(env_prog_, "u_inv_view_proj", xr::Inverse(view_proj));
      glUniform3f(glGetUniformLocation(env_prog_, "u_sky_top"), env_->sky_top.r, env_->sky_top.g, env_->sky_top.b);
      glUniform3f(
          glGetUniformLocation(env_prog_, "u_sky_horizon"), env_->sky_horizon.r, env_->sky_horizon.g,
          env_->sky_horizon.b);
      glUniform3f(glGetUniformLocation(env_prog_, "u_floor"), env_->floor.r, env_->floor.g, env_->floor.b);
      glUniform1f(glGetUniformLocation(env_prog_, "u_has_floor"), env_->has_floor ? 1.f : 0.f);
      glUniform1f(glGetUniformLocation(env_prog_, "u_floor_y"), anchor_pos_[1] - 1.6f);
      glUniform1f(glGetUniformLocation(env_prog_, "u_bleed"), env_->ambient_bleed);
      glUniform1f(glGetUniformLocation(env_prog_, "u_stars"), env_->id == xr::EnvironmentId::kSpace ? 1.f : 0.f);
      // Screen centre in tracking space for the floor light falloff.
      glUniform3f(
          glGetUniformLocation(env_prog_, "u_screen_pos"), anchor_pos_[0] + std::sin(anchor_yaw_) * -screen_.distance_m,
          anchor_pos_[1], anchor_pos_[2] - std::cos(anchor_yaw_) * screen_.distance_m);
      glUniform3f(glGetUniformLocation(env_prog_, "u_ambient"), ambient_[0], ambient_[1], ambient_[2]);
      glUniform1f(glGetUniformLocation(env_prog_, "u_linear"), lin);
      glUniform1f(glGetUniformLocation(env_prog_, "u_alpha"), 1.f);
      glBindVertexArray(vao_empty_);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    if (video_->has_frame() && screen_vertex_count_ > 0) {
      glUseProgram(screen_prog_);
      SetUniformMat4(screen_prog_, "u_mvp", mvp);
      const xr::UvRect r = xr::EyeUvRect(layout_, index == 0 ? xr::Eye::kLeft : xr::Eye::kRight, swap_eyes_);
      glUniform4f(glGetUniformLocation(screen_prog_, "u_eye_rect"), r.u0, r.v0, r.u1, r.v1);
      glUniform1f(glGetUniformLocation(screen_prog_, "u_linear"), lin);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, video_->texture());
      glUniform1i(glGetUniformLocation(screen_prog_, "u_video"), 0);
      glBindVertexArray(vao_screen_);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, screen_vertex_count_);
    }

    DrawHud(mvp, lin);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_TRY(xrReleaseSwapchainImage(eye.handle, &ri));

    out->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    out->pose = view.pose;
    out->fov = view.fov;
    out->subImage.swapchain = eye.handle;
    out->subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
    out->subImage.imageArrayIndex = 0;
    return true;
  }

  // A thin progress bar under the screen, shown for a few seconds after input
  // and whenever playback is paused. Drawn in screen space coordinates.
  void DrawHud(const xr::Mat4& mvp, float lin) {
    const bool show = video_->paused() || Clock::now() < hud_until_;
    if (!show || screen_vertex_count_ == 0) return;
    const double dur = video_->duration();
    const float frac = dur > 0 ? static_cast<float>(std::min(1.0, std::max(0.0, video_->position() / dur))) : 0.f;
    const float half_w = std::min(screen_.width_m, 8.f) * 0.4f;
    const float y = screen_.height_offset_m - screen_height_ * 0.5f - 0.18f;
    const float z = -screen_.distance_m + 0.02f;
    const float h = 0.03f;
    auto quad = [&](float x0, float x1) {
      const float v[] = {x0, y + h, z, x0, y - h, z, x1, y + h, z, x1, y - h, z};
      glBindVertexArray(vao_bar_);
      glBindBuffer(GL_ARRAY_BUFFER, vbo_bar_);
      glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };
    glUseProgram(bar_prog_);
    SetUniformMat4(bar_prog_, "u_mvp", mvp);
    glUniform1f(glGetUniformLocation(bar_prog_, "u_linear"), lin);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform4f(glGetUniformLocation(bar_prog_, "u_color"), 1.f, 1.f, 1.f, 0.25f);
    quad(-half_w, half_w);
    glUniform4f(glGetUniformLocation(bar_prog_, "u_color"), 1.f, 1.f, 1.f, 0.9f);
    quad(-half_w, -half_w + 2.f * half_w * frac);
    glDisable(GL_BLEND);
  }

  Options opt_;
  CommandQueue* commands_;
  VideoPlayer* video_;
  Egl* egl_;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace local_space_ = XR_NULL_HANDLE, view_space_ = XR_NULL_HANDLE;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  EyeSwapchain eyes_[2];
  Actions actions_;
  bool session_running_ = false, exit_ = false, srgb_ = false;
  bool opaque_ok_ = false, alpha_ok_ = false, additive_ok_ = false;
  bool centered_ = false, recenter_pending_ = false, auto_paused_ = false;

  const xr::EnvironmentSpec* env_;
  xr::StereoLayout layout_;
  bool swap_eyes_ = false;
  xr::ScreenParams screen_;
  float screen_height_ = 3.f;
  bool screen_dirty_ = true;
  GLsizei screen_vertex_count_ = 0;
  float anchor_yaw_ = 0.f, anchor_pos_[3] = {0, 0, 0};
  float ambient_[3] = {0.35f, 0.33f, 0.30f};
  double volume_ = 100.0;
  Clock::time_point hud_until_{}, last_seek_{}, last_volume_{};

  GLuint env_prog_ = 0, screen_prog_ = 0, bar_prog_ = 0;
  GLuint vao_empty_ = 0, vao_screen_ = 0, vbo_screen_ = 0, vao_bar_ = 0, vbo_bar_ = 0;
};

bool ParseArgs(int argc, char** argv, Options* o, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--url" && next(&v))
      o->url = v;
    else if (a == "--header" && next(&v))
      o->headers.push_back(v);
    else if (a == "--start" && next(&v))
      o->start_seconds = std::strtod(v.c_str(), nullptr);
    else if (a == "--env" && next(&v))
      o->environment = v;
    else if (a == "--layout" && next(&v))
      o->layout = v;
    else if (a == "--swap-eyes")
      o->swap_eyes = true;
    else if (a == "--volume" && next(&v))
      o->volume = std::atoi(v.c_str());
    else if (a == "--title" && next(&v))
      o->title = v;
    else {
      *error = "unrecognised argument: " + a;
      return false;
    }
  }
  if (o->url.empty()) {
    *error = "--url is required";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string error;
  if (!ParseArgs(argc, argv, &opt, &error)) {
    Fail(error);
    return 2;
  }

  CommandQueue commands;
  std::thread stdin_thread([&commands] {
    std::string line;
    while (std::getline(std::cin, line)) {
      xr::Command c = xr::ParseCommand(line);
      if (c.type != xr::CommandType::kInvalid) commands.Push(std::move(c));
    }
    // The app closed our stdin (it exited or crashed): leave VR cleanly.
    commands.Push({xr::CommandType::kQuit, 0, {}});
  });
  stdin_thread.detach();

  Egl egl;
  if (!egl.Init(&error)) {
    Fail(error);
    return 1;
  }
  int code = 0;
  {
    VideoPlayer video;
    XrApp app(opt, &commands, &video, &egl);
    if (!app.Init(&error) || !video.Init(opt, &error) || !app.Run(&error)) {
      if (!error.empty()) Fail(error);
      code = 1;
    }
    app.Shutdown();
    video.Shutdown();
  }
  egl.Shutdown();
  if (code == 0) Emit("exit");
  return code;
}
