#ifndef PLEZY_LINUX_XR_XR_PROTOCOL_H_
#define PLEZY_LINUX_XR_XR_PROTOCOL_H_

#include <cstdio>
#include <cstdlib>
#include <string>

// Line protocol between the Flutter app and the plezy-xr helper process.
//
// Commands arrive on the helper's stdin, one per line:
//   play | pause | toggle | quit | recenter | swap
//   seek <seconds>        absolute
//   seekrel <seconds>     relative, may be negative
//   volume <0-100>
//   env <wire name>
//   layout <wire name>
// Events leave on stdout, one per line:
//   ready
//   position <seconds> <duration> <paused 0|1> <env> <layout> <hwdec> <dropped>
//     hwdec is mpv's hwdec-current ("no" = software decoding); dropped is the
//     count of frames mpv dropped while decoding or displaying. Older readers
//     ignore the trailing fields.
//   ended
//   error <message>

namespace xr {

enum class CommandType {
  kInvalid,
  kPlay,
  kPause,
  kToggle,
  kQuit,
  kRecenter,
  kSwapEyes,
  kSeek,
  kSeekRelative,
  kVolume,
  kEnvironment,
  kLayout
};

struct Command {
  CommandType type = CommandType::kInvalid;
  double number = 0.0;
  std::string text;
};

inline Command ParseCommand(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  const size_t space = line.find(' ');
  const std::string verb = line.substr(0, space);
  const std::string arg = space == std::string::npos ? "" : line.substr(space + 1);
  Command c;
  auto number = [&](CommandType type) {
    char* end = nullptr;
    const double v = std::strtod(arg.c_str(), &end);
    if (arg.empty() || end == arg.c_str() || !(v == v)) return;  // empty / NaN: invalid
    c.type = type;
    c.number = v;
  };
  if (verb == "play")
    c.type = CommandType::kPlay;
  else if (verb == "pause")
    c.type = CommandType::kPause;
  else if (verb == "toggle")
    c.type = CommandType::kToggle;
  else if (verb == "quit")
    c.type = CommandType::kQuit;
  else if (verb == "recenter")
    c.type = CommandType::kRecenter;
  else if (verb == "swap")
    c.type = CommandType::kSwapEyes;
  else if (verb == "seek")
    number(CommandType::kSeek);
  else if (verb == "seekrel")
    number(CommandType::kSeekRelative);
  else if (verb == "volume") {
    number(CommandType::kVolume);
    if (c.type == CommandType::kVolume) c.number = c.number < 0 ? 0 : c.number > 100 ? 100 : c.number;
  } else if (verb == "env" && !arg.empty()) {
    c.type = CommandType::kEnvironment;
    c.text = arg;
  } else if (verb == "layout" && !arg.empty()) {
    c.type = CommandType::kLayout;
    c.text = arg;
  }
  return c;
}

inline std::string FormatPosition(
    double position, double duration, bool paused, const std::string& env, const std::string& layout,
    const std::string& hwdec = "no", long long dropped = 0) {
  char buf[320];
  std::snprintf(
      buf, sizeof buf, "position %.3f %.3f %d %s %s %s %lld", position, duration, paused ? 1 : 0, env.c_str(),
      layout.c_str(), hwdec.empty() ? "no" : hwdec.c_str(), dropped);
  return buf;
}

// Errors must stay on one line or the app would read the tail as a new event.
inline std::string FormatError(const std::string& message) {
  std::string out = "error ";
  for (char ch : message) out += (ch == '\n' || ch == '\r') ? ' ' : ch;
  return out;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_XR_PROTOCOL_H_
