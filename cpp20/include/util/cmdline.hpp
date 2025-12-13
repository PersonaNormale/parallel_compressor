#pragma once

#include "config.hpp"
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

inline void usage(const char *prog) {
  std::fprintf(stderr,
               R"(Uso: %s [opzioni] <file_o_dir> [altri_file_o_dir...]
Opzioni:
  -r 0|1     Recur into directories (default 1)
  -C 0|1     Compress (0 preserve, 1 remove (default 0))
  -D 0|1     Decompress (0 preserve, 1 remove (default 0))
  -k <bytes>  Small-file threshold and chunk size (default 262144)
  -v         Verbose mode
  -h         Help

Esempio:
  %s -r 1 -C 0 mydir file1.bin file2.txt
)",
               prog, prog);
}

inline long parseCommandLine(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return -1;
  }

  // safe convertion from char to int
  auto to_int = [](const char *s, int &out) -> bool {
    if (!s)
      return false;
    const char *begin = s;
    const char *end = s + std::char_traits<char>::length(s);
    auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc{} && r.ptr == end;
  };

  auto to_size = [](const char *s, std::size_t &out) -> bool {
    if (!s)
      return false;
    const char *begin = s;
    const char *end = s + std::char_traits<char>::length(s);
    unsigned long long val = 0;
    auto r = std::from_chars(begin, end, val);
    if (r.ec != std::errc{} || r.ptr != end)
      return false;
    out = static_cast<std::size_t>(val);
    return true;
  };

  bool seenC = false, seenD = false;
  int i = 1;
  while (i < argc && argv[i][0] == '-') {
    std::string_view opt = argv[i];
    if (opt == "-h" || opt == "--help") {
      usage(argv[0]);
      return -1;
    } else if (opt == "-v") {
      cfg.verbose = true;
      ++i;
    } else if (opt == "-k") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return -1;
      }
      std::size_t chunk = 0;
      if (!to_size(argv[i + 1], chunk) || chunk == 0) {
        std::fprintf(stderr, "Error: invalid chunk size.\n");
        return -1;
      }
      cfg.chunk_size = chunk;
      i += 2;
    } else if (opt == "-r" || opt == "-C" || opt == "-D") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return -1;
      }
      int val = 0;
      if (!to_int(argv[i + 1], val)) {
        usage(argv[0]);
        return -1;
      }

      if (opt == "-r") {
        cfg.recursive = (val != 0);
      } else if (opt == "-C") {
        if (seenD) {
          std::fprintf(stderr, "Error: only one between -C and -D.\n");
          return -1;
        }
        seenC = true;
        cfg.mode = COMP;
        cfg.remove_input = (val != 0);
      } else { // -D
        if (seenC) {
          std::fprintf(stderr, "Error: only one between -C and -D.\n");
          return -1;
        }
        seenD = true;
        cfg.mode = DECOMP;
        cfg.remove_input = (val != 0);
      }
      i += 2;
    } else {
      usage(argv[0]);
      return -1;
    }
  }

  if (!seenC && !seenD) {
    std::fprintf(stderr, "Error: at least one between -C and -D.\n");
    usage(argv[0]);
    return -1;
  }
  if (i >= argc) {
    std::fprintf(stderr, "Error: no file or dir to work.\n");
    usage(argv[0]);
    return -1;
  }
  return i; // index of the first optional args
}
