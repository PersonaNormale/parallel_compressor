#pragma once

#include <filesystem>

namespace decompressor {

/// Esegue la decompressione sequenziale di un file "small".
/// Può essere usata come task atomico da inserire nella thread pool.
///
/// Ritorna true se la decompressione ha avuto successo, false in caso di
/// errore.
bool decompress_small_file(const std::filesystem::path &input_path);

} // namespace decompressor
