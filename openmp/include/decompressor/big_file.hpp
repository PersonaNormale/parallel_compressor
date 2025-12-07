#pragma once

#include "threadpool/threadPool.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace decompressor {

using decompressed_chunk = std::vector<std::uint8_t>;

using file_descriptor = std::FILE *;

// rappresenta un chunk decompresso pronto per essere scritto
struct dechunk_entry {
  std::atomic_bool ready{false};
  decompressed_chunk data; // contenuto decompresso
};

// metadati di un chunk compresso all'interno del file .miniz
struct chunk_index {
  std::uint64_t len;    // lunghezza compressa
  std::uint64_t offset; // offset nel file compresso dei dati (non della len)
};

// contesto condiviso da tutti i task di decompressione
struct big_file_context {
  threadPool *pool{};

  std::filesystem::path input_path;
  file_descriptor fd_out{nullptr};

  std::vector<chunk_index> index;   // metadati chunk compressi
  std::vector<dechunk_entry *> buf; // buffer condiviso dei chunk decompattati

  std::size_t max_chunks{0};
  std::atomic_size_t curr{0}; // prossimo chunk da scrivere
  std::atomic_bool all_chunks_scheduled{false};

  std::mutex cv_mtx;
  std::condition_variable cv;
};

// Scansiona il file compresso e costruisce index[] con coppie {len, offset}
bool build_index(FILE *f, std::vector<chunk_index> &index_out);

// Task di decompressione di un singolo chunk (produttore)
void chunk_task(big_file_context &ctx, std::size_t seq);

// Writer sequenziale (consumatore) che scrive i chunk decompattati in ordine
void writer_task(big_file_context &ctx);

// Funzione di alto livello: decomprime un "big file" in parallelo
bool decompress_big_file(threadPool &pool,
                         const std::filesystem::path &input_path);

} // namespace decompressor
