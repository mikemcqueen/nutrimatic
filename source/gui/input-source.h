#ifndef NUTRIMATIC_GUI_INPUT_SOURCE_H
#define NUTRIMATIC_GUI_INPUT_SOURCE_H

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Index of a Column in pgui's Window.
using ColumnIdentifier = int;

// The lines of the seed file under `key` in app_state()'s seed_map as a
// Column's input source.
struct Seed {
  std::string key;
  bool operator==(Seed const&) const = default;
};

// The words of the dictionary a Column's D: dropdown names as its input
// source.
struct Dict {
  bool operator==(Dict const&) const = default;
};

// Where a Column's items come from: nothing, one of app_state()'s seed files,
// the Column's dictionary, or another Column's items.
using InputSource =
    std::variant<std::monostate, Seed, Dict, ColumnIdentifier>;

// Where a Column's used letters come from: another Column, or none ("all"),
// taking app_state()'s used letters from its letters.
using LetterSource = std::optional<ColumnIdentifier>;

// A list of items, shared unchanged between a Column, the columns reading it,
// and the commands running on it.
using Lines = std::vector<std::string>;
using SharedLines = std::shared_ptr<Lines const>;

// Sets `lines` to the lines of the file at `path`. Returns false, with the
// error diagnosed, when it can't be read.
bool read_file_lines(std::string const& path, Lines* lines);

#endif
