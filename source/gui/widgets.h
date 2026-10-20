#ifndef NUTRIMATIC_GUI_WIDGETS_H
#define NUTRIMATIC_GUI_WIDGETS_H

#include <cstddef>

// A text field filling the rest of the line, less `reserve`, but for a square
// X button to its right, which clears it. Returns true when Enter is pressed
// in the field or the button is clicked.
bool clearable_input(char const* id, char* text, size_t size,
                     float reserve = 0);

// A square button, a frame high, with `label` centered inside by the extent
// of its glyphs, drawn pressed and outlined while `on`. Returns true when
// clicked. `label` is ASCII and also serves as the button's ID.
bool toggle_button(char const* label, bool on);

// "m" and "x" fields, sized for "0.00", editing min_text and max_text, then a
// square "X" button setting them to default_min and default_max, all greyed
// out unless `enabled`. Sets *entered when Enter is pressed in a field or the
// button is clicked.
void cv_range_input(char* min_text, char* max_text, size_t size,
                    char const* default_min, char const* default_max,
                    bool enabled, bool* entered);

#endif
