#ifndef NUTRIMATIC_GUI_WIDGETS_H
#define NUTRIMATIC_GUI_WIDGETS_H

#include <cstddef>

// A text field filling the rest of the line but for a square X button to its
// right, which clears it. Returns true when Enter is pressed in the field or
// the button is clicked.
bool clearable_input(char const* id, char* text, size_t size);

// A square button, a frame high, with `label` centered inside by the extent
// of its glyphs, drawn pressed and outlined while `on`. Returns true when
// clicked. `label` is ASCII and also serves as the button's ID.
bool toggle_button(char const* label, bool on);

#endif
