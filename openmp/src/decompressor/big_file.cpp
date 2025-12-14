#include "decompressor/big_file.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <cerrno>
#include <cstring>
#include <omp.h>
#include <stdexcept>

namespace decompressor {

/// Decomprime un singolo buffer compresso in memoria
decompressed_chunk decompress_buffer(const decompressed_chunk &compressed) {
  // Dichiaro la size da passare alla funzione di decompressione
  size_t out_size = 0;
  // Prendo il vettore dei byte decompressi
  void *out = tinfl_decompress_mem_to_heap(compressed.data(), // input buffer
                                           compressed.size(), // input size
                                           &out_size,         // output size
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);

  // Se è nullo o la sua siz è zero
  if (!out || out_size == 0)
    throw std::runtime_error("decompression failed");

  // Inizializzo il risultato alla dimensione ritornatami da miniz inflate
  decompressed_chunk res(out_size);
  // Copio l'output in res
  std::memcpy(res.data(), out, out_size);
  // Libero la memoria
  mz_free(out);
  return res;
}

/// Costruisce il path di output
std::filesystem::path make_output_path(const std::filesystem::path &input) {
  const std::string s = input.string();
  const std::string &ext = cfg.out_ext;

  if (!ext.empty() && s.size() >= ext.size() &&
      s.rfind(ext) == s.size() - ext.size())
    return std::filesystem::path{s.substr(0, s.size() - ext.size())};

  return std::filesystem::path{s + ".raw"};
}

/// Scansiona il file compresso e costruisce index[] = {len, offset_dati}
bool build_index(FILE *f, std::vector<chunk_index> &index_out) {
  // Se il puntatore a file è vuoto ritorna falso
  if (!f)
    return false;

  // Pulisce index_out
  // TODO vedere se ancora utile
  index_out.clear();

  // Mi assicuro di posizionare il cursore all'inizio del file
  if (fseeko(f, 0, SEEK_SET) != 0)
    return false;

  // Inizializzo il Current Offset
  std::uint64_t curr_off = 0;

  for (;;) {
    std::uint64_t len = 0;
    // Lettura della lunghezza del chunk
    const size_t n = std::fread(&len, 1, sizeof(len), f);

    if (n == 0) {
      if (std::feof(f))
        break; // EOF
      return false;
    }

    if (n != sizeof(len))
      return false;

    // L'offset dei dati è curr_off più la size della sua lunghezza
    const std::uint64_t data_offset = curr_off + sizeof(len);

    // Metto in index_out la lunghezza del chunk e dove si trova
    index_out.push_back(chunk_index{len, data_offset});

    // Aggiorno il current offset
    curr_off = data_offset + len;

    // Avanzo nel file
    if (fseeko(f, static_cast<off_t>(len), SEEK_CUR) != 0)
      return false;
  }

  return true;
}

/// Task di decompressione (worker della farm)
void chunk_task(big_file_context &ctx, std::size_t seq) {
  // Prendo la referenza al numero di sequenza assegnato al chunk
  const chunk_index &meta = ctx.index[seq];

  // Leggo il chunk compresso dal file tramite l'utility
  auto compressed = util::read_chunk(ctx.input_path.string(),
                                     static_cast<std::size_t>(meta.offset),
                                     static_cast<std::size_t>(meta.len));

  // Prendo i raw byte datimi dalla decompressione del buffer
  auto raw = decompress_buffer(compressed);

  // Prendo la referenza a dove devo posizionare il chunk decompresso
  dechunk_entry *e = ctx.buf[seq];
  // Muovo i raw bytes nella locazione
  e->data = std::move(raw);
}

/// Writer del singolo chunk (collector ordinato)
static void writer_write_one_chunk(big_file_context &ctx, std::size_t seq) {
  // Prendo la referenza ad una chunk entry e ad una sua entrata
  dechunk_entry *e = ctx.buf[seq];
  auto &block = e->data;

  // Se il blocco non è vuoto lo scrivo sul fd
  if (!block.empty())
    util::write_chunk(ctx.fd_out, block.data(), block.size());
}

/// Orchestratore
bool decompress_big_file(const std::filesystem::path &input_path) {
  // Dichiaro il contesto e inizializzo l'input path
  big_file_context ctx{};
  ctx.input_path = input_path;

  // Apre input compresso
  FILE *f = std::fopen(input_path.string().c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "Error: cannot open input file: %s\n",
                 input_path.string().c_str());
    return false;
  }

  // Si creano gli indici di lavoro
  const bool ok = build_index(f, ctx.index);
  std::fclose(f);
  if (!ok)
    return false;

  // Assendo il numero massimo di chunk
  ctx.max_chunks = ctx.index.size();

  // Path di output
  std::filesystem::path out = make_output_path(input_path);

  if (cfg.verbose) {
    std::fprintf(stderr, "[D] %s -> %s\n", input_path.string().c_str(),
                 out.string().c_str());
  }

  // Apro il file di output
  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out) {
    std::fprintf(stderr, "Error: cannot open output file: %s\n",
                 out.string().c_str());
    return false;
  }

  // Controllo che non sia vuoto
  if (ctx.max_chunks == 0) {

    if (cfg.verbose)
      std::fprintf(stderr, "Skip, empty file: %s\n",
                   input_path.string().c_str());

    std::fclose(ctx.fd_out);
    ctx.fd_out = nullptr;
    return true;
  }

  // Alloca buffer
  ctx.buf.resize(ctx.max_chunks);
  for (std::size_t i = 0; i < ctx.max_chunks; ++i)
    ctx.buf[i] = new dechunk_entry{};

  // Parallelizzazione OpenMP: decompressione + scrittura ordinata
#pragma omp parallel
#pragma omp single
  {
    for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq) {

      // WORKER produce il chunk seq
      // Prende il suo seq e dice che buf[seq] dipende da lui
#pragma omp task firstprivate(seq) depend(out : ctx.buf[seq])
      {
        chunk_task(ctx, seq);
      }

      // COLLECTOR: scrive il chunk seq in ordine
      // Prende il suo seq e si mette in attesa che la dipendenza buf[seq] sia
      // soddisfatta
#pragma omp task firstprivate(seq) depend(in : ctx.buf[seq])
      {
        writer_write_one_chunk(ctx, seq);
      }
    }
  }

  // Cleanup
  if (ctx.fd_out) {
    std::fclose(ctx.fd_out);
    ctx.fd_out = nullptr;
  }

  // Pulisco il buffer
  for (auto *e : ctx.buf)
    delete e;

  // Rimuovi input compresso se richiesto
  if (cfg.remove_input) {
    std::error_code ecr;
    std::filesystem::remove(input_path, ecr);
    if (ecr && cfg.verbose)
      std::fprintf(stderr, "Warning: file %s has not been removed (%s)\n",
                   input_path.string().c_str(), ecr.message().c_str());
  }

  return true;
}

} // namespace decompressor
