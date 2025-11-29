#include "compressor/big_file.hpp"
#include "compressor/small_file.hpp"
#include "decompressor/big_file.hpp"
#include "decompressor/small_file.hpp"
#include "threadpool/threadPool.hpp"
#include "util/cmdline.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>
 
namespace {

// Soglia (in byte) oltre la quale usiamo la pipeline chunked parallela
constexpr std::uintmax_t BIG_FILE_THRESHOLD = 256 * 1024; // 256 KB

// Riconosce un header zlib (RFC1950) per distinguere i file compressi small
bool looks_like_zlib_stream(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  unsigned char cmf = 0, flg = 0;
  if (!in.read(reinterpret_cast<char *>(&cmf), 1) ||
      !in.read(reinterpret_cast<char *>(&flg), 1))
    return false;

  // Controllo minimo: metodo di compressione = deflate (8) e checksum header
  const bool method_ok = (cmf & 0x0F) == 8;
  const bool checksum_ok = ((static_cast<unsigned>(cmf) << 8) + flg) % 31 == 0;
  return method_ok && checksum_ok;
}

bool should_use_big_compression(const std::filesystem::path &p) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if (ec)
    return false;
  return size >= BIG_FILE_THRESHOLD;
}

} // namespace

int main(int argc, char *argv[]) {
  const long first_arg = parseCommandLine(argc, argv);
  if (first_arg < 0)
    return 1;

  bool all_ok = true;

  threadPool pool; // defaults to hardware_concurrency
  std::vector<std::future<bool>> tasks;

  auto enqueue_file = [&](const std::filesystem::path &p) {
    if (!should_process(p)) {
      if (cfg.verbose)
        std::fprintf(stderr, "Skip: %s\n", p.string().c_str());
      return;
    }
    tasks.emplace_back(pool.submit([path = p.string(), &pool] {
      const std::filesystem::path p(path);
      if (cfg.mode == COMP) {
        if (should_use_big_compression(p))
          return compressor::compress_big_file(pool, p);
        return compressor::compress_small_file(p);
      }

      // Modalità decompressione: prova a riconoscere zlib, altrimenti chunked
      if (looks_like_zlib_stream(p))
        return decompressor::decompress_small_file(p);
      return decompressor::decompress_big_file(pool, p);
    }));
  };

  auto walk = [&](const std::filesystem::path &root) {
    if (!std::filesystem::exists(root) ||
        !std::filesystem::is_directory(root)) {
      std::fprintf(stderr, "Non è una directory: %s\n", root.string().c_str());
      all_ok = false;
      return;
    }

    if (cfg.recursive) {
      std::filesystem::directory_options opts =
          std::filesystem::directory_options::skip_permission_denied;
      for (auto it = std::filesystem::recursive_directory_iterator(root, opts),
                end = std::filesystem::recursive_directory_iterator();
           it != end; ++it) {
        std::error_code ec;
        if (it->is_symlink(ec) || it->is_directory(ec))
          continue;
        if (it->is_regular_file(ec))
          enqueue_file(it->path());
      }
    } else {
      for (auto &de : std::filesystem::directory_iterator(
               root, std::filesystem::directory_options::skip_permission_denied)) {
        std::error_code ec;
        if (de.is_regular_file(ec))
          enqueue_file(de.path());
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

  for (auto &fut : tasks) {
    all_ok &= pool.wait_future(fut);
  }

  return all_ok ? 0 : 1;
}
