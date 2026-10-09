#ifndef NUTRIMATIC_GUI_REVIEW_H
#define NUTRIMATIC_GUI_REVIEW_H

#include <imgui.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "input-source.h"

// A modal review of the pairs a Column shows, each with a checkbox: checked
// records it YES, unchecked NO, for one sentence. Pairs already classified
// YES, for the sentence or globally, are shown checked and locked, and are
// left out of what is recorded. Space or Enter toggles the focused pair. A
// last row holds SUBMIT and CANCEL, each asking for confirmation, NO first;
// Esc asks to cancel, or cancels outright when nothing is checked, and Esc
// in a confirmation answers NO. A confirmed SUBMIT runs `$WF -d "$WFROOT"
// classify pairs` on a worker thread, during which the review is locked. When
// it fails, its output is shown and the checks are kept.
class Review {
 public:
  enum class Result { open, cancelled, submitted };

  // A review of `rows` of `items`, the values beside them taken from
  // `values`, either empty or one per item, for `sentence`. Null, with the
  // error diagnosed, when $WFROOT is unset, the classified YES pairs can't be
  // loaded, or an item isn't a pair.
  static std::unique_ptr<Review> open(SharedLines items, SharedLines values,
                                      std::vector<int> rows, int sentence);

  // Draws the review with its list over the screen rect from `min` to `max`
  // and its status line just above, in the colors of a focused ListBox.
  Result render(ImVec2 min, ImVec2 max);

 private:
  // A `wf classify pairs` run; done is set once ok and output are.
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

  SharedLines items_;
  SharedLines values_;
  std::vector<int> rows_;
  int sentence_ = 0;
  std::string root_;
  std::string wf_;
  std::vector<char> checked_;
  std::vector<char> locked_;
  int checked_count_ = 0;
  int locked_count_ = 0;
  bool opened_ = false;
  bool focus_first_ = true;
  Confirm confirm_ = Confirm::none;
  std::string failure_;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
};

#endif
