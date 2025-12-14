#include "compressor/big_file.hpp"
#include "compressor/compressor.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <cstdio>
#include <filesystem>
#include <omp.h>
#include <vector>

namespace compressor {

/// Comprime il buffer e ritorna un array di byte
compressed_chunk compress_buffer(const std::vector<unsigned char> &raw) {
  size_t out_size = 0;

  void *out = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &out_size,
                                         TDEFL_WRITE_ZLIB_HEADER);

  if (!out || out_size == 0)
    throw std::runtime_error("compression failed");

  // Crea un vettore di byte di dimensione dell'output
  compressed_chunk res(out_size);
  // Copia out in res
  memcpy(res.data(), out, out_size);
  // Fa una free
  mz_free(out);
  return res;
}

/// Task di compressione del chunk
/// Prende il contesto del big_file e il numero di sequenza
void chunk_task(big_file_context &ctx, std::size_t seq) {
  // Calcolo l'offset di dove prenderlo dalla memoria
  const auto offset = util::chunk_offset(seq, cfg.chunk_size);
  // Leggo i raw byte dal file
  auto raw = util::read_chunk(ctx.input_path.string(), offset, cfg.chunk_size);

  // Comprimo i raw byte letti dalla memoria
  auto compressed = compress_buffer(raw);

  // Prendo la chunk entry dal buffer nel contesto e faccio una move dei dati
  chunk_entry *e = ctx.buf[seq].get();
  e->data = std::move(compressed);
}

/// Scrittura del singolo chunk (collector ordinato)
/// Prende il contesto e il numero di sequenza da dove iniziare
static void writer_write_one_chunk(big_file_context &ctx, std::size_t seq) {
  // Prendo la referenza al chunk entry corrente
  chunk_entry *ce = ctx.buf[seq].get();
  // Prendo il primo vettore di byte compressi
  const auto &vec = ce->data;

  // Prendo lunghezza del chunk compresso
  std::uint64_t len = vec.size();
  if (std::fwrite(&len, sizeof(len), 1, ctx.fd_out) != 1) {
    throw std::runtime_error(
        "writer_write_one_chunk: failed to write chunk length");
  }

  // Scrivo i dati compressi
  if (!vec.empty()) {
    size_t written = std::fwrite(vec.data(), 1, vec.size(), ctx.fd_out);
    if (written != vec.size()) {
      throw std::runtime_error(
          "writer_write_one_chunk: failed to write chunk data");
    }
  }
}

/// Orchestratore
bool compress_big_file(const std::filesystem::path &input_path) {
  // Dichiaro la struttura del contesto
  big_file_context ctx{};
  ctx.input_path = input_path;

  // Controllo che il chunk non sia nullo
  if (cfg.chunk_size == 0)
    return false;

  std::error_code ec;
  // Leggo la file size
  const auto size = std::filesystem::file_size(input_path, ec);
  if (ec)
    return false;

  // Se la size è nulla termino subito
  if (size == 0) {
    std::filesystem::path out = input_path;
    out += cfg.out_ext;
    FILE *f = std::fopen(out.string().c_str(), "wb");
    if (!f)
      return false;
    std::fclose(f);
    return true;
  }

  // Scrivo il numero massimo di chunk
  ctx.max_chunks =
      static_cast<std::size_t>((size + cfg.chunk_size - 1) / cfg.chunk_size);

  // Porto il buffer e i token alla stessa dimensione
  ctx.buf.resize(ctx.max_chunks);
  ctx.write_token.resize(ctx.max_chunks);

  // Creo il filename dell'output
  std::filesystem::path out = input_path;
  out += cfg.out_ext;

  // Apro il file descriptor dell'output
  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out)
    return false;

  // For non parallelizzato poiché l'allocazione è piccola
  // new non scala bene, le allocazioni dinamiche contendono l'allocator
  for (std::size_t i = 0; i < ctx.max_chunks; ++i)
    ctx.buf[i] = std::make_unique<chunk_entry>();

  // Viene creato un team di thread OpenMP
#pragma omp parallel
  // Un solo thread esegue questo blocco e si pone da orchestratore del tutto
  // Sarà lui a creare task e dare lavoro ai worker
#pragma omp single
  {
    // Itera su tutti i chunk e per ciascun chunk crea un task di compressione
    // ed un task di scrittura
    for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq) {

      // WORKER: lettura + compressione (parallela)
      // Crea un task che può essere eseguito da qualsiasi thread nel team
      // Ogni task riceve una copia privata di seq
      // Segnala al runtime che questo task produce la dipendenza per buf[seq]
#pragma omp task firstprivate(seq) depend(out : ctx.buf[seq])
      {
        chunk_task(ctx, seq);
      }

      // WRITER: serializzato e ordinato
      // Nel caso sia il primo non parte finché il chunk non è pronto
      // Usa e produce il token 0 per dare inizio a tutto
      if (seq == 0) {
#pragma omp task firstprivate(seq) depend(in : ctx.buf[seq])                   \
    depend(inout : ctx.write_token[seq])
        {
          writer_write_one_chunk(ctx, seq);
        }
      } else {
#pragma omp task firstprivate(seq) depend(in : ctx.buf[seq])                   \
    depend(in : ctx.write_token[seq - 1]) depend(inout : ctx.write_token[seq])
        {
          writer_write_one_chunk(ctx, seq);
        }
      }
    }
  }

  std::fclose(ctx.fd_out);

  if (cfg.remove_input)
    std::filesystem::remove(input_path);

  return true;
}

} // namespace compressor
