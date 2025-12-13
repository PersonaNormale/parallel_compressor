#include "compressor/big_file.hpp"
#include "compressor/compressor.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <cstdio>
#include <filesystem>
#include <omp.h>
#include <vector>

namespace compressor {

compressed_chunk compress_buffer(const std::vector<unsigned char> &raw) {
  size_t out_size = 0;

  void *out = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &out_size,
                                         TDEFL_WRITE_ZLIB_HEADER);

  if (!out || out_size == 0)
    throw std::runtime_error("compression failed");

  compressed_chunk res(out_size);
  memcpy(res.data(), out, out_size);
  mz_free(out);
  return res;
}

void chunk_task(big_file_context &ctx, std::size_t seq) {
  const auto offset = util::chunk_offset(seq, cfg.chunk_size);
  auto raw = util::read_chunk(ctx.input_path.string(), offset, cfg.chunk_size);

  auto compressed = compress_buffer(raw);

  chunk_entry *e = ctx.buf[seq];
  e->data = std::move(compressed);
}

// Scrittura del singolo chunk (collector ordinato)
static void writer_write_one_chunk(big_file_context &ctx, std::size_t seq) {
  chunk_entry *ce = ctx.buf[seq];
  const auto &vec = ce->data;

  // Scrivi lunghezza del chunk compresso
  std::uint64_t len = vec.size();
  if (std::fwrite(&len, sizeof(len), 1, ctx.fd_out) != 1) {
    // gestione errore
  }

  // Scrivi i dati compressi
  if (!vec.empty()) {
    size_t written = std::fwrite(vec.data(), 1, vec.size(), ctx.fd_out);
    if (written != vec.size()) {
      // gestione errore
    }
  }
}

// Orchestratore di alto livello
bool compress_big_file(const std::filesystem::path &input_path) {
  big_file_context ctx{};
  ctx.input_path = input_path;

  if (cfg.chunk_size == 0) {
    std::fprintf(stderr, "Error: chunk size must be greater than 0.\n");
    return false;
  }

  // Determina dimensione file
  std::error_code ec;
  const auto size = std::filesystem::file_size(input_path, ec);
  if (ec) {
    std::fprintf(stderr, "Error: cannot stat input file: %s\n",
                 input_path.string().c_str());
    return false;
  }

  // Caso file vuoto
  if (size == 0) {
    std::filesystem::path out = input_path;
    out += cfg.out_ext;
    FILE *f = std::fopen(out.string().c_str(), "wb");
    if (!f)
      return false;
    std::fclose(f);
    return true;
  }

  // Numero chunk
  ctx.max_chunks =
      static_cast<std::size_t>((size + cfg.chunk_size - 1) / cfg.chunk_size);

  // Alloca buffer
  ctx.buf.resize(ctx.max_chunks);
  try {
    for (std::size_t i = 0; i < ctx.max_chunks; ++i)
      ctx.buf[i] = new chunk_entry{};
  } catch (...) {
    for (auto *e : ctx.buf)
      delete e;
    throw;
  }

  // Apre output
  std::filesystem::path out = input_path;
  out += cfg.out_ext;
  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out) {
    for (auto *e : ctx.buf)
      delete e;
    return false;
  }

  // Parallelizzazione OpenMP (pattern farm + collector)
#pragma omp parallel
#pragma omp single
  {
    for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq) {

      // Worker
#pragma omp task firstprivate(seq) depend(out : ctx.buf[seq])
      {
        chunk_task(ctx, seq);
      }

      // Collector
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

  for (auto *e : ctx.buf)
    delete e;

  ctx.buf.clear();

  if (cfg.remove_input) {
    std::error_code ecr;
    std::filesystem::remove(input_path, ecr);
    if (ecr && cfg.verbose)
      std::fprintf(stderr, "Warning: file %s has not been removed (%s)\n",
                   input_path.string().c_str(), ecr.message().c_str());
  }

  return true;
}

} // namespace compressor
