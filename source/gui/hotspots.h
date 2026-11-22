#ifndef NUTRIMATIC_GUI_HOTSPOTS_H
#define NUTRIMATIC_GUI_HOTSPOTS_H

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "input-source.h"

struct LetterToolsParams;

using HotspotSections = std::vector<std::pair<std::string, SharedLines>>;

// The sections of hotspots file `number` at `path`, in file order, each a
// [name] line, named number.name, followed by the nonblank lines under it,
// trimmed. Its [all] section, named number.all, instead holds every distinct
// line of all its sections, in file order. Null when the file can't be read;
// that and lines before its first section are diagnosed.
std::shared_ptr<HotspotSections const> read_hotspots_file(
    std::filesystem::path const& path, size_t number);

// The lines of one section of app_state()'s hotspots (see
// AppState::refresh_hotspots()) whose a-z letters, any other characters
// skipped, fit within the LetterToolsParams' letters less its used letters,
// kept whole, as written, in file order, ignoring its input. Its second row
// is a dropdown of the section names, in file order, naming `section`, which
// falls back to the first section whenever it names none of them, and to
// empty when there are none. The dropdown takes the section's lines from
// app_state() on the main thread and reports an option change when the
// choice or the hotspots version changed. Its Column has no save button.
struct Hotspots {
  static constexpr bool keeps_lines_whole = true;
  static constexpr bool hides_save_button = true;

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_display_options(bool* entered);

  std::string section;
  SharedLines lines = std::make_shared<Lines const>();
  unsigned version = 0;
};

#endif
