#include "index.h"

#include <assert.h>
#include <limits.h>
#include <sys/mman.h>

#include <algorithm>

using namespace std;

IndexReader::IndexReader(FILE* fp) {
  // open the file
  fseek(fp, 0, SEEK_END);
  length = ftell(fp);
  void* map = mmap(NULL, length, PROT_READ, MAP_SHARED, fileno(fp), 0);
  data = (const unsigned char*) map;
  if (map == MAP_FAILED) {
    fprintf(stderr, "error: can't mmap data file (length %zu)\n", length);
    exit(1);
  }

  // scan the top level nodes to compute the total
  std::vector<Choice> top;
  children(root(), 0, CHAR_MIN, CHAR_MAX, &top);
  while (top.size() == 1 && top[0].count == 0) {
    off_t node = top[0].next;
    top.clear();
    children(node, 0, CHAR_MIN, CHAR_MAX, &top);
  }

  total = 0;
  for (size_t i = 0; i < top.size(); ++i) total += top[i].count;
}

IndexReader::~IndexReader() {
  munmap((void*) data, length);
}

int IndexReader::children(off_t n, int64_t count,
                          char min, char max,
                          std::vector<Choice>* out) const {
  CharSet allowed;
  allowed.clear();
  for (int c = 0; c <= UCHAR_MAX; ++c)
    if ((char) c >= min && (char) c <= max) allowed.set((unsigned char) c);
  return children(n, count, allowed, out);
}

int IndexReader::children(off_t n, int64_t count,
                          CharSet const& allowed,
                          std::vector<Choice>* out) const {
  if (n == (off_t) -1) return count;

  Choice choice;
  assert(n >= 1 && n <= length);
  int num = data[--n];
  assert(num >= 0 && num < 0x100);

  if (num >= 0x20 && num < 0x80) {
    if (n < 1) fail(n, "need immediate next");
    if (allowed.has((unsigned char) num)) {
      choice.ch = num;
      choice.count = count;
      choice.next = n;
      out->push_back(choice);
    }
    return 0;
  }

  int count_size = (num < 0xC0) ? 1 : (num < 0xE0) ? 2 : 8;
  int offset_size = (num < 0x20) ? 0 : (num < 0xA0) ? 1 : (num < 0xE0) ? 2 : 8;

  num = num & 0x1F;
  if (num == 0) {
    if (n < 1) fail(n, "need count");
    num = data[--n];
  }

  ssize_t size = count_size + offset_size + 1;
  if (num == 0 || n < num * size) fail(n, "bad size");

  off_t start = n - num * size;
  for (off_t p = start; p < n; p += size) {
    // The character is one byte at a known offset; everything else about this
    // child is variable-width and has to be decoded.  Rejecting here skips all
    // of that, and the push_back behind it.
    if (!allowed.has(data[p])) continue;  // TODO: binary search
    choice.ch = data[p];

    if (count_size == 1) {
      choice.count = data[p + 1];
    } else if (count_size == 2) {
      choice.count = data[p + 1] | (data[p + 2] << 8);
    } else {
      choice.count = 0;
      for (int j = 0; j < count_size; ++j)
        choice.count |= (uint64_t)(data[p + 1 + j]) << (j * 8);
    }

    if (choice.count <= 0) fail(p + 1, "bad count");
    
    if (offset_size == 0) {
      choice.next = (off_t) -1;
    } else if (offset_size == 1) {
      off_t offset = data[p + 1 + count_size];
      choice.next = (offset == 255) ? (off_t) -1 : start - offset;
    } else if (offset_size == 2) {
      off_t offset = data[p + count_size + 1] | (data[p + count_size + 2] << 8);
      choice.next = (offset == 65535) ? (off_t) -1 : start - offset;
    } else {
      off_t offset = 0;
      for (int j = 0; j < offset_size; ++j)
        offset |= (uint64_t)(data[p + 1 + count_size + j]) << (j * 8);
      assert(offset_size == sizeof(off_t));
      if (offset == (off_t) -1)
        choice.next = (off_t) -1;
      else
        choice.next = start - offset;
    }

    if (choice.next != (off_t) -1 && (choice.next < 0 || choice.next > start))
      fail(p + 1 + count_size, "bad offset");
    out->push_back(choice);
    count -= choice.count;
  }

  return count;
}

bool IndexReader::exact_entry_count(
    std::string const& entry, int64_t* result) const {
  Node node = root();
  int64_t parent_count = count();
  CharSet allowed;
  std::vector<Choice> choices;

  for (size_t i = 0; i <= entry.size(); ++i) {
    unsigned char const ch =
        i == entry.size() ? (unsigned char) ' '
                          : (unsigned char) entry[i];
    allowed.clear();
    allowed.set(ch);
    choices.clear();
    children(node, parent_count, allowed, &choices);
    if (choices.empty()) return false;
    assert(choices.size() == 1);
    node = choices[0].next;
    parent_count = choices[0].count;
  }

  // A choice's count includes entries below it. The count belonging exactly
  // to the selected trailing-space node is the residual returned after its
  // children are removed.
  allowed.fill();
  choices.clear();
  int64_t const exact = children(node, parent_count, allowed, &choices);
  if (exact <= 0) return false;
  *result = exact;
  return true;
}

void IndexReader::fail(off_t n, const char* message) const {
  fprintf(stderr, "error: pos %lld = 0x%02x: %s\n", static_cast<long long>(n),
      data[n], message);
  exit(1);
}
