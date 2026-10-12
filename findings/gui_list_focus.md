# Keyboard Focus in pgui List Boxes

## Question

Does pgui have keyboard focus? Could the arrow keys move the selection in the
list box of the column that was clicked most recently?

## Answer

Yes. pgui uses Dear ImGui, which already has keyboard focus and keyboard
navigation built in. pgui just hasn't turned navigation on, so nothing needs
bolting on. Not implemented yet.

## What's already there

- ImGui tracks a focused window and a "nav id", which is the item that has
  keyboard focus. Clicking an item already makes it the nav id. So "the list
  box you last clicked has focus" already happens; arrow keys just don't do
  anything yet.
- Arrow-key navigation is controlled by `ImGuiConfigFlags_NavEnableKeyboard`.
  `source/gui/pgui.cpp` doesn't set it, so it's off.
- `ImGuiListClipper`, which `ListBox::render()` uses, already handles
  navigation. It keeps the focused row rendered even when it's scrolled out of
  the clipped range, scrolls it into view, and makes PageUp/PageDown work.
- The installed ImGui (1.92.8) has `ImGuiSelectableFlags_SelectOnNav`, which
  makes a Selectable select itself when the arrow keys move onto it. That gives
  "the selection follows the arrow keys".

## Minimal change

```cpp
// pgui.cpp, after context creation
ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

// list-box.cpp, in row()
if (ImGui::Selectable(items[i].c_str(), selected_ == i, ImGuiSelectableFlags_SelectOnNav))
```

The existing toggle logic still works. Moving onto an item that isn't selected
selects it, and pressing Space or Enter on the selected item deselects it, the
same as clicking it again.

## Things to watch for

- **Navigation isn't limited to the list.** Up at the top of a list moves
  focus to the widgets above it (filter box, command options). Left and right
  move into the neighbouring column's list. With `SelectOnNav`, landing in that
  list would select whatever item focus lands on and start that column's
  command. If that's unwanted, keep arrow keys inside each list by handling
  Up/Down directly: check `ImGui::IsWindowFocused()` and
  `ImGui::IsKeyPressed(ImGuiKey_UpArrow)` inside the list and change
  `selected_` directly. That's about 10 lines and gives full control.
- **Holding an arrow key reruns things on every step.** Each move changes
  `selected_`, which bumps `version_` in `Column`, so the chained columns run
  again on each step. Holding the key down could queue a lot of
  pfilter/freqsort runs. A debounce, or committing the selection only when the
  key is released, would limit that.
- **Text fields take the keys when focused.** While the filter `InputText` has
  focus, keystrokes go to it (`io.WantTextInput`). That's normally what's
  wanted.
- **There's a visible focus highlight.** ImGui draws a nav highlight rectangle
  around the focused item. Its style can be changed, or it can be hidden.

## Suggested order

Start with the two-line version to see how it feels. Add the in-list key
handling if focus jumping between columns gets annoying.

## Alt-Tab back into the window loses the focused item

When pgui regains focus while Alt is still held, the Alt release reaches
ImGui as a lone Alt tap, which toggles the nav "menu layer"
(`NavUpdateWindowing()` in imgui.cpp). A ListBox is a child window with no
menu bar, so ImGui clears the active id and moves focus to the parent
window, where the nav item is the list box's child window as a whole. Space
then re-enters it. ImGui guards the Alt release *after* losing focus
(`AppFocusLost` clears keys), but not a press that arrives after regaining
it. It happens in or out of a Review.

Fix: `Window::render()` calls `SetKeyOwner(ImGuiMod_Alt, ...)` every frame.
The toggle is cancelled when Alt has an owner. pgui has no menu bars and no
Alt shortcuts, so nothing else depends on the toggle.
