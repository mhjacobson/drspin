//
//  lldb-symbolicator.cpp
//  drspin
//
//  Created by Matt Jacobson on 6/2/22.
//

#include "lldb-symbolicator.h"
#include <assert.h>
#include <stdio.h>
#include <string>
#include <sys/types.h>

LLDBSymbolicator::LLDBSymbolicator(const pid_t pid) {
    const std::string lldb_command = std::string("/usr/bin/lldb -p ") + std::to_string(pid);
    _connection = popen(lldb_command.c_str(), "r+");

    // Read the prologue.  (TODO: less hacky way?)
    for (int i = 0; i < 5; i++) {
        size_t line_len;
        char *const line = fgetln(_connection, &line_len);
        assert(line != NULL);
#if 0
        line[line_len - 1] = '\0';
        fprintf(stderr, "discarded: %s\n", line);
#endif /* 0 */
    }
}

std::string LLDBSymbolicator::symbolicate(const uintptr_t address) {
    if (address == 0) return std::string("...");

    std::string result = "???";
    const auto entry = _cache.find(address);

    if (entry != _cache.end()) {
        result = entry->second;
    } else {
        char buf[48];
        sprintf(buf, "%#lx", address);

        // NOTE: the second (pointless) command is so we can tell when the output from the first command is over and can return without leaving stale input in the buffer.
        const std::string command = std::string("image look -a ") + buf + "\np (void)0\n";

#if 0
        fprintf(stderr, "writing: %s\n", command.c_str());
#endif /* 0 */
        fputs(command.c_str(), _connection);

        // Discard the echo.  (TODO: less hacky way?)
        for (int i = 0; i < 1; i++) {
            size_t line_len;
            char *const line = fgetln(_connection, &line_len);
            assert(line != NULL);
#if 0
            line[line_len - 1] = '\0';
            fprintf(stderr, "discarded: %s\n", line);
#endif /* 0 */
        }

        for (;;) {
            size_t line_len;
            char *const line = fgetln(_connection, &line_len);

            if (line == NULL) {
                break;
            } else {
                if (line[line_len - 1] == '\n') {
                    line[line_len - 1] = '\0';
#if 0
                    fprintf(stderr, "read: %s\n", line);
#endif /* 0 */
                    const char *const needle = strstr(line, "Summary: ");

                    if (needle != NULL) {
                        result = std::string(needle + 9);
                        // We've got our result, but keep looping until we get the prompt.
                    } else if (!strncmp(line, "(lldb)", 6)) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }

        _cache[address] = result;
    }

    return result;
}

LLDBSymbolicator::~LLDBSymbolicator() {
#if 0
    printf("closing connection %p\n", _connection);
#endif /* 0 */
    pclose(_connection);
}
