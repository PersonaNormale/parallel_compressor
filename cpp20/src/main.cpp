#include "compressor/big_file.hpp"
#include "compressor/small_file.hpp"
#include "decompressor/big_file.hpp"
#include "decompressor/small_file.hpp"
#include "threadpool/threadPool.hpp"
#include "util/cmdline.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"

#include <filesystem>
#include <future>
#include <vector>

int main(int argc, char *argv[]) {
  const long first_arg = parseCommandLine(argc, argv);
  if (first_arg < 0)
    return 1;

  bool all_ok = true;

  threadPool pool; // defaults to hardware_concurrency

  std::vector<std::filesystem::path> big_files;
  std::vector<std::filesystem::path> small_files;

  auto enqueue_file = [&](const std::filesystem::path &p) {
    if (!should_process(p)) {
      if (cfg.verbose)
        std::fprintf(stderr, "Skip: %s\n", p.string().c_str());
      return;
    }

    const std::filesystem::path path = p;

    if (cfg.mode == COMP) {
      if (should_use_big_compression(path)) {
        big_files.push_back(path);
      } else {
        small_files.push_back(path);
      }
    } else {
      if (looks_like_zlib_stream(path)) {
        small_files.push_back(path);
      } else {
        big_files.push_back(path);
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

  // Esecuzione dei big file uno alla volta
  for (const auto &path : big_files) {
    if (cfg.mode == COMP) {
      all_ok &= compressor::compress_big_file(pool, path);
    } else {
      all_ok &= decompressor::decompress_big_file(pool, path);
    }
  }

  // Esecuzione degli small file in parallelo
  std::vector<std::future<bool>> tasks;
  tasks.reserve(small_files.size());

  for (const auto &path : small_files) {
    tasks.emplace_back(
      pool.submit([path] {
        if (cfg.mode == COMP)
          return compressor::compress_small_file(path);
        else
          return decompressor::decompress_small_file(path);
      })
    );
  }

  for (auto &f : tasks) {
    all_ok &= f.get();
  }

  return all_ok ? 0 : 1;
}
