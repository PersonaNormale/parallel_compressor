#include "compressor/big_file.hpp"
#include "compressor/compressor.hpp"
#include "threadpool/threadPool.hpp"
#include "util/config.hpp"
#include "util/utility.hpp"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace compressor {

namespace {
// TODO Da mettere dipendente al threshold per la dimensione dello small file
constexpr std::size_t CHUNK_SIZE = 256 * 1024;
} // namespace

compressed_chunk compress_buffer(const std::vector<unsigned char> &raw) {
  size_t out_size = 0;

  void *out = tdefl_compress_mem_to_heap(
      raw.data(),             // input buffer
      raw.size(),             // input size
      &out_size,              // output size written
      TDEFL_WRITE_ZLIB_HEADER // o 0 se vuoi raw DEFLATE
  );

  if (!out || out_size == 0)
    throw std::runtime_error("compression failed");

  compressed_chunk res;
  res.resize(out_size);
  memcpy(res.data(), out, out_size);

  mz_free(out);
  return res;
}

void chunk_task(big_file_context &ctx, std::size_t seq) {

  // Calcolo da dove iniziare a leggere
  const auto offset = util::chunk_offset(seq, CHUNK_SIZE);

  // Prendo i byte raw da compressare
  auto raw = util::read_chunk(ctx.input_path.string(), offset, CHUNK_SIZE);
  auto compressed = compress_buffer(raw);

  // Prendo il puntatore alla struttura dati
  // Scrivo i dati
  // Con memory_order_release mi accerto di impostare ready a true
  // unicamente quando pronto quando tutti i dati sono scritti
  // Già da qui il writer può vedere che il chunk è pronto
  chunk_entry *e = ctx.buf[seq];
  e->data = std::move(compressed);
  e->ready.store(true, std::memory_order_release);

  // Prendo il chunk aspettato in questo momento
  const auto c = ctx.curr.load(std::memory_order_acquire);
  // Se è in questo task allora prendo la lock e notifico il writer
  if (seq == c) {
    std::lock_guard<std::mutex> lk(ctx.cv_mtx);
    ctx.cv.notify_one();
  }
}

void writer_task(big_file_context &ctx) {
  // Prende il mutex sulla CV
  std::unique_lock<std::mutex> lk(ctx.cv_mtx);

  while (true) {

    // Aspetta sulla lock finché:
    //   prendendo il valore del prossimo chunk da scrivere prima delle altre
    //   operazioni di load
    //   1. siamo sotto i chunk massimi e il chunk corrente è pronto
    //   oppure
    //   2. tutti i chunks sono stati schedulati e non c'è più lavoro
    ctx.cv.wait(lk, [&] {
      size_t c = ctx.curr.load(std::memory_order_acquire);

      if (c < ctx.max_chunks &&
          ctx.buf[c]->ready.load(std::memory_order_acquire))
        return true;

      if (ctx.all_chunks_scheduled.load(std::memory_order_acquire) &&
          c == ctx.max_chunks)
        return true;

      return false;
    });

    // Prima di tutte le operazioni di load riprendi il valore del chunk
    // corrente da scrivere
    std::size_t c = ctx.curr.load(std::memory_order_acquire);

    // Caso: chunk pronto
    // Qui creeremo la window di chunk pronti più lunga possibile per una write
    // consecutiva di chunk
    // L'intervallo sarà [current_chunk, end)
    if (c < ctx.max_chunks &&
        ctx.buf[c]->ready.load(std::memory_order_acquire)) {
      // La fine per ora è il prossimo chunk da scrivere
      std::size_t end = c;
      // Finché siamo sotto la fine dei chunks e il prossimo chunk è pronto
      // Maggiore la fine di 1
      while (end < ctx.max_chunks &&
             ctx.buf[end]->ready.load(std::memory_order_acquire))
        ++end;

      // Vettore locale di chunk compressi
      std::vector<compressed_chunk> local;
      // Riservo solo le posizioni dei chunk pronti
      local.reserve(end - c);
      // Per ofni posizione nel buffer faccio una move dei dati nel buffer
      // locale dei chunks pronti
      for (std::size_t i = c; i < end; ++i)
        local.push_back(std::move(ctx.buf[i]->data));

      // Dico che il chunk corrente per continuare è end
      ctx.curr.store(end, std::memory_order_release);

      // Libero la lock fino a che non è pronto
      lk.unlock();
      // Inizio la fase di scrittura fuori area critica
      // Scrivo come metadato la lunghezza del chunk compresso
      // così che alla decompressione si sappia quanto è grande il chunk da
      // decomprimere
      for (auto &cc : local) {
        // Lunghezza del chunk compresso in byte (uint64_t per portabilità)
        const std::uint64_t len = static_cast<std::uint64_t>(cc.size());

        // Scrive sul file la lunghezza del chunk (8 byte)
        // In modo che il decompressore sappia quanti byte leggere dopo.
        // TODO indicizzazione 8 byte esagereta, dopo trovare metodo migliore
        // possibile rifare tutto a 32 bit
        util::write_chunk(ctx.fd_out,
                          reinterpret_cast<const std::uint8_t *>(&len),
                          sizeof(len));

        // Scrive il contenuto del chunk compresso
        util::write_chunk(ctx.fd_out, cc.data(), cc.size());
      }

      // riprendo la lock e alla prossima iterazione mi rimetto in attesa sulla
      // CV
      lk.lock();

      continue;
    }

    // Caso: tutti schedulati e tutti scritti
    // Se siamo qui significa che il chunk corrente NON è pronto.
    // Con il predicato della wait, l'unico modo per essere qui è
    // all_chunks_scheduled == true e curr == max_chunks.
    break;
  }

  // Libero la lock sulla CV
  lk.unlock();
  // Controllo che il file descriptor sia a posto e lo chiudo, poi lo setto a
  // null per non lasciarlo dangling
  if (ctx.fd_out) {
    std::fclose(ctx.fd_out);
    ctx.fd_out = nullptr;
  }
}

bool compress_big_file(threadPool &pool,
                       const std::filesystem::path &input_path) {
  big_file_context ctx{};
  ctx.pool = &pool;
  ctx.input_path = input_path;

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

  // Numero di chunk
  ctx.max_chunks =
      static_cast<std::size_t>((size + CHUNK_SIZE - 1) / CHUNK_SIZE);

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

  ctx.curr.store(0, std::memory_order_relaxed);
  ctx.all_chunks_scheduled.store(false, std::memory_order_relaxed);

  // Apre output
  std::filesystem::path out = input_path;
  out += cfg.out_ext;
  ctx.fd_out = std::fopen(out.string().c_str(), "wb");
  if (!ctx.fd_out) {
    for (auto *e : ctx.buf)
      delete e;
    return false;
  }

  // Lancia il writer prima dei produttori
  auto writer_future = pool.submit([&ctx] { writer_task(ctx); });

  // Lancia tutti i chunk_task
  for (std::size_t seq = 0; seq < ctx.max_chunks; ++seq)
    pool.submit([&ctx, seq] { chunk_task(ctx, seq); });

  // Segnala che nessun nuovo chunk verrà schedulato
  ctx.all_chunks_scheduled.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(ctx.cv_mtx);
    ctx.cv.notify_one();
  }

  // Attende completamento writer
  writer_future.get();

  // Cleanup
  for (auto *e : ctx.buf)
    delete e;
  ctx.buf.clear();

  return true;
}

} // namespace compressor
