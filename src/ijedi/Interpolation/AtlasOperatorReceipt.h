/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ijedi {

// Canonical, endian-independent encoding shared by geometry metrics and
// operator receipts. No numerical compiler or executor lives in this helper.
class AtlasOperatorReceipt {
 public:
  AtlasOperatorReceipt() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("cannot initialize Atlas receipt SHA-256");
    }
  }
  void integer(std::uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; ++i) {
      bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    }
    add(bytes, sizeof(bytes));
  }
  void real(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    integer(bits);
  }
  void string(const std::string &value) {
    integer(value.size());
    add(value.data(), value.size());
  }
  std::string finish() {
    flush();
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length;
    if (EVP_DigestFinal_ex(context_.get(), bytes, &length) != 1 || length != 32) {
      throw std::runtime_error("cannot finalize Atlas receipt SHA-256");
    }
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (size_t i = 0; i < length; ++i) {
      text << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return text.str();
  }

 private:
  void add(const void *data, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    while (size) {
      const size_t count = std::min(size, sizeof(buffer_) - buffered_);
      std::memcpy(buffer_ + buffered_, bytes, count);
      buffered_ += count;
      bytes += count;
      size -= count;
      if (buffered_ == sizeof(buffer_)) {
        flush();
      }
    }
  }
  void flush() {
    if (buffered_ && EVP_DigestUpdate(context_.get(), buffer_, buffered_) != 1) {
      throw std::runtime_error("Atlas receipt hash failed");
    }
    buffered_ = 0;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
  unsigned char buffer_[4096];
  size_t buffered_ = 0;
};

}  // namespace ijedi
