#pragma once

#include "threadpool/threadPool.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace compressor {

using compressed_chunk =
    std::vector<std::uint8_t>; // più elegante di usare char IMO, seguendo anche
                               // le convenzioni della libreria miniz
using file_descriptor = std::FILE *;

/// Entry del buffer condiviso per un singolo chunk compresso.
struct chunk_entry {
  compressed_chunk data;
};

/// Contesto condiviso tra tutti i task che partecipano alla compressione di un
/// big file.
struct big_file_context {
  threadPool *pool; // TODO da rimuovere nel polishing

  std::filesystem::path input_path; /// path del file sorgente
  file_descriptor fd_out{};         /// file di output

  std::vector<chunk_entry *> buf; /// buffer di chunk compressi
  std::size_t max_chunks{0};      /// numero totale di chunk
};

/// Task che legge e comprime un singolo chunk (seq) del big file.
void chunk_task(big_file_context &ctx, std::size_t seq);

/// Task persistente che scrive in ordine i chunk pronti.
void writer_task(big_file_context &ctx);

/// Task di alto livello: prepara il contesto e programma chunk_task +
/// writer_task.
bool compress_big_file(threadPool &pool,
                       const std::filesystem::path &input_path);

} // namespace compressor
