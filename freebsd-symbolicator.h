//
//  freebsd-symbolicator.h
//  drspin
//
//  Created by Matt Jacobson on 6/7/22.
//

#include "util.h"
#include <stdint.h>
#include <string>
#include <vector>

#ifndef FREEBSD_SYMBOLICATOR_H
#define FREEBSD_SYMBOLICATOR_H

struct Symbol {
    Symbol(std::string name, uintptr_t address, size_t size);
    std::string name() const;
    uintptr_t address() const;
    size_t size() const;
private:
    std::string _name;
    uintptr_t _address;
    size_t _size;
};

struct Library {
    Library(std::string path, uintptr_t load_address);
    std::string symbolicate(uintptr_t address) const;
    std::string path() const;
    std::string name() const;
    uintptr_t load_address() const;
    uintptr_t base_address() const;
private:
    std::string _path;
    uintptr_t _load_address;
    uintptr_t _base_address;
    std::vector<Symbol> _symbols;
};

struct FreeBSDSymbolicator : public Symbolicator {
    FreeBSDSymbolicator(pid_t pid);
    std::string symbolicate(uintptr_t address);
    void print_libraries() const;
private:
    pid_t _pid;
    std::vector<Library> _libraries;
};

#endif /* FREEBSD_SYMBOLICATOR_H */
