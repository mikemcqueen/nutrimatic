#include "coherence-measure.h"
#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

static void check(bool condition, char const* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void write_text(std::string const& path, char const* text) {
  FILE* fp = fopen(path.c_str(), "wb");
  check(fp != NULL, "could not create input fixture");
  check(fputs(text, fp) >= 0, "could not write input fixture");
  check(fclose(fp) == 0, "could not close input fixture");
}

static void write_index(std::string const& path) {
  FILE* fp = fopen(path.c_str(), "w+b");
  check(fp != NULL, "could not create index fixture");
  {
    IndexWriter writer(fp);
    writer.next("aa ", 0, 100);
    writer.next("aa bb ", 0, 80);
    writer.next("bb ", 0, 100);
    writer.next("bb cc ", 0, 70);
    writer.next("cc ", 0, 100);
    writer.next("dd ", 0, 50);
    writer.next("ee ", 0, 50);
    writer.next("ff ", 0, 50);
    writer.next("gg ", 0, 1);
    writer.next("gg hh ", 0, 1);
    writer.next("hh ", 0, 1);
    writer.next("ii ", 0, 4);
    writer.next("ii jj ", 0, 4);
    writer.next("jj ", 0, 4);
    writer.next("zz ", 0, 1000000);
    writer.next(NULL, 0, 0);
  }
  check(fclose(fp) == 0, "could not close index fixture");
}

static std::string read_stream(FILE* fp) {
  fflush(fp);
  check(fseek(fp, 0, SEEK_END) == 0, "could not size captured stream");
  long const length = ftell(fp);
  check(length >= 0, "could not measure captured stream");
  check(fseek(fp, 0, SEEK_SET) == 0, "could not rewind captured stream");
  std::string text(size_t(length), '\0');
  if (length != 0)
    check(fread(&text[0], 1, size_t(length), fp) == size_t(length),
          "could not read captured stream");
  return text;
}

struct RunResult {
  int status;
  std::string output;
  std::string error;
};

static RunResult run(
    std::vector<std::string> arguments,
    CoherenceResourceLimits limits = {
      COHERENCE_MEMORY_LIMIT,
      COHERENCE_ORDER_RELAXATION_LIMIT,
    }) {
  std::vector<char*> argv;
  for (size_t i = 0; i < arguments.size(); ++i)
    argv.push_back(&arguments[i][0]);
  argv.push_back(NULL);

  FILE* captured_out = tmpfile();
  FILE* captured_err = tmpfile();
  check(captured_out != NULL && captured_err != NULL,
        "could not create captured streams");
  fflush(NULL);
  int const saved_out = dup(STDOUT_FILENO);
  int const saved_err = dup(STDERR_FILENO);
  check(saved_out >= 0 && saved_err >= 0, "could not save output streams");
  check(dup2(fileno(captured_out), STDOUT_FILENO) >= 0,
        "could not capture stdout");
  check(dup2(fileno(captured_err), STDERR_FILENO) >= 0,
        "could not capture stderr");
  int const status = run_measure_coherence(
      int(arguments.size()), argv.data(), limits);
  fflush(NULL);
  check(dup2(saved_out, STDOUT_FILENO) >= 0, "could not restore stdout");
  check(dup2(saved_err, STDERR_FILENO) >= 0, "could not restore stderr");
  close(saved_out);
  close(saved_err);

  RunResult result = {
    status,
    read_stream(captured_out),
    read_stream(captured_err),
  };
  fclose(captured_out);
  fclose(captured_err);
  return result;
}

static bool contains(std::string const& text, char const* fragment) {
  return text.find(fragment) != std::string::npos;
}

static size_t count_occurrences(
    std::string const& text, char const* fragment) {
  size_t count = 0;
  size_t position = 0;
  while ((position = text.find(fragment, position)) != std::string::npos) {
    ++count;
    position += strlen(fragment);
  }
  return count;
}

static std::vector<std::string> base_args(
    std::string const& index, std::string const& input,
    char const* format, char const* order) {
  return {
    "measure-coherence", index, input,
    "--format", format, "--order", order,
  };
}

int main() {
  char directory_template[] = "/tmp/test-measure-coherence.XXXXXX";
  char* directory = mkdtemp(directory_template);
  check(directory != NULL, "could not create fixture directory");
  std::string const root(directory);
  std::string const index = root + "/test.index";
  std::string const text = root + "/input.txt";
  std::string const dfs = root + "/input.dfs";
  write_index(index);

  RunResult long_help = run({"measure-coherence", "--help"});
  RunResult short_help = run({"measure-coherence", "-h"});
  RunResult question_help = run({"measure-coherence", "-?"});
  check(long_help.status == 0 && short_help.status == 0 &&
            question_help.status == 0,
        "all help spellings succeed");
  check(long_help.output == short_help.output &&
            long_help.output == question_help.output,
        "all help spellings show the same screen");
  check(contains(long_help.output, "Required options:") &&
            contains(long_help.output, "Expected pair count") &&
            !contains(long_help.output, "--mu") &&
            !contains(long_help.output, "--calibrate") &&
            !contains(long_help.output, "--missing-count") &&
            contains(long_help.output,
                     "Answer-table columns:") &&
            contains(long_help.output, "Exit status:"),
        "help screen documents usage, options, output, and statuses");
  RunResult unknown_option = run({"measure-coherence", "-x"});
  check(unknown_option.status == 2,
        "unknown short option is not mistaken for question-mark help");
  RunResult removed_mu = run({"measure-coherence", "--mu", "1000"});
  check(removed_mu.status == 2, "removed smoothing option is rejected");

  write_text(text, "cc bb aa\naa bb\naa\n");
  RunResult printed = run(base_args(index, text, "text", "printed"));
  check(printed.status == 0, "text format parses");
  check(contains(printed.output, "evidence") &&
            !contains(printed.output, "association") &&
            !contains(printed.output, "pair %"),
        "answer table reports evidence without old association calibration");
  check(contains(printed.output, " cc bb aa"),
        "printed order is preserved");
  size_t const one_word_position = printed.output.rfind(" aa ");
  size_t const one_word_begin =
      printed.output.rfind('\n', one_word_position - 1);
  size_t const one_word_end = printed.output.find('\n', one_word_position);
  check(one_word_position != std::string::npos &&
            one_word_begin != std::string::npos &&
            one_word_end != std::string::npos &&
            count_occurrences(
                printed.output.substr(
                    one_word_begin + 1, one_word_end - one_word_begin),
                "nan") == 3,
        "one-word coherence is undefined");
  size_t const printed_first = printed.output.find(" cc bb aa");
  size_t const printed_second = printed.output.find(" aa bb");
  check(printed_first != std::string::npos &&
            printed_second != std::string::npos &&
            printed_first < printed_second,
        "input sorting preserves row order");

  write_text(dfs, "0.00001234 cc bb aa\n5 aa bb\n2 aa\n");
  RunResult dfs_result = run(base_args(index, dfs, "dfs", "printed"));
  check(dfs_result.status == 0 &&
            contains(dfs_result.output, "1.234e-05"),
        "DFS format retains a small legacy score's magnitude");

  std::vector<std::string> stdin_args = base_args(index, "-", "text", "printed");
  RunResult stdin_result = run(stdin_args);
  check(stdin_result.status == 2, "stdin path is rejected");

  write_text(text, "aa  bb\n");
  RunResult bad_text = run(base_args(index, text, "text", "printed"));
  check(bad_text.status == 1, "canonical input is rejected");

  write_text(text, "aa aa bb\n");
  RunResult duplicate = run(base_args(index, text, "text", "best"));
  check(duplicate.status == 0 &&
            contains(duplicate.output, " aa bb aa"),
        "best evidence ordering handles duplicate words");

  write_text(text, "cc bb aa\naa bb\n");
  std::vector<std::string> sorted_args =
      base_args(index, text, "text", "printed");
  sorted_args.push_back("--sort");
  sorted_args.push_back("coherence");
  RunResult sorted = run(sorted_args);
  check(sorted.status == 0 && contains(sorted.output, "evidence"),
        "coherence sorting uses direct evidence");

  write_text(text, "aa bb\nbb aa\n");
  std::vector<std::string> pair_args =
      base_args(index, text, "text", "printed");
  pair_args.push_back("--pairs");
  RunResult pair_details = run(pair_args);
  check(pair_details.status == 0, "--pairs succeeds");

  write_text(text, "cc bb aa\nff ee dd\naa\n");
  std::vector<std::string> best_args = base_args(index, text, "text", "best");
  best_args.push_back("--sort");
  best_args.push_back("coherence");
  RunResult best = run(best_args);
  RunResult best_again = run(best_args);
  check(best.status == 0 && contains(best.output, " aa bb cc"),
        "exact ordering selects the known directed path");
  check(contains(best.output, " dd ee ff"),
        "exact ordering ties use final and predecessor occurrence indexes");
  check(best.output == best_again.output,
        "coherence sorting is deterministic");
  size_t const one_word = best.output.rfind(" aa ");
  size_t const directed = best.output.find(" aa bb cc");
  check(one_word != std::string::npos && directed != std::string::npos &&
            one_word > directed,
        "undefined one-word rows sort after defined rows");

  write_text(text, "aa bb dd\naa bb cc\n");
  std::vector<std::string> weakest_args =
      base_args(index, text, "text", "printed");
  weakest_args.push_back("--sort");
  weakest_args.push_back("coherence");
  RunResult weakest_first = run(weakest_args);
  size_t const complete_chain = weakest_first.output.find(" aa bb cc");
  size_t const broken_chain = weakest_first.output.find(" aa bb dd");
  check(weakest_first.status == 0 && complete_chain != std::string::npos &&
            broken_chain != std::string::npos &&
            complete_chain < broken_chain,
        "weakest boundary outranks an isolated strong pair");

  RunResult memory_failure = run(
      base_args(index, text, "text", "printed"), {16, 1000});
  check(memory_failure.status == 1,
        "injected memory limit fails deterministically");

  write_text(text, "aa bb\n");
  RunResult work_failure = run(
      base_args(index, text, "text", "best"),
      {COHERENCE_MEMORY_LIMIT, 1});
  check(work_failure.status == 1,
        "injected ordering-work limit fails deterministically");

  std::vector<std::string> repeated =
      base_args(index, text, "text", "printed");
  repeated.push_back("--order");
  repeated.push_back("best");
  check(run(repeated).status == 2, "repeated singleton option is rejected");

  std::vector<std::string> negative_top =
      base_args(index, text, "text", "printed");
  negative_top.push_back("--top");
  negative_top.push_back("-1");
  check(run(negative_top).status == 2, "negative top count is rejected");

  unlink(index.c_str());
  unlink(text.c_str());
  unlink(dfs.c_str());
  rmdir(root.c_str());
  return 0;
}
