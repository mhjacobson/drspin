//
//  lldb-symbolicator.hpp
//  drspin
//
//  Created by Matt Jacobson on 6/2/22.
//

#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

#ifndef LLDB_SYMBOLICATOR_H
#define LLDB_SYMBOLICATOR_H

struct LLDBSymbolicator : public Symbolicator, private DeleteImplicit {
    LLDBSymbolicator(pid_t pid);
    std::string symbolicate(uintptr_t address);
    ~LLDBSymbolicator();
private:
    std::unordered_map<uintptr_t, std::string> _cache;
    FILE *_connection;
};

#endif /* LLDB_SYMBOLICATOR_H */
