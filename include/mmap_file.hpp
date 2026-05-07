#pragma once

// Read-only memory-mapped file wrapper. Used by the search engine to share
// the posting file across worker threads without holding a mutex on a single
// ifstream.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace idx::io {

class MmapFile {
public:
    static MmapFile open_readonly(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open: " + path);
        }
        struct stat st {};
        if (::fstat(fd, &st) < 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "fstat: " + path);
        }
        void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                         PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "mmap: " + path);
        }
        ::madvise(p, static_cast<std::size_t>(st.st_size), MADV_RANDOM);
        return MmapFile{fd, p, static_cast<std::size_t>(st.st_size)};
    }

    ~MmapFile() {
        if (data_ != nullptr && data_ != MAP_FAILED) {
            ::munmap(data_, size_);
        }
        if (fd_ >= 0) ::close(fd_);
    }

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    MmapFile(MmapFile&& o) noexcept : fd_(o.fd_), data_(o.data_), size_(o.size_) {
        o.fd_ = -1;
        o.data_ = nullptr;
        o.size_ = 0;
    }

    const std::uint8_t* data() const noexcept {
        return static_cast<const std::uint8_t*>(data_);
    }

    std::size_t size() const noexcept { return size_; }

private:
    MmapFile(int fd, void* p, std::size_t s) : fd_(fd), data_(p), size_(s) {}

    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace idx::io
