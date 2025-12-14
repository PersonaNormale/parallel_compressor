#include "compressor/big_file.hpp"
#include "compressor/small_file.hpp"
#include "decompressor/big_file.hpp"
#include "decompressor/small_file.hpp"
#include "util/cmdline.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <filesystem>

int main(int argc, char *argv[]) {

  // Command line parsing
  const long first_arg = parseCommandLine(argc, argv);
  if (first_arg < 0)
    return 1;

  // Flag per controllare che tutti i file siano stati compressi correttamente
  bool all_ok = true;

  // Funzione lambda per l'enqueuing del file
  auto enqueue_file = [&](const std::filesystem::path &p) {
    // Check per vedere se un file è necessario comprimerlo o meno
    if (!should_process(p)) {
      if (cfg.verbose)
        std::fprintf(stderr, "Skip: %s\n", p.string().c_str());
      return;
    }

    const std::filesystem::path path = p;

    // Check se compressare o meno
    if (cfg.mode == COMP) {
      if (should_use_big_compression(path)) {
        // Chiamata compressione parallela
        all_ok &= compressor::compress_big_file(path);
      } else {
        all_ok &= compressor::compress_small_file(path);
      }
    } else {
      if (looks_like_zlib_stream(path)) {
        all_ok &= decompressor::decompress_small_file(path);
      } else {
        // Chiamata decompressione parallela
        all_ok &= decompressor::decompress_big_file(path);
      }
    }
  };

  // Funzione per il walk nel filesystem delle directory con controllo se il
  // flag ricorsivo sia attivo o meno
  auto walk = [&](const std::filesystem::path &root) {
    if (!std::filesystem::exists(root) ||
        !std::filesystem::is_directory(root)) {
      all_ok = false;
      return;
    }

    if (cfg.recursive) {
      const auto opts =
          std::filesystem::directory_options::skip_permission_denied;

      for (const auto &entry :
           std::filesystem::recursive_directory_iterator(root, opts)) {

        std::error_code ec;
        if (entry.is_regular_file(ec)) {
          enqueue_file(entry.path());
        }
      }
    } else {
      for (const auto &de : std::filesystem::directory_iterator(
               root,
               std::filesystem::directory_options::skip_permission_denied)) {

        std::error_code ec;
        if (de.is_regular_file(ec)) {
          enqueue_file(de.path());
        }
      }
    }
  };

  for (int i = static_cast<int>(first_arg); i < argc; ++i) {
    const std::filesystem::path in(argv[i]);

    if (isDirectory(argv[i])) {
      walk(in);
      continue;
    }

    enqueue_file(in);
  }

  return all_ok ? 0 : 1;
}
