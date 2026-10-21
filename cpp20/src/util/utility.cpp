#include "util/utility.hpp"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace util {

std::vector<std::uint8_t> read_chunk(const std::string &path,
                                     std::size_t offset, std::size_t max_len) {
  // Apro il file per questo thread
  // Se ho `f` è NULL ritorno l'errore ricevuto
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    throw std::runtime_error("read_chunk: cannot open file '" + path + "' (" +
                             std::strerror(errno) + ")");
  }

  // Seek sicuro a 64 bit
  // Per via del fatto che se usassi un long normale per indicizzare al
  // massimo potrei arrivare a offset di 2GB per file, che sarebbe ok per
  // architetture a 32 bit ma non per 64 bit
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
    std::fclose(f);
    throw std::runtime_error("read_chunk: fseeko failed at offset " +
                             std::to_string(offset) + " (" +
                             std::strerror(errno) + ")");
  }

  // Buffer di lettura
  // Con il numero massimo di byte che ogni chunk ha
  std::vector<std::uint8_t> buf(max_len);

  // Lettura effettiva
  size_t n = std::fread(buf.data(), 1, max_len, f);

  // Controllo errori
  // Torno errore poiché la read potrebbe fallire non solo perché arrivata
  // alla fine del file
  if (n < max_len && std::ferror(f)) {
    std::fclose(f);
    throw std::runtime_error("read_chunk: fread failed (" +
                             std::string(std::strerror(errno)) + ")");
  }

  std::fclose(f);

  // Ridimensiono al numero reale di byte letti
  buf.resize(n);
  return buf;
}

void write_chunk(std::FILE *f, const std::uint8_t *data, std::size_t len) {

  if (!f) {
    throw std::runtime_error("write_chunk: FILE* is null");
  }

  size_t n = std::fwrite(data, 1, len, f);

  if (n != len) {
    throw std::runtime_error("write_chunk: fwrite failed (" +
                             std::string(std::strerror(errno)) + ")");
  }
}

} // namespace util
