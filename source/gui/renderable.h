#ifndef NUTRIMATIC_GUI_RENDERABLE_H
#define NUTRIMATIC_GUI_RENDERABLE_H

#include <memory>
#include <type_traits>
#include <utility>

// Any value with a render() member, drawn once per frame between
// ImGui::NewFrame() and ImGui::Render(). Holds its value by copy.
class Renderable {
 public:
  template <typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, Renderable>)
  Renderable(T x) : self_(std::make_unique<Model<T>>(std::move(x))) {}

  Renderable(Renderable const& x) : self_(x.self_->copy()) {}
  Renderable(Renderable&&) noexcept = default;
  Renderable& operator=(Renderable x) noexcept {
    self_ = std::move(x.self_);
    return *this;
  }

  void render() { self_->render(); }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual std::unique_ptr<Concept> copy() const = 0;
    virtual void render() = 0;
  };

  template <typename T>
  struct Model final : Concept {
    explicit Model(T x) : data(std::move(x)) {}
    std::unique_ptr<Concept> copy() const override {
      return std::make_unique<Model>(*this);
    }
    void render() override { data.render(); }
    T data;
  };

  std::unique_ptr<Concept> self_;
};

#endif
