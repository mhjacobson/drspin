//
//  drspin.cpp
//  drspin
//
//  Created by Matt Jacobson on 6/2/22.
//

#include "freebsd-symbolicator.h"
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include <libutil.h>
#include <unistd.h>
#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

struct TreeFrame {
    TreeFrame(const uintptr_t address) {
        _address = address;
        _count = 0;
    }

    TreeFrame &child(const uintptr_t address) {
        for (TreeFrame &child : _children) {
            if (child._address == address) {
                return child;
            }
        }

        TreeFrame new_child = TreeFrame(address);
        _children.push_back(new_child);

        return _children.back();
    }

    void increment(const unsigned int value) {
        _count += value;
    }

    virtual void print_tree_with_indentation(unsigned int indentation, Symbolicator &symbolicator) const {
        printf("%*s%u  %s (%#lx)\n", indentation, "", _count, symbolicator.symbolicate(_address).c_str(), _address);

        for (const TreeFrame &child : _children) {
            child.print_tree_with_indentation(indentation + 2, symbolicator);
        }
    }

    void print_tree(Symbolicator &symbolicator) const {
        print_tree_with_indentation(0, symbolicator);
    }

    void sort() {
        std::sort(_children.begin(), _children.end(), std::greater<TreeFrame>());

        for (TreeFrame &child : _children) {
            child.sort();
        }
    }

    bool operator>(const TreeFrame &other) const {
        return _count > other._count;
    }
private:
    uintptr_t _address;
    unsigned int _count;
protected:
    std::vector<TreeFrame> _children;
};

struct RootTreeFrame : public TreeFrame {
    RootTreeFrame() : TreeFrame(0) {}

    virtual void print_tree_with_indentation(unsigned int indentation, Symbolicator &symbolicator) const {
        for (const TreeFrame &child : _children) {
            child.print_tree_with_indentation(indentation, symbolicator);
        }
    }
};

struct Thread {
    using Sample = std::vector<uintptr_t>;
    const lwpid_t lwpid;

    Thread(const lwpid_t lwpid)
    : lwpid(lwpid) { }

    void add_sample(const Sample &&sample) {
        _samples.push_back(sample);
    }

    void print_tree(Symbolicator &symbolicator) const {
        printf("  Thread %#x:\n", this->lwpid);
        RootTreeFrame root_frame;

        for (const Sample &sample : _samples) {
            TreeFrame *cur_frame = &root_frame;

            for (const uintptr_t addr : sample) {
                cur_frame = &cur_frame->child(addr);
                cur_frame->increment(1);
            }
        }

        root_frame.sort();
        root_frame.print_tree_with_indentation(2, symbolicator);
        printf("\n");
    }

private:
    std::vector<Sample> _samples;
};

struct Process : private DeleteImplicit {
    Process(const pid_t pid) {
        _pid = pid;
        _info = kinfo_getproc(pid);
        assert(_info != NULL);
    }

    const char *name() {
        return _info->ki_comm;
    }

    Thread &thread(const lwpid_t lwpid) {
        for (Thread &thread : _threads) {
            if (thread.lwpid == lwpid) {
                return thread;
            }
        }

        return _threads.emplace_back(lwpid);
    }

    void print_tree(Symbolicator &symbolicator) const {
        printf("Process: %s [%d]\n\n", _info->ki_comm, _pid);

        for (const Thread &thread : _threads) {
            thread.print_tree(symbolicator);
        }
    }

    ~Process() {
        free(_info);
    }
private:
    pid_t _pid;
    struct kinfo_proc *_info;
    std::vector<Thread> _threads;
};

bool got_signal;
void handle_signal(int signo) {
    got_signal = true;
}

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage:\n\tdrspin <pid> <seconds>\n");
        exit(1);
    }

    int rv;
    const pid_t pid = atoi(argv[1]);
    const int seconds = atoi(argv[2]);

    signal(SIGHUP, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Process process(pid);

    printf("Sampling process %s [%d] for %d seconds with 1 millisecond of run time between samples...\n", process.name(), pid, seconds);

    rv = ptrace(PT_ATTACH, pid, 0, 0);
    assert(!rv);

    for (int i = 0; i < seconds * 1000 && !got_signal; i++) {
        int status;
        const pid_t waited_pid = wait(&status);
        assert(pid == waited_pid);

        lwpid_t lwpids[64];
        const int num_lwp = ptrace(PT_GETLWPLIST, pid, (caddr_t)lwpids, 64);
        assert(num_lwp > 0);

        for (int j = 0; j < num_lwp; j++) {
            const lwpid_t lwpid = lwpids[j];
            std::vector<uintptr_t> stack;

            struct reg regs;
            rv = ptrace(PT_GETREGS, lwpid, (caddr_t)&regs, 0);
            assert(!rv);

#if defined(__x86_64__) && __x86_64__
            uintptr_t pc = regs.r_rip;
            uintptr_t fp = regs.r_rbp;
#elif defined(__aarch64__) && __aarch64__
            uintptr_t pc = regs.elr; // "exception link register" -- i.e., PC saved from when we interrupted the process
            uintptr_t fp = regs.x[29]; // x29 by convention
#else
#error don't know how to get pc/fp
#endif

            for (;;) {
#if 0
                printf("pc == %lx, fp == %lx\n", pc, fp);
#endif /* 0 */
                stack.insert(stack.begin(), pc);

                uintptr_t data[2];
                struct ptrace_io_desc io_desc = {
                    .piod_op = PIOD_READ_D,
                    .piod_offs = (void *)fp,
                    .piod_addr = data,
                    .piod_len = sizeof (data),
                };
                rv = ptrace(PT_IO, pid, (caddr_t)&io_desc, 0);
                const bool fault = (errno == EFAULT);
                assert(!rv || fault);

#if (defined(__x86_64__) && __x86_64__) || (defined(__aarch64__) && __aarch64__)
                const uintptr_t next_fp = data[0];
                const uintptr_t next_pc = data[1];
#else
#error don't know how to get next pc/fp
#endif

                if (next_fp <= fp || next_fp - fp > 1024 * 1024 || fault) {
#if 0
                    printf("next_fp: %lx (fault: %s)\n", next_fp, fault ? "YES" : "NO");
#endif /* 0 */
                    break;
                }

                pc = next_pc;
                fp = next_fp;
            }

            process.thread(lwpid).add_sample(std::move(stack));
        }

#if 0
        printf("\n");
#endif /* 0 */

        rv = ptrace(PT_CONTINUE, pid, (caddr_t)1, 0);
        assert(!rv);

        usleep(1000);

        kill(pid, SIGSTOP);
    }

    int status;
    const pid_t waited_pid = wait(&status);
    assert(pid == waited_pid);

    printf("Sampling completed.  Processing symbols...\n");

    FreeBSDSymbolicator symbolicator(pid);
    process.print_tree(symbolicator);

    rv = ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    assert(!rv);

//    LLDBSymbolicator symbolicator(pid);
//    process.print_tree(symbolicator);

    return 0;
}
