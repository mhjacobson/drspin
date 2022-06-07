# `drspin`: sampling profiler for FreeBSD

This is a sampling profiler for FreeBSD, in the spirit of sample(1) or spindump(8) on Mac OS X.

It uses frame-pointer-based stack walking, so code compiled with `-fomit-frame-pointer` will confuse it.  It supports multiple kernel threads.

It can use either of two symbolication engines: (1) a FreeBSD-specific one that queries the dynamic linker info and parses ELF symbol tables, or (2) a hackier but in some cases fuller-featured one (e.g., C++ demangling, "artificial" symbols) that puppets LLDB.

Example usage:

```
# drspin `pgrep sophie` 5
Sampling process sophie [42205] for 5 seconds with 1 millisecond of run time between samples...
Sampling completed.  Processing symbols...
Process: sophie [42205]

  Thread 0x187f0:
  4992  _start + 256 (in sophie) (0x207780)
    4884  main + 352 (in sophie) (0x208211)
      4546  _ZN5Input14get_next_frameEPb + 190 (in sophie) (0x20ba2c)
        4546  av_read_frame + 655 (in libavformat.so.59) (0x8003321df)
          4545  read_frame_internal + 108 (in libavformat.so.59) (0x80033227c)
            ...
```
