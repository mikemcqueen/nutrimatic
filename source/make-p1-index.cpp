// make-p1-index.cpp - Index phase 1 classifier verdicts for pair lookups.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "p1-index.h"
#include "workflow-paths.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

constexpr char PROGRAM[] = "make-p1-index";

namespace {

constexpr int OPT_THREADS = 256;

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s [-d DIR] [--threads N] -o OUT\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  write OUT, an index of the phase 1 verdicts in every *.jsonl\n"
          "  file of DIR for pair-prior and bag-stats --p1; a pair on more\n"
          "  than one line keeps each word order's largest YES\n"
          "\noptions:\n", stdout);
    dfs_help_option("-d, --dir DIR",
                    "read *.jsonl files in DIR (default: $WFROOT/%s)",
                    WORKFLOW_P1_OUT_PATH);
    dfs_help_option("-o, --output OUT", "write the index to OUT (required)");
    dfs_help_option("--threads N",
                    "read files on N threads, 0 for all of them (default 0)");
    dfs_help_option("-h, --help", "show this help");
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    static struct optparse_long const long_options[] = {
        { "dir", 'd', OPTPARSE_REQUIRED },
        { "output", 'o', OPTPARSE_REQUIRED },
        { "threads", OPT_THREADS, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    struct optparse options;
    optparse_init(&options, argv);
    std::optional<std::filesystem::path> dir;
    std::optional<std::filesystem::path> out;
    int threads = 0;
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 'd':
            dir = options.optarg;
            break;
          case 'o':
            out = options.optarg;
            break;
          case OPT_THREADS:
            if (!parse_count(options.optarg, "--threads", &threads)) {
                usage(argv[0], stderr);
                return 2;
            }
            break;
          case 'h':
            usage(argv[0], stdout);
            return 0;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            usage(argv[0], stderr);
            return 2;
        }
    }
    if (!out || optparse_arg(&options) != NULL) {
        usage(argv[0], stderr);
        return 2;
    }
    if (!default_workflow_path(PROGRAM, &dir, WORKFLOW_P1_OUT_PATH)) return 1;
    std::string error;
    if (!build_p1_index(*dir, *out, resolve_search_threads(threads),
                        &error)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }
    return 0;
}
