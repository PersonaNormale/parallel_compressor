#pragma once
#include <string>

// operations
enum WorkMode : int { COMP = 0, DECOMP = 1 };

// Global configuration arguments
struct Config {
  int recursive = 1; // -r 0|1 (default: recur)
  bool remove_input =
      false;     // -C 0|1 or -D 0|1, 1 = remove, 0 = preserve (default 0)
  int level = 6; // -l 0..9 compression level
  WorkMode mode = COMP;                // default: compress
  bool verbose = false;                // -v verbose mode
  std::string out_ext = ".defl";       // estensione del file compresso
  std::size_t chunk_size = 256 * 1024; // size dei chunks paralleli (bytes)
};

// Unique instance of the global cfg variable (C++17 inline variable)
inline Config cfg;
