#include "compressor/small_file.hpp"
#include "util/utility.hpp"

namespace compressor {

  bool compress_small_file(const std::filesystem::path& input_path) {
      return doWork(input_path.string().c_str());
  }

} // namespace compressor
