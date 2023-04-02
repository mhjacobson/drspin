//
//  freebsd-symbolicator.cpp
//  drspin
//
//  Created by Matt Jacobson on 6/7/22.
//

#include "freebsd-symbolicator.h"
#include <assert.h>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/elf.h>
#include <sys/link_elf.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

template<typename T>
T remote_read(const pid_t pid, const uintptr_t addr) {
    T data;
    struct ptrace_io_desc io_desc = {
        .piod_op = PIOD_READ_D,
        .piod_offs = (void *)addr,
        .piod_addr = &data,
        .piod_len = sizeof (T),
    };

    const int rv = ptrace(PT_IO, pid, (caddr_t)&io_desc, 0);
    assert(!rv);

    return data;
}

std::string remote_read_string(const pid_t pid, const uintptr_t addr) {
    std::string result = "";
    uintptr_t cur_addr = addr;

    for (;;) {
        const char c = remote_read<char>(pid, cur_addr);
        if (c == '\0') break;
        result += c;
        cur_addr++;
    }

    return result;
}

template<typename T>
struct RemoteArray {
    RemoteArray(const pid_t pid, const uintptr_t base_address, const size_t count)
    : _pid(pid), _base_address(base_address), _count(count) {}

    T get(const size_t index) const {
        return remote_read<T>(_pid, _base_address + index * sizeof (T));
    }

    struct Iterator {
        Iterator(const RemoteArray<T> *const array, const size_t index)
        : _array(array), _index(index) {}

        Iterator &operator++() {
            _index++;
            return *this;
        }

        bool operator!=(const Iterator other) const {
            return _index != other._index;
        }

        const T operator*() const {
            return _array->get(_index);
        }
    private:
        const RemoteArray<T> *const _array;
        size_t _index;
    };

    Iterator begin() const {
        return Iterator(this, 0);
    }

    Iterator end() const {
        return Iterator(this, this->_count);
    }

private:
    const pid_t _pid;
    const uintptr_t _base_address;
    const size_t _count;
};

// Scan the process's `auxv` array for an AT_PHDR entry, and return its value, which is the address of the Elf_Phdr in the process's address space.
RemoteArray<Elf_Phdr> get_phdr_array(const pid_t pid) {
    const int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_AUXV, pid };
    Elf_Auxinfo auxv[AT_COUNT];
    size_t auxv_size = sizeof (auxv);

    if (sysctl(mib, 4, auxv, &auxv_size, nullptr, 0) != 0) {
        abort();
    }

    uintptr_t base_address = 0;
    size_t count = 0;

    for (int i = 0; i < auxv_size / sizeof (Elf_Auxinfo); i++) {
        if (auxv[i].a_type == AT_PHDR) {
            base_address = auxv[i].a_un.a_val;
        } else if (auxv[i].a_type == AT_PHNUM) {
            count = auxv[i].a_un.a_val;
        }
    }

    return RemoteArray<Elf_Phdr>(pid, base_address, count);
}

Elf_Phdr get_dynamic_phdr(const pid_t pid) {
    const RemoteArray<Elf_Phdr> phdr_array = get_phdr_array(pid);

    for (const Elf_Phdr phdr : phdr_array) {
        if (phdr.p_type == PT_DYNAMIC) {
            return phdr;
        }
    }

    // Didn't find a PT_DYNAMIC?
    abort();
}

RemoteArray<Elf_Dyn> get_dyn_array(const pid_t pid) {
    const Elf_Phdr dynamic_phdr = get_dynamic_phdr(pid);
    return RemoteArray<Elf_Dyn>(pid, dynamic_phdr.p_vaddr, dynamic_phdr.p_filesz / sizeof (Elf_Dyn));
}

uintptr_t read_debug_ptr(const pid_t pid) {
    const RemoteArray<Elf_Dyn> dyn_array = get_dyn_array(pid);

    for (const Elf_Dyn dyn : dyn_array) {
        if (dyn.d_tag == DT_DEBUG) {
            return dyn.d_un.d_val;
        }
    }

    // Didn't find a DT_DEBUG?
    abort();
}

std::vector<Library> read_libraries(const pid_t pid) {
    const uintptr_t debug_ptr = read_debug_ptr(pid);
    const struct r_debug debug = remote_read<struct r_debug>(pid, debug_ptr);
    uintptr_t link_map_ptr = (uintptr_t)debug.r_map;
    std::vector<Library> libraries;

    while (link_map_ptr != 0) {
        const Link_map map = remote_read<Link_map>(pid, link_map_ptr);
        const std::string path = remote_read_string(pid, (uintptr_t)map.l_name);

        libraries.emplace_back(path, (uintptr_t)map.l_base);
        link_map_ptr = (uintptr_t)map.l_next;
    }

    return libraries;
}

Symbol::Symbol(std::string name, uintptr_t address, size_t size)
: _name(name), _address(address), _size(size) { }

std::string Symbol::name() const {
    return _name;
}

uintptr_t Symbol::address() const {
    return _address;
}

size_t Symbol::size() const {
    return _size;
}

Library::Library(const std::string path, const uintptr_t load_address)
: _path(path), _load_address(load_address) {
    if (path == "[vdso]") return;

    const MappedFile file(path);
    const Elf_Ehdr *const header = file.read<Elf_Ehdr>(0);

    // Get the unslid base address.
    bool got_base_address = false;
    for (const Elf_Phdr &phdr : file.read_array<Elf_Phdr>(header->e_phoff, header->e_phnum)) {
        if (phdr.p_type == PT_LOAD) {
            _base_address = (uintptr_t)phdr.p_vaddr;
            got_base_address = true;
            break;
        }
    }
    assert(got_base_address);

    // Find the symbol tables and their associated string tables.
    StaticUnownedArray<Elf_Sym> symtab, dynsymtab;
    const char *strtab = NULL;
    const char *dynstrtab = NULL;

    // Using the section names string table, walk the sections array.
    const StaticUnownedArray<Elf_Shdr> sections = file.read_array<Elf_Shdr>(header->e_shoff, header->e_shnum);
    const char *const shstrtab = file.read<char>(sections[header->e_shstrndx].sh_offset);

    for (const Elf_Shdr &section : sections) {
        if (section.sh_type == SHT_SYMTAB) {
            symtab = file.read_array<Elf_Sym>(section.sh_offset, section.sh_size / sizeof (Elf_Sym));
        } else if (section.sh_type == SHT_DYNSYM) {
            dynsymtab = file.read_array<Elf_Sym>(section.sh_offset, section.sh_size / sizeof (Elf_Sym));
        } else if (section.sh_type == SHT_STRTAB) {
            const char *const name = shstrtab + section.sh_name;

            if (!strcmp(name, ".strtab")) {
                strtab = file.read<char>(section.sh_offset);
            } else if (!strcmp(name, ".dynstr")) {
                dynstrtab = file.read<char>(section.sh_offset);
            }
        }
    }

    assert(symtab.count() == 0 || strtab != NULL);
    assert(dynsymtab.count() == 0 || dynstrtab != NULL);

    // Add symbols from both symbol tables.
    for (const Elf_Sym &symbol : symtab) {
        if (symbol.st_size > 0) {
            const std::string name = strtab + symbol.st_name;
            _symbols.emplace_back(name, symbol.st_value, symbol.st_size);
        }
    }

    for (const Elf_Sym &symbol : dynsymtab) {
        if (symbol.st_size > 0) {
            const std::string name = dynstrtab + symbol.st_name;
            _symbols.emplace_back(name, symbol.st_value, symbol.st_size);
        }
    }

    // TODO: add "artificial" symbols by parsing the PLT, like lldb does

    std::sort(_symbols.begin(), _symbols.end(), [](const Symbol &a, const Symbol &b) {
        return a.address() < b.address();
    });
}

std::string Library::symbolicate(const uintptr_t address) const {
    // upper_bound() returns the first symbol *greater than* the supplied address (or end() if none).
    auto iter = std::upper_bound(_symbols.begin(), _symbols.end(), address,
                                 [](const uintptr_t address, const Symbol &symbol) {
        return address < symbol.address();
    });

    std::string base_string = "???";

    if (iter != _symbols.begin()) {
        iter--;

        const Symbol &symbol = *iter;
        const uintptr_t offset = address - symbol.address();

        if (offset < symbol.size()) {
            base_string = symbol.name() + " + " + std::to_string(offset);
        }
    }

    return base_string + " (in " + name() + ")";
}

#include <libgen.h>
std::string Library::name() const {
    // TODO: improve
    return std::string(basename((char *)_path.c_str()));
}

uintptr_t Library::load_address() const {
    return _load_address;
}

uintptr_t Library::base_address() const {
    return _base_address;
}

FreeBSDSymbolicator::FreeBSDSymbolicator(const pid_t pid) {
    _pid = pid;
    _libraries = read_libraries(pid);

    std::sort(_libraries.begin(), _libraries.end(), [](const Library &a, const Library b) {
        return a.load_address() < b.load_address();
    });
}

std::string FreeBSDSymbolicator::symbolicate(const uintptr_t address) {
    if (address == 0) return std::string("...");

    // upper_bound() returns the first library *greater than* the supplied address (or end() if none).
    auto iter = std::upper_bound(_libraries.begin(), _libraries.end(), address,
                                 [](const uintptr_t address, const Library &library) {
        return address < library.load_address();
    });

    if (iter != _libraries.begin()) {
        iter--;

        const Library &library = *iter;
        return library.symbolicate(library.base_address() + address - library.load_address());
    } else {
        return "???";
    }
}
