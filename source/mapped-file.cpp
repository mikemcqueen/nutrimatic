#include "mapped-file.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

MappedFile::~MappedFile() { close(); }

void MappedFile::close() {
    if (data_ != nullptr && size_ > 0)
        munmap(const_cast<char*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
}

bool MappedFile::open(std::string const& path, std::string* error) {
    close();
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = path + ": " + strerror(errno);
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
        *error = path + ": " + strerror(errno);
        ::close(fd);
        return false;
    }
    size_ = size_t(info.st_size);
    if (size_ == 0) {
        ::close(fd);
        data_ = "";
        return true;
    }
    void* const map = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        *error = path + ": can't map: " + strerror(errno);
        size_ = 0;
        return false;
    }
    data_ = static_cast<char const*>(map);
    return true;
}

bool npy_array(MappedFile const& file, std::string_view descr, size_t width,
               void const** data, size_t* size, std::string* error) {
    std::string_view const text(file.data(), file.size());
    if (text.size() < 10 || text.substr(0, 6) != "\x93NUMPY") {
        *error = "not an .npy file";
        return false;
    }
    size_t header_length;
    size_t header_start;
    if (uint8_t(text[6]) == 1) {
        header_length = uint8_t(text[8]) | size_t(uint8_t(text[9])) << 8;
        header_start = 10;
    } else {
        if (text.size() < 12) {
            *error = "truncated .npy header";
            return false;
        }
        uint32_t length;
        memcpy(&length, text.data() + 8, 4);
        header_length = length;
        header_start = 12;
    }
    if (header_start + header_length > text.size()) {
        *error = "truncated .npy header";
        return false;
    }
    std::string_view const header = text.substr(header_start, header_length);
    std::string const dtype(descr);
    if (header.find("'descr': '" + dtype + "'") ==
            std::string_view::npos ||
        header.find("'fortran_order': False") == std::string_view::npos) {
        *error = "not a " + dtype + " array";
        return false;
    }
    size_t const shape = header.find("'shape': (");
    if (shape == std::string_view::npos) {
        *error = "no shape in the .npy header";
        return false;
    }
    char* end;
    std::string const dims(header.substr(shape + 10));
    unsigned long long const count = strtoull(dims.c_str(), &end, 10);
    if (end == dims.c_str() || (*end != ',' && *end != ')') ||
        (*end == ',' && end[1] != ')')) {
        *error = "not a 1-D array";
        return false;
    }
    size_t const offset = header_start + header_length;
    if (count > (text.size() - offset) / width) {
        *error = "truncated .npy data";
        return false;
    }
    *data = text.data() + offset;
    *size = size_t(count);
    return true;
}
