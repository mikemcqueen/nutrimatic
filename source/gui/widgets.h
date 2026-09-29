#ifndef NUTRIMATIC_GUI_WIDGETS_H
#define NUTRIMATIC_GUI_WIDGETS_H

#include <cstddef>

// A text field filling the rest of the line but for a square X button to its
// right, which clears it. Returns true when Enter is pressed in the field or
// the button is clicked.
bool clearable_input(char const* id, char* text, size_t size);

#endif
