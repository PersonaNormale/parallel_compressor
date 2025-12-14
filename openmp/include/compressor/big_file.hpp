#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace compressor {

using compressed_chunk = std::vector<std::uint8_t>;
using file_descriptor = std::FILE *;

struct chunk_entry {
  compressed_chunk data;
};

struct writer_token {};

struct big_file_context {
  std::filesystem::path input_path;
  file_descriptor fd_out{nullptr};

  std::vector<chunk_entry *> buf;
  std::size_t max_chunks{0};

  std::vector<writer_token> write_token;
};

bool compress_big_file(const std::filesystem::path &input_path);

void chunk_task(big_file_context &ctx, std::size_t seq);

} // namespace compressor
