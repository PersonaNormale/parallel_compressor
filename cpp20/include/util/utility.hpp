#pragma once
#include "config.hpp"
#include "miniz.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

inline bool isDirectory(const char *path) {
  std::error_code ec;
  const fs::path p(path);
  // check symlinks
  const auto st = fs::symlink_status(p, ec);
  if (ec)
    return false;
  // true only for real directories (a symlink-to-dir returns false)
  if (fs::is_directory(st))
    return true;
  return false;
}

inline bool should_process(const std::filesystem::path &p) {
  const std::string ext = p.extension().string(); // file extension
  if (cfg.mode == COMP) {
    if (ext == cfg.out_ext || ext == ".tmp")
      return false; // already compressed or temporary
    return true;
  } else {                       // DECOMP
    return (ext == cfg.out_ext); // decompress only files with proper suffix
  }
}

namespace {
constexpr std::size_t CHUNK = 256 * 1024;

/**
 * Compress an input byte stream to an output byte stream using zlib/DEFLATE.
 *
 * Reads in fixed-size chunks (CHUNK), feeds them to zlib's deflate(), and
 * writes the produced compressed bytes to out.
 *
 * Returns:
 *  - true  on success (no zlib or I/O errors encountered).
 *  - false if deflate() signals an error (e.g., Z_STREAM_ERROR) or if writing
 * to out fails.
 *
 * Notes:
 *  - This is a streaming compressor: it does not need the input size in advance
 * and uses bounded memory (two CHUNK-sized buffers)
 *  - deflateEnd() is always called to release zlib resources before returning
 *  - On the last iteration, flush = Z_FINISH ensures the compressed stream is
 * properly terminated; the code breaks out when Z_STREAM_END is reached
 */
inline bool compress_stream(std::istream &in, std::ostream &out, int level) {
  z_stream s{};

  // Initializes a zlib stream with deflateInit(level).
  if (deflateInit(&s, level) != Z_OK)
    return false;

  std::vector<unsigned char> inbuf(CHUNK), outbuf(CHUNK);
  bool ok = true;
  int zret = Z_OK;

  while (in) {
    // Repeatedly reads up to CHUNK bytes from in into an input buffer
    in.read(reinterpret_cast<char *>(inbuf.data()),
            static_cast<std::streamsize>(inbuf.size()));
    s.avail_in = static_cast<uInt>(in.gcount());
    s.next_in = inbuf.data();
    const int flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;

    // Calls deflate() with Z_NO_FLUSH for all intermediate chunks and Z_FINISH
    // on the final chunk (detected via in.eof()) to terminate the stream. After
    // each deflate() call, writes any produced bytes from the output buffer to
    // out.
    do {
      s.avail_out = static_cast<uInt>(outbuf.size());
      s.next_out = outbuf.data();
      zret = deflate(&s, flush);
      if (zret == Z_STREAM_ERROR) {
        ok = false;
        break;
      }
      const std::size_t have = outbuf.size() - s.avail_out;
      if (have)
        out.write(reinterpret_cast<const char *>(outbuf.data()),
                  static_cast<std::streamsize>(have));
      if (!out) {
        ok = false;
        break;
      }
    } while (s.avail_out ==
             0); // Keep calling deflate until output buffer not full

    // Stops when deflate() returns Z_STREAM_END during the final flush or upon
    // error.
    if (!ok)
      break;
    if (flush == Z_FINISH && zret == Z_STREAM_END)
      break;
  }
  deflateEnd(&s);
  return ok;
}

/**
 * Decompress a zlib stream from in to out using a bounded-buffer loop.
 *
 * Return value:
 *  - true  if the stream ends cleanly with Z_STREAM_END and there are no I/O
 * errors.
 *  - false on any zlib error (NEED_DICT/DATA_ERROR/MEM_ERROR/STREAM_ERROR),
 *    or if EOF is reached with no more progress possible (truncated stream),
 *    or on output I/O failure.
 *
 * Notes:
 *  - This expects a zlib stream (inflateInit). If the compressed data was
 * produced as gzip or raw-deflate, the way to initialize the stream is
 * different (not supported).
 *  - CHUNK controls the I/O granularity; memory usage is ~2*CHUNK + zlib state.
 *  - inflateEnd() is called exactly once on every exit path.
 */
inline bool decompress_stream(std::istream &in, std::ostream &out) {
  constexpr std::size_t CHUNK = 256 * 1024;
  std::vector<unsigned char> inbuf(CHUNK), outbuf(CHUNK);

  // Initializes a zlib inflate state (zlib wrapper, not raw/gzip).
  z_stream s{};
  if (inflateInit(&s) != Z_OK)
    return false;

  int zret = Z_OK;
  bool ok = true;

  for (;;) {
    // Repeatedly provides input to zlib only when its internal input buffer is
    // empty.
    if (s.avail_in == 0) {
      in.read(reinterpret_cast<char *>(inbuf.data()),
              static_cast<std::streamsize>(inbuf.size()));
      s.avail_in = static_cast<uInt>(in.gcount());
      s.next_in = inbuf.data();
    }

    // prepare the output
    s.avail_out = static_cast<uInt>(outbuf.size());
    s.next_out = outbuf.data();

    // Always calls inflate() once per iteration so it can eventually return
    // Z_STREAM_END even after EOF on the input stream.
    zret = inflate(&s, Z_NO_FLUSH);
    if (zret == Z_STREAM_END) {
      const std::size_t have = outbuf.size() - s.avail_out;
      if (have)
        out.write(reinterpret_cast<const char *>(outbuf.data()),
                  static_cast<std::streamsize>(have));
      ok = static_cast<bool>(out);
      inflateEnd(&s);
      return ok;
    }

    if (zret == Z_NEED_DICT || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR ||
        zret == Z_STREAM_ERROR) {
      inflateEnd(&s);
      return false;
    }

    // Writes any produced bytes to out after each inflate() call.
    const std::size_t have = outbuf.size() - s.avail_out;
    if (have) {
      out.write(reinterpret_cast<const char *>(outbuf.data()),
                static_cast<std::streamsize>(have));
      if (!out) {
        inflateEnd(&s);
        return false;
      }
    }

    // This is the case EOF but not END, Z_STREAM_END never reached
    if (s.avail_in == 0 && in.eof() && zret == Z_BUF_ERROR) {
      inflateEnd(&s);
      return false;
    }
    // keep going on
  }
}

inline fs::path make_output_path(const fs::path &in) {
  if (cfg.mode == COMP) {
    return fs::path(in.string() + cfg.out_ext);
  } else {
    const std::string s = in.string();
    const std::string &ext = cfg.out_ext;
    if (s.size() >= ext.size() && s.rfind(ext) == s.size() - ext.size()) {
      return fs::path{s.substr(0, s.size() - ext.size())};
    }
    return fs::path{s + ".raw"};
  }
}
} // namespace

// apply compression or decompression on a regular file
inline bool doWork(const char *path_cstr) {
  const fs::path in(path_cstr);
  if (!fs::exists(in)) {
    std::fprintf(stderr, "File disappear: %s\n", path_cstr);
    return false;
  }
  if (!fs::is_regular_file(in)) {
    if (cfg.verbose)
      std::fprintf(stderr, "Skip, not a regular file: %s\n", path_cstr);
    return true;
  }

  const fs::path out_final = make_output_path(in);
  const fs::path out_tmp = fs::path(out_final.string() + ".tmp");

  if (cfg.verbose) {
    if (cfg.mode == COMP)
      std::fprintf(stderr, "[C] %s -> %s\n", in.string().c_str(),
                   out_final.string().c_str());
    else
      std::fprintf(stderr, "[D] %s -> %s\n", in.string().c_str(),
                   out_final.string().c_str());
  }

  std::error_code ec_eq;
  if (fs::equivalent(in, out_final, ec_eq) && !ec_eq) {
    std::fprintf(stderr, "Error: Output coincide with input: %s\n",
                 out_final.string().c_str());
    return false;
  }

  std::ifstream ifs(in, std::ios::binary);
  if (!ifs) {
    std::fprintf(stderr, "Error: cannot open in input: %s\n",
                 in.string().c_str());
    return false;
  }
  std::ofstream ofs(out_tmp, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    std::fprintf(stderr, "Error: cannot open in output: %s\n",
                 out_tmp.string().c_str());
    return false;
  }

  bool ok = (cfg.mode == COMP) ? compress_stream(ifs, ofs, cfg.level)
                               : decompress_stream(ifs, ofs);
  ofs.close();

  if (!ok) {
    std::error_code ecc;
    fs::remove(out_tmp, ecc); // remove temporary file
    std::fprintf(stderr, "Error: %s on %s\n",
                 (cfg.mode == COMP ? "compression" : "decompression"),
                 in.string().c_str());
    return false;
  }

  // atomic raname, if it is not supported by the FS, fallback to copy+remove
  std::error_code ec;
  fs::rename(out_tmp, out_final, ec);
  if (ec) { // rename failed
    std::error_code ecw, ecr;
    fs::copy_file(out_tmp, out_final, fs::copy_options::overwrite_existing,
                  ecw);
    fs::remove(out_tmp, ecr); // remove temporary file
    if (ecw) {
      std::fprintf(stderr, "Error: impossible to write output: %s\n",
                   out_final.string().c_str());
      return false;
    }
  }
  // do we have to remove the input file?
  if (cfg.remove_input) {
    std::error_code ecr;
    fs::remove(in, ecr);
    if (ecr && cfg.verbose)
      std::fprintf(stderr, "Warning: file %s has not been removed (%s)\n",
                   in.string().c_str(), ecr.message().c_str());
  }
  return true;
}

// walking the directory recursively (if -r 1)
inline bool walkDir(const char *dir_cstr) {
  const fs::path root(dir_cstr);
  if (!fs::exists(root) || !fs::is_directory(root)) {
    std::fprintf(stderr, "Non è una directory: %s\n", dir_cstr);
    return false;
  }

  bool all_ok = true;
  auto handle = [&](const fs::path &p) {
    if (!should_process(p)) {
      if (cfg.verbose)
        std::fprintf(stderr, "Skip: %s\n", p.string().c_str());
      return;
    }
    all_ok &= doWork(p.string().c_str());
  };

  if (cfg.recursive) {
    fs::directory_options opts = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(root, opts),
              end = fs::recursive_directory_iterator();
         it != end; ++it) {
      std::error_code ec;
      if (it->is_symlink(ec) || it->is_directory(ec))
        continue;
      if (it->is_regular_file(ec))
        handle(it->path());
    }
  } else {
    for (auto &de : fs::directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
      std::error_code ec;
      if (de.is_regular_file(ec))
        handle(de.path());
    }
  }
  return all_ok;
}
