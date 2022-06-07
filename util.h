//
//  util.h
//  drspin
//
//  Created by Matt Jacobson on 6/7/22.
//

#include <assert.h>
#include <stdint.h>
#include <array>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef UTIL_H
#define UTIL_H

struct DeleteImplicit {
    DeleteImplicit() = default;
    DeleteImplicit(DeleteImplicit &) = delete;
    DeleteImplicit(DeleteImplicit &&) = delete;
    DeleteImplicit &operator=(DeleteImplicit &) = delete;
    DeleteImplicit &operator=(DeleteImplicit &&) = delete;
    ~DeleteImplicit() = default;
};

template<typename T>
struct StaticUnownedArray {
    StaticUnownedArray()
    : _pointer(nullptr), _count(0) {}
    StaticUnownedArray(const T *pointer, const size_t count)
    : _pointer(pointer), _count(count) {}
    size_t count() const { return _count; }
    const T &operator[](const size_t index) const { return _pointer[index]; }
    const T *begin() const { return _pointer; }
    const T *end() const { return _pointer + _count; }
private:
    const T *_pointer;
    size_t _count;
};

struct MappedFile : public DeleteImplicit {
    MappedFile(const std::string path) {
        const int fd = open(path.c_str(), O_RDONLY);
        assert(fd != -1);

        struct stat st;
        const int rv = fstat(fd, &st);
        assert(!rv);

        _size = st.st_size;
        _va = mmap(NULL, _size, PROT_READ, MAP_SHARED, fd, 0);
        assert(_va != MAP_FAILED);

        close(fd);
    }

    template<typename T>
    const T *read(const size_t offset) const {
        return (const T *)((const char *)_va + offset);
    }

    template<typename T>
    const StaticUnownedArray<T> read_array(const size_t offset, const size_t count) const {
        return StaticUnownedArray<T>((const T *)((const char *)_va + offset), count);
    }

    ~MappedFile() {
        const int rv = munmap((void *)_va, _size);
        assert(!rv);
    }
private:
    const void *_va;
    size_t _size;
};

struct Symbolicator {
    virtual std::string symbolicate(uintptr_t address) = 0;
};

#endif /* UTIL_H */
