#include "decompressor/small_file.hpp"
#include "util/utility.hpp"

namespace decompressor {

bool decompress_small_file(const std::filesystem::path &input_path) {
  return doWork(input_path.string().c_str());
}

} // namespace decompressor
