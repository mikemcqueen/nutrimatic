#ifndef NUTRIMATIC_MAPPED_FILE_H
#define NUTRIMATIC_MAPPED_FILE_H

#include <cstddef>
#include <cstdint>
#include <string>

// A whole file mapped read-only. Moves, doesn't copy.
class MappedFile {
  public:
    MappedFile() = default;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(MappedFile const&) = delete;
    MappedFile& operator=(MappedFile const&) = delete;
    ~MappedFile();

    // Maps `path`, replacing what was mapped. False, with `*error` set, when
    // it can't be opened or mapped.
    bool open(std::string const& path, std::string* error);

    char const* data() const { return data_; }
    size_t size() const { return size_; }

  private:
    void close();

    char const* data_ = nullptr;
    size_t size_ = 0;
};

// An array in a mapped NumPy .npy file: a little-endian 1-D array of `T`.
template <typename T>
struct NpyArray {
    T const* data = nullptr;
    size_t size = 0;

    T operator[](size_t i) const { return data[i]; }
    T const* begin() const { return data; }
    T const* end() const { return data + size; }
};

// Finds the 1-D array in `file`, an .npy file whose dtype is `descr`
// ("<u4" or "<u8"), of `width`-byte elements. False, with `*error` set, when
// the header doesn't describe one.
bool npy_array(MappedFile const& file, char const* descr, size_t width,
               void const** data, size_t* size, std::string* error);

template <typename T>
bool npy_array(MappedFile const& file, NpyArray<T>* out, std::string* error) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    void const* data;
    if (!npy_array(file, sizeof(T) == 4 ? "<u4" : "<u8", sizeof(T), &data,
                   &out->size, error))
        return false;
    out->data = static_cast<T const*>(data);
    return true;
}

#endif
