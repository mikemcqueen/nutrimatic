#ifndef NUTRIMATIC_GUI_RENDERABLE_H
#define NUTRIMATIC_GUI_RENDERABLE_H

// Something drawn once per frame, between ImGui::NewFrame() and
// ImGui::Render().
class Renderable {
 public:
  virtual ~Renderable() = default;
  virtual void render() = 0;
};

#endif
