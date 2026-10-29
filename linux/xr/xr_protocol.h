#ifndef PLEZY_LINUX_XR_XR_PROTOCOL_H_
#define PLEZY_LINUX_XR_XR_PROTOCOL_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Line protocol between the Flutter app and the plezy-xr helper process.
//
// Commands arrive on the helper's stdin, one per line:
//   play | pause | toggle | quit | recenter | swap
//   load <url>            play this stream (replaces what is playing); preceded by any number of
//   header <Name: value>  HTTP headers for it, and optionally  start <seconds>
//   stop                  stop playback and return to the menu
//   hwdec <auto|software> for the next load: let the hardware decoder try, or decode in software
//   seek <seconds>        absolute
//   seekrel <seconds>     relative, may be negative
//   volume <0-100>
//   env <wire name>
//   layout <wire name>
// Menu screens arrive as tab-separated lines (fields may contain spaces, never tabs or newlines):
//   ui.begin <TAB> title <TAB> canGoBack(0|1)      start a new screen
//   ui.tile  <TAB> id <TAB> title <TAB> subtitle <TAB> imagePath   one poster tile (imagePath may be empty)
//   ui.end                                         show the screen
//   ui.status <TAB> text                           show a message instead of tiles ("Loading...")
//   ui.dump <TAB> path                             debug: write the current menu to a PNG
// Events leave on stdout, one per line:
//   select <TAB> id        a tile was picked
//   back                   the back button / menu button was pressed
//   stopped                playback was stopped from inside the headset (menu button)
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
  kLayout,
  kLoad,
  kHeader,
  kStart,
  kStop,
  kHwdec,
  kUiBegin,
  kUiTile,
  kUiEnd,
  kUiStatus,
  kUiDump
};

struct Command {
  CommandType type = CommandType::kInvalid;
  double number = 0.0;
  std::string text;
  std::vector<std::string> fields;  // ui.* lines: the tab-separated fields after the verb
};

inline std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out;
  size_t from = 0;
  while (true) {
    const size_t tab = line.find('\t', from);
    if (tab == std::string::npos) {
      out.push_back(line.substr(from));
      return out;
    }
    out.push_back(line.substr(from, tab - from));
    from = tab + 1;
  }
}

inline Command ParseCommand(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.compare(0, 3, "ui.") == 0) {
    Command ui;
    std::vector<std::string> parts = SplitTabs(line);
    const std::string ui_verb = parts[0];
    parts.erase(parts.begin());
    if (ui_verb == "ui.begin" && parts.size() >= 2)
      ui.type = CommandType::kUiBegin;
    else if (ui_verb == "ui.tile" && parts.size() >= 4)
      ui.type = CommandType::kUiTile;
    else if (ui_verb == "ui.end")
      ui.type = CommandType::kUiEnd;
    else if (ui_verb == "ui.dump" && !parts.empty())
      ui.type = CommandType::kUiDump;
    else if (ui_verb == "ui.status" && !parts.empty())
      ui.type = CommandType::kUiStatus;
    if (ui.type != CommandType::kInvalid) ui.fields = std::move(parts);
    return ui;
  }
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
  else if (verb == "stop")
    c.type = CommandType::kStop;
  else if (verb == "hwdec" && (arg == "auto" || arg == "software")) {
    c.type = CommandType::kHwdec;
    c.text = arg;
  } else if (verb == "load" && !arg.empty()) {
    c.type = CommandType::kLoad;
    c.text = arg;
  } else if (verb == "header" && !arg.empty()) {
    c.type = CommandType::kHeader;
    c.text = arg;
  } else if (verb == "start")
    number(CommandType::kStart);
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

inline std::string FormatSelect(const std::string& id) { return "select\t" + id; }

// Errors must stay on one line or the app would read the tail as a new event.
inline std::string FormatError(const std::string& message) {
  std::string out = "error ";
  for (char ch : message) out += (ch == '\n' || ch == '\r') ? ' ' : ch;
  return out;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_XR_PROTOCOL_H_
