#ifndef NUTRIMATIC_GUI_COMMAND_H
#define NUTRIMATIC_GUI_COMMAND_H

#include <array>
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
//            LetterToolsParams const& params, Lines* output) const
// run() sets `output`; it returns false, with the error diagnosed, when the
// call fails. It may run on any thread, reading app_state() only for its
// classified pairs.
// It may also have
//   bool render_options()
// drawing its option widgets in the current content region and returning
// true when an option changed; commands without it have no options. And it
// may have
//   std::string used_letters() const
// returning the letters it adds to the used letters it runs with, and those
// of the Columns downstream of it; commands without it add none. And it may
// have
//   static constexpr bool reads_dictionaries
// true when its Column's source dropdown offers "dict", its D: dictionary;
// commands without it don't. And it may have
//   static constexpr bool reads_pairs
// true when it reads pairs, so choosing it moves its Column's source off a
// dictionary; commands without it don't.
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
           LetterToolsParams const& params, Lines* output) const {
    return self_->run(inputs, params, output);
  }

  bool render_options() { return self_->render_options(); }

  std::string used_letters() const { return self_->used_letters(); }

  bool reads_dictionaries() const { return self_->reads_dictionaries(); }

  bool reads_pairs() const { return self_->reads_pairs(); }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual std::unique_ptr<Concept> copy() const = 0;
    virtual bool run(std::vector<SharedLines> const& inputs,
                     LetterToolsParams const& params, Lines* output) const = 0;
    virtual bool render_options() = 0;
    virtual std::string used_letters() const = 0;
    virtual bool reads_dictionaries() const = 0;
    virtual bool reads_pairs() const = 0;
  };

  template <typename T>
  struct Model final : Concept {
    explicit Model(T x) : data(std::move(x)) {}
    std::unique_ptr<Concept> copy() const override {
      return std::make_unique<Model>(*this);
    }
    bool run(std::vector<SharedLines> const& inputs,
             LetterToolsParams const& params, Lines* output) const override {
      return data.run(inputs, params, output);
    }
    bool render_options() override {
      if constexpr (requires { data.render_options(); })
        return data.render_options();
      else
        return false;
    }
    std::string used_letters() const override {
      if constexpr (requires { data.used_letters(); })
        return data.used_letters();
      else
        return std::string();
    }
    bool reads_dictionaries() const override {
      if constexpr (requires { T::reads_dictionaries; })
        return T::reads_dictionaries;
      else
        return false;
    }
    bool reads_pairs() const override {
      if constexpr (requires { T::reads_pairs; })
        return T::reads_pairs;
      else
        return false;
    }
    T data;
  };

  std::unique_ptr<Concept> self_;
};

// The names every Column's dropdown offers, in order; "" names no command.
inline constexpr std::array<char const*, 4> command_names = {
    "", "pfilter", "freqsort", "pairs"};

// The command a Column's dropdown names: PairFilter for "pfilter", FreqSort
// for "freqsort", FindPairs for "pairs", none otherwise.
std::optional<Command> make_command(std::string const& name);

#endif
