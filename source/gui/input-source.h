#ifndef NUTRIMATIC_GUI_INPUT_SOURCE_H
#define NUTRIMATIC_GUI_INPUT_SOURCE_H

#include <memory>
#include <string>
#include <variant>
#include <vector>

// Index of a Column in pgui's Window.
using ColumnIdentifier = int;

// app_state()'s seed lines as a Column's input source.
struct Seed {
  bool operator==(Seed const&) const = default;
};

// Where a Column's items come from: nothing, app_state()'s seed, or another
// Column's items.
using InputSource = std::variant<std::monostate, Seed, ColumnIdentifier>;

// A list of items, shared unchanged between a Column, the columns reading it,
// and the commands running on it.
using Lines = std::vector<std::string>;
using SharedLines = std::shared_ptr<Lines const>;

// Sets `lines` to the lines of the file at `path`. Returns false, with the
// error diagnosed, when it can't be read.
bool read_file_lines(std::string const& path, Lines* lines);

#endif
