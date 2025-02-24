/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/io/async/SSLContext.h>

namespace proxygen {

template <typename T>
class Versioned : public T {
 public:
  uint64_t getVersion() const {
    return version_;
  }

 private:
  friend class ClientContextConfig;
  uint64_t version_ = 0;
};

class ThreadLocalSSLContext {
 public:
  using VersionedSSLContext = Versioned<folly::SSLContext>;

  /**
   * Get a SSLContext that is cached lazily.
   */
  virtual std::shared_ptr<VersionedSSLContext> getSSLContext() const = 0;

  virtual ~ThreadLocalSSLContext() {
  }
};

} // namespace proxygen
