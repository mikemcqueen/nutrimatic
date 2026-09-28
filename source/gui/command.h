#ifndef NUTRIMATIC_GUI_COMMAND_H
#define NUTRIMATIC_GUI_COMMAND_H

#include <memory>
#include <optional>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

#include "app-state.h"
#include "input-source.h"

// A tool call: any value with
//   bool run(std::vector<SharedLines> const& inputs,
//            GlobalSettings const& settings, Lines* output) const
// run() sets `output`; it returns false, with the error diagnosed, when the
// call fails. It may run on any thread, reading app_state() only for its
// dictionary.
// Holds its value by copy.
class Command {
 public:
  template <typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, Command>)
  Command(T x) : self_(std::make_unique<Model<T>>(std::move(x))) {}

  Command(Command const& x) : self_(x.self_->copy()) {}
  Command(Command&&) noexcept = default;
  Command& operator=(Command x) noexcept {
    self_ = std::move(x.self_);
    return *this;
  }

  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const {
    return self_->run(inputs, settings, output);
  }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual std::unique_ptr<Concept> copy() const = 0;
    virtual bool run(std::vector<SharedLines> const& inputs,
                     GlobalSettings const& settings, Lines* output) const = 0;
  };

  template <typename T>
  struct Model final : Concept {
    explicit Model(T x) : data(std::move(x)) {}
    std::unique_ptr<Concept> copy() const override {
      return std::make_unique<Model>(*this);
    }
    bool run(std::vector<SharedLines> const& inputs,
             GlobalSettings const& settings, Lines* output) const override {
      return data.run(inputs, settings, output);
    }
    T data;
  };

  std::unique_ptr<Concept> self_;
};

// The command a Column's dropdown names: PairFilter for "pfilter", FreqSort
// for "freqsort", none otherwise.
std::optional<Command> make_command(std::string const& name);

#endif
