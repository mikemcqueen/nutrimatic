#ifndef NUTRIMATIC_GUI_WIDGETS_H
#define NUTRIMATIC_GUI_WIDGETS_H

#include <cstddef>
#include <string_view>

// A text field filling the rest of the line, less `reserve`, but for a square
// X button to its right, which clears it. Returns true when Enter is pressed
// in the field or the button is clicked.
bool clearable_input(std::string_view id, char* text, size_t size,
                     float reserve = 0);

// A square button, a frame high, with `label` centered inside by the extent
// of its glyphs, drawn pressed and outlined while `on`. Returns true when
// clicked. `label` is ASCII and also serves as the button's ID.
bool toggle_button(std::string_view label, bool on);

// A square button, a frame high, with a clockwise circular arrow drawn
// inside. Returns true when clicked. `id` is the button's ID.
bool refresh_button(std::string_view id);

// "m" and "x" fields, sized for "0.00", editing min_text and max_text, then a
// square "X" button setting them to default_min and default_max, all greyed
// out unless `enabled`. Sets *entered when Enter is pressed in a field or the
// button is clicked.
void cv_range_input(char* min_text, char* max_text, size_t size,
                    std::string_view default_min,
                    std::string_view default_max,
                    bool enabled, bool* entered);

#endif
