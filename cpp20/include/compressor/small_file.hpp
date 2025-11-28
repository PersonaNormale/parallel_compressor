#pragma once

#include <filesystem>

namespace compressor {

/// Esegue la compressione sequenziale di un file "small"
/// Viene usato come task atomico da inserire nella thread pool.
///
/// Ritorna true se la compressione ha avuto successo, false in caso di errore.
bool compress_small_file(const std::filesystem::path& input_path);

} // namespace compressor
