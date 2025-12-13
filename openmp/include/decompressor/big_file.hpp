#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace decompressor {

using decompressed_chunk = std::vector<std::uint8_t>;
using file_descriptor = std::FILE *;

// rappresenta un chunk decompresso pronto per essere scritto
struct dechunk_entry {
  decompressed_chunk data;
};

// metadati di un chunk compresso all'interno del file .miniz
struct chunk_index {
  std::uint64_t len;    // lunghezza compressa
  std::uint64_t offset; // offset nel file dei dati compressi
};

// contesto condiviso da tutti i task di decompressione
struct big_file_context {
  std::filesystem::path input_path;
  file_descriptor fd_out{nullptr};

  std::vector<chunk_index> index;   // metadati chunk compressi
  std::vector<dechunk_entry *> buf; // buffer condiviso dei chunk decompressi

  std::size_t max_chunks{0};
};

// Scansiona il file compresso e costruisce index[] con coppie {len, offset}
bool build_index(FILE *f, std::vector<chunk_index> &index_out);

// Task di decompressione di un singolo chunk (produttore)
void chunk_task(big_file_context &ctx, std::size_t seq);

// Decomprime un big file in parallelo con OpenMP
bool decompress_big_file(const std::filesystem::path &input_path);

} // namespace decompressor
