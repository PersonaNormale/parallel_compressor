#include "decompressor/big_file.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <cerrno>
#include <cstring>
#include <omp.h>
#include <stdexcept>

namespace decompressor {

namespace {

// Decomprime un singolo buffer compresso in memoria
decompressed_chunk decompress_buffer(const decompressed_chunk &compressed) {
  size_t out_size = 0;
  void *out = tinfl_decompress_mem_to_heap(compressed.data(), // input buffer
                                           compressed.size(), // input size
                                           &out_size,         // output size
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);

  if (!out || out_size == 0)
    throw std::runtime_error("decompression failed");

  decompressed_chunk res(out_size);
  std::memcpy(res.data(), out, out_size);
  mz_free(out);
  return res;
}

// Costruisce il path di output
std::filesystem::path make_output_path(const std::filesystem::path &input) {
  const std::string s = input.string();
  const std::string &ext = cfg.out_ext;

  if (!ext.empty() && s.size() >= ext.size() &&
      s.rfind(ext) == s.size() - ext.size())
    return std::filesystem::path{s.substr(0, s.size() - ext.size())};

  return std::filesystem::path{s + ".raw"};
}

} // namespace

// Scansiona il file compresso e costruisce index[] = {len, offset_dati}
bool build_index(FILE *f, std::vector<chunk_index> &index_out) {
  if (!f)
    return false;

  index_out.clear();

  if (fseeko(f, 0, SEEK_SET) != 0)
    return false;

  std::uint64_t curr_off = 0;

  for (;;) {
    std::uint64_t len = 0;
    const size_t n = std::fread(&len, 1, sizeof(len), f);

    if (n == 0) {
      if (std::feof(f))
        break; // EOF
      return false;
    }

    if (n != sizeof(len))
      return false;

    const std::uint64_t data_offset = curr_off + sizeof(len);

    index_out.push_back(chunk_index{len, data_offset});

    curr_off = data_offset + len;

    if (fseeko(f, static_cast<off_t>(len), SEEK_CUR) != 0)
      return false;
  }

  return true;
}

// Task di decompressione (worker della farm)
void chunk_task(big_file_context &ctx, std::size_t seq) {
  const chunk_index &meta = ctx.index[seq];

  auto compressed = util::read_chunk(ctx.input_path.string(),
                                     static_cast<std::size_t>(meta.offset),
                                     static_cast<std::size_t>(meta.len));

  auto raw = decompress_buffer(compressed);

  dechunk_entry *e = ctx.buf[seq];
  e->data = std::move(raw);
}

// Writer del singolo chunk (collector ordinato)
static void writer_write_one_chunk(big_file_context &ctx, std::size_t seq) {
  dechunk_entry *e = ctx.buf[seq];
  auto &block = e->data;

  if (!block.empty())
    util::write_chunk(ctx.fd_out, block.data(), block.size());

}

// Funzione di alto livello: decomprime il big file con OpenMP
bool decompress_big_file(const std::filesystem::path &input_path) {
  big_file_context ctx{};
  ctx.input_path = input_path;

  // Apre input compresso
  FILE *f = std::fopen(input_path.string().c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "Error: cannot open input file: %s\n",
                 input_path.string().c_str());
    return false;
  }

  const bool ok = build_index(f, ctx.index);
  std::fclose(f);
  if (!ok)
    return false;

  ctx.max_chunks = ctx.index.size();

  // Path di output
  std::filesystem::path out = make_output_path(input_path);

  if (cfg.verbose) {
    std::fprintf(stderr, "[D] %s -> %s\n", input_path.string().c_str(),
                 out.string().c_str());
  }

  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out) {
    std::fprintf(stderr, "Error: cannot open output file: %s\n",
                 out.string().c_str());
    return false;
  }

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
  try {
    for (std::size_t i = 0; i < ctx.max_chunks; ++i)
      ctx.buf[i] = new dechunk_entry{};
  } catch (...) {
    for (auto *e : ctx.buf)
      delete e;
    ctx.buf.clear();
    throw;
  }

  // Parallelizzazione OpenMP: decompressione + scrittura ordinata
#pragma omp parallel
#pragma omp single
  {
    for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq) {

      // worker → produce il chunk seq
#pragma omp task firstprivate(seq) depend(out : ctx.buf[seq])
      {
        chunk_task(ctx, seq);
      }

      // collector → scrive il chunk seq in ordine
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
