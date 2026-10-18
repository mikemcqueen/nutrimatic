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
//   bool render_options(bool* entered)
// drawing its option widgets in the current content region, returning true
// when an option changed, and setting *entered when Enter is pressed in one of
// its fields, which set nothing themselves; commands without it have no
// options. And it may have
//   bool render_display_options(bool* entered)
// drawing its second row of option widgets the same way; commands without it
// leave that row empty. And it may have
//   bool apply_fields()
// setting the options its fields name, both rows', from what they hold,
// called when Enter is pressed in any field of its Column, and returning true
// when an option changed; commands without it have no fields. And it may have
//   unsigned display_version() const
//   bool display_hides(std::string const& value) const
// a display filter on its output: display_hides() says whether the item with
// `value` (see Column::output()) is hidden, and display_version() goes up
// each time its answers change; commands without them hide nothing. And it
// may have
//   std::string used_letters() const
// returning the letters it adds to the used letters it runs with, and those
// of the Columns downstream of it; commands without it add none. And it may
// have
//   static constexpr bool has_dictionary_in_src
// true when its Column's source dropdown offers "dict", its D: dictionary;
// commands without it don't. And it may have
//   static constexpr bool reads_pairs
// true when it reads pairs, so choosing it moves its Column's source off a
// dictionary; commands without it don't. And it may have
//   static constexpr bool reads_classified
// true when its output depends on the classified pairs, so it reruns when
// they're recorded (see AppState::classified_version); commands without it
// don't. And it may have
//   static constexpr bool reviewable
// true when its Column's shown pairs can be reviewed (see Review); commands
// without it can't. And it may have
//   static constexpr bool has_columns_in_dictionaries
// true when its Column's D: dropdown also offers the columns before it, and
// choosing it sets D: to the column just before; commands without it don't.
// And it may have
//   static constexpr bool uses_dictionary
// true when run() reads its params' dictionary or dictionary_lines whatever
// its source, so it reruns when that dictionary is reloaded; commands
// without it read them only through a "dict" source. And it may have
//   static constexpr bool uses_big_dictionary
// true when run() reads its params' big_dictionary, so it reruns when
// "big_dict" is reloaded; commands without it don't. And it may have
//   static constexpr bool cancellable
// true when run() checks its params' stop token, so its Column offers an x
// button that cancels it; commands without it aren't. And it may have
//   static constexpr char const* default_seed
// the seed key choosing it sets its Column's source to, when app_state()'s
// seed_map has it; commands without it have none.
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

  bool render_options(bool* entered) {
    return self_->render_options(entered);
  }

  bool render_display_options(bool* entered) {
    return self_->render_display_options(entered);
  }

  bool apply_fields() { return self_->apply_fields(); }

  unsigned display_version() const { return self_->display_version(); }

  bool display_hides(std::string const& value) const {
    return self_->display_hides(value);
  }

  std::string used_letters() const { return self_->used_letters(); }

  bool has_dictionary_in_src() const { return self_->has_dictionary_in_src(); }

  bool reads_pairs() const { return self_->reads_pairs(); }

  bool reads_classified() const { return self_->reads_classified(); }

  bool reviewable() const { return self_->reviewable(); }

  bool cancellable() const { return self_->cancellable(); }

  bool has_columns_in_dictionaries() const {
    return self_->has_columns_in_dictionaries();
  }

  bool uses_dictionary() const { return self_->uses_dictionary(); }

  bool uses_big_dictionary() const { return self_->uses_big_dictionary(); }

  // Empty when it has none.
  std::string default_seed() const { return self_->default_seed(); }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual std::unique_ptr<Concept> copy() const = 0;
    virtual bool run(std::vector<SharedLines> const& inputs,
                     LetterToolsParams const& params, Lines* output) const = 0;
    virtual bool render_options(bool* entered) = 0;
    virtual bool render_display_options(bool* entered) = 0;
    virtual bool apply_fields() = 0;
    virtual unsigned display_version() const = 0;
    virtual bool display_hides(std::string const& value) const = 0;
    virtual std::string used_letters() const = 0;
    virtual bool has_dictionary_in_src() const = 0;
    virtual bool reads_pairs() const = 0;
    virtual bool reads_classified() const = 0;
    virtual bool reviewable() const = 0;
    virtual bool cancellable() const = 0;
    virtual bool has_columns_in_dictionaries() const = 0;
    virtual bool uses_dictionary() const = 0;
    virtual bool uses_big_dictionary() const = 0;
    virtual std::string default_seed() const = 0;
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
    bool render_options(bool* entered) override {
      if constexpr (requires { data.render_options(entered); })
        return data.render_options(entered);
      else
        return false;
    }
    bool render_display_options(bool* entered) override {
      if constexpr (requires { data.render_display_options(entered); })
        return data.render_display_options(entered);
      else
        return false;
    }
    bool apply_fields() override {
      if constexpr (requires { data.apply_fields(); })
        return data.apply_fields();
      else
        return false;
    }
    unsigned display_version() const override {
      if constexpr (requires { data.display_version(); })
        return data.display_version();
      else
        return 0;
    }
    bool display_hides(std::string const& value) const override {
      if constexpr (requires { data.display_hides(value); })
        return data.display_hides(value);
      else
        return false;
    }
    std::string used_letters() const override {
      if constexpr (requires { data.used_letters(); })
        return data.used_letters();
      else
        return std::string();
    }
    bool has_dictionary_in_src() const override {
      if constexpr (requires { T::has_dictionary_in_src; })
        return T::has_dictionary_in_src;
      else
        return false;
    }
    bool reads_pairs() const override {
      if constexpr (requires { T::reads_pairs; })
        return T::reads_pairs;
      else
        return false;
    }
    bool reads_classified() const override {
      if constexpr (requires { T::reads_classified; })
        return T::reads_classified;
      else
        return false;
    }
    bool reviewable() const override {
      if constexpr (requires { T::reviewable; })
        return T::reviewable;
      else
        return false;
    }
    bool cancellable() const override {
      if constexpr (requires { T::cancellable; })
        return T::cancellable;
      else
        return false;
    }
    bool has_columns_in_dictionaries() const override {
      if constexpr (requires { T::has_columns_in_dictionaries; })
        return T::has_columns_in_dictionaries;
      else
        return false;
    }
    bool uses_dictionary() const override {
      if constexpr (requires { T::uses_dictionary; })
        return T::uses_dictionary;
      else
        return false;
    }
    bool uses_big_dictionary() const override {
      if constexpr (requires { T::uses_big_dictionary; })
        return T::uses_big_dictionary;
      else
        return false;
    }
    std::string default_seed() const override {
      if constexpr (requires { T::default_seed; })
        return T::default_seed;
      else
        return std::string();
    }
    T data;
  };

  std::unique_ptr<Concept> self_;
};

// The names every Column's dropdown offers, in order; "" names no command.
inline constexpr std::array<char const*, 5> command_names = {
    "", "pfilter", "freqsort", "pairs", "bag-stats"};

// The command a Column's dropdown names: PairFilter for "pfilter", FreqSort
// for "freqsort", FindPairs for "pairs", BagStats for "bag-stats", none
// otherwise.
std::optional<Command> make_command(std::string const& name);

#endif
