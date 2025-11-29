#include "decompressor/big_file.hpp"

#include "util/config.hpp"
#include "util/utility.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace decompressor {

namespace {

// Decomprime un singolo buffer compresso in memoria
decompressed_chunk decompress_buffer(const decompressed_chunk &decompressed) {
  size_t out_size = 0;
  void *out = tinfl_decompress_mem_to_heap(
      decompressed.data(),         // input buffer
      decompressed.size(),         // input size
      &out_size,                   // output size
      TINFL_FLAG_PARSE_ZLIB_HEADER // ZLIB header, speculare al compressore
  );

  if (!out || out_size == 0)
    throw std::runtime_error("decompression failed");

  decompressed_chunk res(out_size);
  std::memcpy(res.data(), out, out_size);
  mz_free(out);
  return res;
}

// Costruisce il path di output a partire da quello di input
std::filesystem::path make_output_path(const std::filesystem::path &input) {
  const std::string s = input.string();
  const std::string &ext = cfg.out_ext;

  // Se l'input termina con cfg.out_ext, rimuovila
  if (!ext.empty() && s.size() >= ext.size() &&
      s.rfind(ext) == s.size() - ext.size())
    return std::filesystem::path{s.substr(0, s.size() - ext.size())};

  // Fallback: aggiungi ".raw"
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

  std::uint64_t curr_off = 0; // offset logico nel file

  for (;;) {
    std::uint64_t len = 0;
    const size_t n = std::fread(&len, 1, sizeof(len), f);

    if (n == 0) {
      // EOF pulito
      if (std::feof(f))
        break;
      // errore di I/O
      return false;
    }

    // lettura parziale della len file corrotto
    if (n != sizeof(len))
      return false;

    const std::uint64_t data_offset = curr_off + sizeof(len);
    index_out.push_back(chunk_index{len, data_offset});

    // avanza: [len][chunk]
    curr_off = data_offset + len;

    // salta il blocco compresso
    if (fseeko(f, static_cast<off_t>(len), SEEK_CUR) != 0)
      return false;
  }

  return true;
}

// Task di decompressione di un singolo chunk
void chunk_task(big_file_context &ctx, std::size_t seq) {
  const chunk_index &meta = ctx.index[seq];

  // Legge il blocco compresso dal file
  auto decompressed = util::read_chunk(ctx.input_path.string(),
                                       static_cast<std::size_t>(meta.offset),
                                       static_cast<std::size_t>(meta.len));

  // Decompressione in memoria
  auto raw = decompress_buffer(decompressed);

  // Pubblica il risultato nel buffer condiviso
  dechunk_entry *e = ctx.buf[seq];
  e->data = std::move(raw);
  e->ready.store(true, std::memory_order_release);

  // Se questo chunk sblocca l'avanzamento del writer, notifica
  const auto c = ctx.curr.load(std::memory_order_acquire);
  if (seq == c) {
    std::lock_guard<std::mutex> lk(ctx.cv_mtx);
    ctx.cv.notify_one();
  }
}

// Writer sequenziale: scrive i chunk decompattati in ordine
void writer_task(big_file_context &ctx) {
  std::unique_lock<std::mutex> lk(ctx.cv_mtx);

  while (true) {
    // Attende:
    // 1. chunk corrente pronto, oppure
    // 2. tutti i chunk schedulati e nessun lavoro residuo
    ctx.cv.wait(lk, [&] {
      std::size_t c = ctx.curr.load(std::memory_order_acquire);

      if (c < ctx.max_chunks &&
          ctx.buf[c]->ready.load(std::memory_order_acquire))
        return true;

      if (ctx.all_chunks_scheduled.load(std::memory_order_acquire) &&
          c == ctx.max_chunks)
        return true;

      return false;
    });

    std::size_t c = ctx.curr.load(std::memory_order_acquire);

    // Caso: chunk corrente pronto
    if (c < ctx.max_chunks &&
        ctx.buf[c]->ready.load(std::memory_order_acquire)) {

      // Trova una run contigua [c, end) di chunk pronti
      std::size_t end = c;
      while (end < ctx.max_chunks &&
             ctx.buf[end]->ready.load(std::memory_order_acquire))
        ++end;

      // Copia/muove i dati in un vettore locale
      std::vector<decompressed_chunk> local;
      local.reserve(end - c);
      for (std::size_t i = c; i < end; ++i)
        local.push_back(std::move(ctx.buf[i]->data));

      // Aggiorna il prossimo chunk da scrivere
      ctx.curr.store(end, std::memory_order_release);

      // Scrittura fuori dalla sezione critica
      lk.unlock();
      for (auto &block : local)
        util::write_chunk(ctx.fd_out, block.data(), block.size());
      lk.lock();

      continue;
    }

    // Se siamo qui, il chunk corrente NON è pronto.
    // Con il predicato della wait, l'unica possibilità è:
    // all_chunks_sched == true e curr == max_chunks.
    break;
  }

  lk.unlock();

  if (ctx.fd_out) {
    std::fclose(ctx.fd_out);
    ctx.fd_out = nullptr;
  }
}

// Orchestratore per la decompressione di un big file
bool decompress_big_file(threadPool &pool,
                         const std::filesystem::path &input_path) {
  big_file_context ctx{};
  ctx.pool = &pool;
  ctx.input_path = input_path;

  // Apre input compresso
  FILE *f = std::fopen(input_path.string().c_str(), "rb");
  if (!f)
    return false;

  const bool ok = build_index(f, ctx.index);
  std::fclose(f);
  if (!ok)
    return false;

  ctx.max_chunks = ctx.index.size();

  // Costruisce il path di output
  std::filesystem::path out = make_output_path(input_path);
  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out)
    return false;

  // Caso file vuoto: nessun chunk
  if (ctx.max_chunks == 0) {
    std::fclose(ctx.fd_out);
    ctx.fd_out = nullptr;
    return true;
  }

  // Alloca il buffer dei chunk
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

  ctx.curr.store(0, std::memory_order_relaxed);
  ctx.all_chunks_scheduled.store(false, std::memory_order_relaxed);

  // Lancia il writer
  auto writer_future = pool.submit([&ctx] { writer_task(ctx); });

  // Lancia i task di decompressione
  for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq)
    pool.submit([&ctx, seq] { chunk_task(ctx, seq); });

  // Segnala che non verranno schedulati altri chunk
  ctx.all_chunks_scheduled.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(ctx.cv_mtx);
    ctx.cv.notify_one();
  }

  // Attende il completamento del writer
  writer_future.get();

  // Cleanup
  for (auto *e : ctx.buf)
    delete e;
  ctx.buf.clear();

  // Rimuovo gli input file se richiesto
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
