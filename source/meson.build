project(
  'nutrimatic', ['cpp'],
  version: '0.1',
  default_options: [
    'cpp_std=c++17',
    'warning_level=3',
    'werror=true',
    # The search spends nearly all its time crossing between these libraries:
    # SearchDriver::step() calls IndexReader::children() once per step and the
    # filter's has_transition() once per surviving child.  Through a shared
    # library neither can be inlined and every call goes through the PLT.
    # Linking statically with LTO lets them inline into step(); worth ~7% on
    # find-anagrams.  See findings/perf-analysis.md.
    'default_library=static',
    'b_lto=true',
  ],
)

# Disable warnings that openfst headers trigger
if meson.get_compiler('cpp').get_id() == 'clang'
  # clang-only args
  extra_toolchain_args = ['-Wno-deprecated-copy']
else
  # gcc-only args
  extra_toolchain_args = [
    '-Wno-dangling-pointer',
    '-Wno-missing-template-keyword',
  ]
endif
project_args = [
  '-Wno-ignored-qualifiers',
  '-Wno-sign-compare',
  '-Wno-overloaded-virtual',
  '-Wno-unused-parameter',
  '-Wno-vla',
] + extra_toolchain_args

add_project_arguments(
  project_args,
  language: ['cpp'],
)

fst_dep = dependency('openfst')
tre_dep = dependency('tre')
xml2_dep = dependency('libxml-2.0')

# Public-domain getopt_long work-alike (https://github.com/skeeto/optparse),
# vendored because getopt.h is POSIX-only.  Built with warnings off since it
# isn't our code to fix.
optparse_lib = static_library(
  'optparse',
  ['optparse.cpp'],
  override_options: ['warning_level=0', 'werror=false'],
)

index_lib = library(
  'index',
  ['index-reader.cpp', 'index-walker.cpp', 'index-writer.cpp'],
)

search_lib = library(
  'search',
  ['search-driver.cpp', 'search-printer.cpp'],
  link_with: [index_lib],
)

expr_lib = library(
  'expr', [
    'expr-anagram.cpp',
    'expr-filter.cpp',
    'expr-intersect.cpp',
    'expr-optimize.cpp',
    'expr-parse.cpp'
  ],
  link_with: [search_lib],
  dependencies: fst_dep,
)

foreach p : ['remove-markup']
  executable(p, p + '.cpp', dependencies: [tre_dep, xml2_dep], install: true)
endforeach

foreach p : ['make-index', 'merge-indexes', 'dump-index', 'explore-index']
  executable(p, p + '.cpp', link_with: index_lib, install: true)
endforeach

executable('find-anagrams', 'find-anagrams.cpp',
  link_with: [optparse_lib, search_lib, index_lib], install: true)

foreach p : ['find-phone-words']
  executable(p, p + '.cpp', link_with: [search_lib, index_lib], install: true)
endforeach

foreach p : ['find-expr', 'test-expr']
  executable(p, p + '.cpp', link_with: [expr_lib, search_lib, index_lib], dependencies: fst_dep, install: true)
endforeach
