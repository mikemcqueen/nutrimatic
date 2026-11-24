#ifndef NUTRIMATIC_GUI_WIDGETS_H
#define NUTRIMATIC_GUI_WIDGETS_H

#include <cstddef>
#include <string_view>

// A text field filling the rest of the line, less `reserve`, but for a square
// X button to its right, which clears it. Returns true when Enter is pressed
// in the field or the button is clicked.
bool clearable_input(std::string_view id, char* text, size_t size,
                     float reserve = 0);

// A button a frame high, square unless `label` needs it wider, with `label`
// centered inside by the extent of its glyphs, drawn pressed and outlined
// while `on`. Returns true when clicked. `label` is ASCII and also serves as
// the button's ID.
bool toggle_button(std::string_view label, bool on);

// A square button, a frame high, with a clockwise circular arrow drawn
// inside. Returns true when clicked. `id` is the button's ID.
bool refresh_button(std::string_view id);

inline constexpr int MIN_FONT_SIZE = 14;
inline constexpr int MAX_FONT_SIZE = 20;
inline constexpr int DEFAULT_FONT_SIZE = 15;

// A "p:" label and a field sized for two digits showing *size while it isn't
// being edited. Enter in the field stores its value in *size and returns true
// when that's from MIN_FONT_SIZE to MAX_FONT_SIZE. `id` is the field's ID.
bool font_size_input(std::string_view id, int* size);

// Writes value into text as a cv bound fitting a field sized for "0.00": two
// decimals below 10, one below 100, none above.
void format_cv_bound(double value, char* text, size_t size);

// "m" and "x" fields, sized for "0.00", editing min_text and max_text, then a
// square "X" button setting them to default_min and default_max, all greyed
// out unless `enabled`. Sets *entered when Enter is pressed in a field or the
// button is clicked.
void cv_range_input(char* min_text, char* max_text, size_t size,
                    std::string_view default_min,
                    std::string_view default_max,
                    bool enabled, bool* entered);

#endif
