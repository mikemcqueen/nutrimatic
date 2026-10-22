#ifndef NUTRIMATIC_GUI_REVIEW_H
#define NUTRIMATIC_GUI_REVIEW_H

#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "input-source.h"
#include "review-kind.h"

// A modal review of the pairs or dictionary words a Column shows, each with a
// checkbox, for one sentence. For pairs, checked records it YES, unchecked
// NO; pairs already classified YES, for the sentence or globally, are shown
// checked and locked, and are left out of what is recorded. For words, each
// row's first field, checked records it NO, removing it from the dictionary,
// and unchecked records nothing; words already reviewed are shown unchecked
// and locked. Space or Enter toggles the focused row. A last row holds SUBMIT
// and CANCEL, each asking for confirmation, NO first; Esc asks to cancel, or
// cancels outright when nothing is checked, and Esc in a confirmation answers
// NO. SUBMIT with nothing to record (every pair row locked, or no word row
// checked) cancels outright. A request to close pgui asks the same as CANCEL
// when anything is checked. A confirmed SUBMIT runs
// `$WF -d "$WFROOT" classify pairs`, or `$WF -d "$WFROOT" remove words` on
// pgui.sN.words, on a worker thread, during which the review is locked. When it fails, its output is shown and the checks are
// kept.
class Review {
 public:
  enum class Result { open, cancelled, submitted, closed };

  // A review of `kind`, pairs or words, of `rows` of `items`, the values
  // beside them taken from `values`, either empty or one per item, for
  // `sentence`. Null, with the error diagnosed, when $WF or $WFROOT is
  // unset, the classified YES pairs or the reviewed words can't be loaded,
  // or an item isn't a pair or a word.
  static std::unique_ptr<Review> open(ReviewKind kind, SharedLines items,
                                      SharedLines values,
                                      std::vector<int> rows, int sentence);

  ReviewKind kind() const { return kind_; }

  // Draws the review with its list over the screen rect from `min` to `max`
  // and its status line just above, in the colors of a focused ListBox.
  Result render(ImVec2 min, ImVec2 max);

  // Asks to close pgui. True when it can close now: nothing is checked, or a
  // SUBMIT is recording the checks. Otherwise asks, as CANCEL does, to discard
  // them, and render() returns closed once that is confirmed.
  bool request_close();

  // Whether a SUBMIT is still recording the checks.
  bool running() const;

 private:
  // A `wf classify pairs` or `wf remove words` run; done is set once ok and
  // output are.
  struct Job {
    std::atomic<bool> done = false;
    bool ok = false;
    std::string output;
  };

  enum class Confirm { none, submit, cancel };

  Review() = default;
  void submit();
  void render_rows();
  // The YES/NO confirmation popup `id`; returns true when YES is chosen.
  bool render_confirm(char const* id, std::string const& question);

  ReviewKind kind_ = ReviewKind::none;
  SharedLines items_;
  SharedLines values_;
  std::vector<int> rows_;
  // For words, each row's word, parsed by open().
  std::vector<std::string> words_;
  int sentence_ = 0;
  std::filesystem::path root_;
  std::string wf_;
  std::vector<char> checked_;
  std::vector<char> locked_;
  int checked_count_ = 0;
  int locked_count_ = 0;
  bool opened_ = false;
  bool focus_first_ = true;
  Confirm confirm_ = Confirm::none;
  bool close_requested_ = false;
  std::string failure_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
};

#endif
