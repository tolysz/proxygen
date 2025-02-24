/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "proxygen/lib/http/stats/ThreadLocalHTTPSessionStats.h"

namespace proxygen {

TLHTTPSessionStats::TLHTTPSessionStats(const std::string& prefix)
    : txnsOpen(prefix + "_transactions_open"),
      txnsOpened(
          prefix + "_txn_opened", facebook::fb303::SUM, facebook::fb303::RATE),
      txnsFromSessionReuse(prefix + "_txn_session_reuse",
                           facebook::fb303::SUM,
                           facebook::fb303::RATE),
      txnsTransactionStalled(prefix + "_txn_transaction_stall",
                             facebook::fb303::SUM,
                             facebook::fb303::RATE),
      txnsSessionStalled(prefix + "_txn_session_stall",
                         facebook::fb303::SUM,
                         facebook::fb303::RATE),
      presendIoSplit(prefix + "_presend_io_split",
                     facebook::fb303::SUM,
                     facebook::fb303::RATE),
      presendExceedLimit(prefix + "_presend_exceed_limit",
                         facebook::fb303::SUM,
                         facebook::fb303::RATE),
      ttlbaTracked(prefix + "_ttlba_tracked",
                   facebook::fb303::SUM,
                   facebook::fb303::RATE),
      ttlbaReceived(prefix + "_ttlba_received",
                    facebook::fb303::SUM,
                    facebook::fb303::RATE),
      ttlbaTimeout(prefix + "_ttlba_timeout",
                   facebook::fb303::SUM,
                   facebook::fb303::RATE),
      ttlbaNotFound(prefix + "_ttlba_not_found",
                    facebook::fb303::SUM,
                    facebook::fb303::RATE),
      ttlbaExceedLimit(prefix + "_ttlba_exceed_limit",
                       facebook::fb303::SUM,
                       facebook::fb303::RATE),
      ttbtxTracked(prefix + "_ttbtx_tracked",
                   facebook::fb303::SUM,
                   facebook::fb303::RATE),
      ttbtxReceived(prefix + "_ttbtx_received",
                    facebook::fb303::SUM,
                    facebook::fb303::RATE),
      ttbtxTimeout(prefix + "_ttbtx_timeout",
                   facebook::fb303::SUM,
                   facebook::fb303::RATE),
      ttbtxNotFound(prefix + "_ttbtx_not_found",
                    facebook::fb303::SUM,
                    facebook::fb303::RATE),
      ttbtxExceedLimit(prefix + "_ttbtx_exceed_limit",
                       facebook::fb303::SUM,
                       facebook::fb303::RATE),
      txnsPerSession(prefix + "_txn_per_session",
                     1,
                     0,
                     999,
                     facebook::fb303::AVG,
                     50,
                     95,
                     99),
      sessionIdleTime(prefix + "_session_idle_time",
                      1,
                      0,
                      150,
                      facebook::fb303::AVG,
                      50,
                      75,
                      95,
                      99) {
}

void TLHTTPSessionStats::recordTransactionOpened() noexcept {
  txnsOpen.incrementValue(1);
  txnsOpened.add(1);
}

void TLHTTPSessionStats::recordTransactionClosed() noexcept {
  txnsOpen.incrementValue(-1);
}

void TLHTTPSessionStats::recordSessionReused() noexcept {
  txnsFromSessionReuse.add(1);
}

void TLHTTPSessionStats::recordPresendIOSplit() noexcept {
  presendIoSplit.add(1);
}

void TLHTTPSessionStats::recordPresendExceedLimit() noexcept {
  presendExceedLimit.add(1);
}

void TLHTTPSessionStats::recordTTLBAExceedLimit() noexcept {
  ttlbaExceedLimit.add(1);
}

void TLHTTPSessionStats::recordTTLBANotFound() noexcept {
  ttlbaNotFound.add(1);
}

void TLHTTPSessionStats::recordTTLBAReceived() noexcept {
  ttlbaReceived.add(1);
}

void TLHTTPSessionStats::recordTTLBATimeout() noexcept {
  ttlbaTimeout.add(1);
}

void TLHTTPSessionStats::recordTTLBATracked() noexcept {
  ttlbaTracked.add(1);
}

void TLHTTPSessionStats::recordTTBTXExceedLimit() noexcept {
  ttbtxExceedLimit.add(1);
}

void TLHTTPSessionStats::recordTTBTXReceived() noexcept {
  ttbtxReceived.add(1);
}

void TLHTTPSessionStats::recordTTBTXTimeout() noexcept {
  ttbtxTimeout.add(1);
}

void TLHTTPSessionStats::recordTTBTXNotFound() noexcept {
  ttbtxNotFound.add(1);
}

void TLHTTPSessionStats::recordTTBTXTracked() noexcept {
  ttbtxTracked.add(1);
}

void TLHTTPSessionStats::recordTransactionsServed(uint64_t num) noexcept {
  txnsPerSession.add(num);
}

void TLHTTPSessionStats::recordSessionIdleTime(
    std::chrono::seconds idleTime) noexcept {
  sessionIdleTime.add(idleTime.count());
}

void TLHTTPSessionStats::recordTransactionStalled() noexcept {
  txnsTransactionStalled.add(1);
}

void TLHTTPSessionStats::recordSessionStalled() noexcept {
  txnsSessionStalled.add(1);
}

} // namespace proxygen
