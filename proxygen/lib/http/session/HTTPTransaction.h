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

#include <climits>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestructionBase.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/lang/Assume.h>
#include <iosfwd>
#include <proxygen/lib/http/HTTPConstants.h>
#include <proxygen/lib/http/HTTPHeaderSize.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>
#include <proxygen/lib/http/Window.h>
#include <proxygen/lib/http/codec/HTTPCodec.h>
#include <proxygen/lib/http/session/ByteEvents.h>
#include <proxygen/lib/http/session/HTTP2PriorityQueue.h>
#include <proxygen/lib/http/session/HTTPEvent.h>
#include <proxygen/lib/http/session/HTTPTransactionEgressSM.h>
#include <proxygen/lib/http/session/HTTPTransactionIngressSM.h>
#include <proxygen/lib/utils/Time.h>
#include <proxygen/lib/utils/WheelTimerInstance.h>
#include <set>
#include <wangle/acceptor/TransportInfo.h>

namespace proxygen {

/**
 * An HTTPTransaction represents a single request/response pair
 * for some HTTP-like protocol.  It works with a Transport that
 * performs the network processing and wire-protocol formatting
 * and a Handler that implements some sort of application logic.
 *
 * The typical sequence of events for a simple application is:
 *
 *   * The application accepts a connection and creates a Transport.
 *   * The Transport reads from the connection, parses whatever
 *     protocol the client is speaking, and creates a Transaction
 *     to represent the first request.
 *   * Once the Transport has received the full request headers,
 *     it creates a Handler, plugs the handler into the Transaction,
 *     and calls the Transaction's onIngressHeadersComplete() method.
 *   * The Transaction calls the Handler's onHeadersComplete() method
 *     and the Handler begins processing the request.
 *   * If there is a request body, the Transport streams it through
 *     the Transaction to the Handler.
 *   * When the Handler is ready to produce a response, it streams
 *     the response through the Transaction to the Transport.
 *   * When the Transaction has seen the end of both the request
 *     and the response, it detaches itself from the Handler and
 *     Transport and deletes itself.
 *   * The Handler deletes itself at some point after the Transaction
 *     has detached from it.
 *   * The Transport may, depending on the protocol, process other
 *     requests after -- or even in parallel with -- that first
 *     request.  Each request gets its own Transaction and Handler.
 *
 * For some applications, like proxying, a Handler implementation
 * may obtain one or more upstream connections, each represented
 * by another Transport, and create outgoing requests on the upstream
 * connection(s), with each request represented as a new Transaction.
 *
 * With a multiplexing protocol like SPDY on both sides of a proxy,
 * the cardinality relationship can be:
 *
 *                 +-----------+     +-----------+     +-------+
 *   (Client-side) | Transport |1---*|Transaction|1---1|Handler|
 *                 +-----------+     +-----------+     +-------+
 *                                                         1
 *                                                         |
 *                                                         |
 *                                                         1
 *                                   +---------+     +-----------+
 *                (Server-side)      |Transport|1---*|Transaction|
 *                                   +---------+     +-----------+
 *
 * A key design goal of HTTPTransaction is to serve as a protocol-
 * independent abstraction that insulates Handlers from the semantics
 * different of HTTP-like protocols.
 */

/** Info about Transaction running on this session */
class TransactionInfo {
 public:
  TransactionInfo() {
  }

  TransactionInfo(std::chrono::milliseconds ttfb,
                  std::chrono::milliseconds ttlb,
                  uint64_t eHeader,
                  uint64_t inHeader,
                  uint64_t eBody,
                  uint64_t inBody,
                  bool completed)
      : timeToFirstByte(ttfb),
        timeToLastByte(ttlb),
        egressHeaderBytes(eHeader),
        ingressHeaderBytes(inHeader),
        egressBodyBytes(eBody),
        ingressBodyBytes(inBody),
        isCompleted(completed) {
  }

  /** Time to first byte */
  std::chrono::milliseconds timeToFirstByte{0};
  /** Time to last byte */
  std::chrono::milliseconds timeToLastByte{0};

  /** Number of bytes send in headers */
  uint64_t egressHeaderBytes{0};
  /** Number of bytes receive headers */
  uint64_t ingressHeaderBytes{0};
  /** Number of bytes send in body */
  uint64_t egressBodyBytes{0};
  /** Number of bytes receive in body */
  uint64_t ingressBodyBytes{0};

  /** Is the transaction was completed without error */
  bool isCompleted{false};
};

class HTTPSessionStats;
class HTTPTransaction;
class HTTPTransactionHandler {
 public:
  /**
   * Called once per transaction. This notifies the handler of which
   * transaction it should talk to and will receive callbacks from.
   */
  virtual void setTransaction(HTTPTransaction* txn) noexcept = 0;

  /**
   * Called once after a transaction successfully completes. It
   * will be called even if a read or write error happened earlier.
   * This is a terminal callback, which means that the HTTPTransaction
   * object that gives this call will be invalid after this function
   * completes.
   */
  virtual void detachTransaction() noexcept = 0;

  /**
   * Called at most once per transaction. This is usually the first
   * ingress callback. It is possible to get a read error before this
   * however. If you had previously called pauseIngress(), this callback
   * will be delayed until you call resumeIngress().
   */
  virtual void onHeadersComplete(std::unique_ptr<HTTPMessage> msg) noexcept = 0;

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress().
   */
  virtual void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept = 0;

  /**
   * Same as onBody() but with additional offset parameter.
   */
  virtual void onBodyWithOffset(uint64_t /* bodyOffset */,
                                std::unique_ptr<folly::IOBuf> chain) {
    onBody(std::move(chain));
  }

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). This signifies the beginning of a chunk of length
   * 'length'. You will receive onBody() after this. Also, the length will
   * be greater than zero.
   */
  virtual void onChunkHeader(size_t /* length */) noexcept {
  }

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). This signifies the end of a chunk.
   */
  virtual void onChunkComplete() noexcept {
  }

  /**
   * Can be called any number of times per transaction. If you had
   * previously called pauseIngress(), this callback will be delayed until
   * you call resumeIngress(). Trailers can be received once right before
   * the EOM of a chunked HTTP/1.1 reponse or multiple times per
   * transaction from SPDY and HTTP/2.0 HEADERS frames.
   */
  virtual void onTrailers(std::unique_ptr<HTTPHeaders> trailers) noexcept = 0;

  /**
   * Can be called once per transaction. If you had previously called
   * pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). After this callback is received, there will be no
   * more normal ingress callbacks received (onEgress*() and onError()
   * may still be invoked). The Handler should consider
   * ingress complete after receiving this message. This Transaction is
   * still valid, and work may still occur on it until detachTransaction
   * is called.
   */
  virtual void onEOM() noexcept = 0;

  /**
   * Can be called once per transaction. If you had previously called
   * pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). After this callback is invoked, further data
   * will be forwarded using the onBody() callback. Once the data transfer
   * is completed (EOF recevied in case of CONNECT), onEOM() callback will
   * be invoked.
   */
  virtual void onUpgrade(UpgradeProtocol protocol) noexcept = 0;

  /**
   * Can be called at any time before detachTransaction(). This callback
   * implies that an error has occurred. To determine if ingress or egress
   * is affected, check the direciont on the HTTPException. If the
   * direction is INGRESS, it MAY still be possible to send egress.
   */
  virtual void onError(const HTTPException& error) noexcept = 0;

  /**
   * If the remote side's receive buffer fills up, this callback will be
   * invoked so you can attempt to stop sending to the remote side.
   */
  virtual void onEgressPaused() noexcept = 0;

  /**
   * This callback lets you know that the remote side has resumed reading
   * and you can now continue to send data.
   */
  virtual void onEgressResumed() noexcept = 0;

  /**
   * Ask the handler to construct a handler for a pushed transaction associated
   * with its transaction.
   *
   * TODO: Reconsider default implementation here. If the handler
   * does not implement, better set max initiated to 0 in a settings frame?
   */
  virtual void onPushedTransaction(HTTPTransaction* /* txn */) noexcept {
  }

  /**
   * Ask the handler to construct a handler for a ExTransaction associated
   * with its transaction.
   */
  virtual void onExTransaction(HTTPTransaction* /* txn */) noexcept {
  }

  /**
   * Inform the handler that a GOAWAY has been received on the
   * transport. This callback will only be invoked if the transport is
   * SPDY or HTTP/2. It may be invoked multiple times, as HTTP/2 allows this.
   *
   * @param code The error code received in the GOAWAY frame
   */
  virtual void onGoaway(ErrorCode /* code */) noexcept {
  }

  /**
   * Inform the handler that unframed body is starting.
   */
  virtual void onUnframedBodyStarted(uint64_t /* offset */) noexcept {}

  /**
   * Inform the handler that data arrived into underlying transport's read
   * buffer.
   */
  virtual void onBodyPeek(uint64_t /* offset */,
                          const folly::IOBufQueue& /* chain */) noexcept {
  }

  /**
   * Inform the handler that the sender skipped data below certain offset.
   */
  virtual void onBodySkipped(uint64_t /* offset */) noexcept {
  }

  /**
   * Inform the handler that the receiver doesn't expect data under certain
   * offset anymore.
   */
  virtual void onBodyRejected(uint64_t /* offset */) noexcept {
  }

  virtual ~HTTPTransactionHandler() {
  }
};

class HTTPPushTransactionHandler : public HTTPTransactionHandler {
 public:
  ~HTTPPushTransactionHandler() override {
  }

  void onHeadersComplete(std::unique_ptr<HTTPMessage>) noexcept final {
    LOG(FATAL) << "push txn received headers";
  }

  void onBody(std::unique_ptr<folly::IOBuf>) noexcept final {
    LOG(FATAL) << "push txn received body";
  }

  void onBodyWithOffset(uint64_t,
                        std::unique_ptr<folly::IOBuf>) noexcept final {
    LOG(FATAL) << "push txn received body with offset";
  }

  void onChunkHeader(size_t /* length */) noexcept final {
    LOG(FATAL) << "push txn received chunk header";
  }

  void onChunkComplete() noexcept final {
    LOG(FATAL) << "push txn received chunk complete";
  }

  void onTrailers(std::unique_ptr<HTTPHeaders>) noexcept final {
    LOG(FATAL) << "push txn received trailers";
  }

  void onEOM() noexcept final {
    LOG(FATAL) << "push txn received EOM";
  }

  void onUpgrade(UpgradeProtocol) noexcept final {
    LOG(FATAL) << "push txn received upgrade";
  }

  void onPushedTransaction(HTTPTransaction*) noexcept final {
    LOG(FATAL) << "push txn received push txn";
  }
};

/**
 * Callback interface to be notified of events on the byte stream.
 */
class HTTPTransactionTransportCallback {
 public:
  virtual void firstHeaderByteFlushed() noexcept = 0;

  virtual void firstByteFlushed() noexcept = 0;

  virtual void lastByteFlushed() noexcept = 0;

  virtual void trackedByteFlushed() noexcept {
  }

  virtual void lastByteAcked(std::chrono::milliseconds latency) noexcept = 0;

  virtual void trackedByteEventTX(const ByteEvent& /* event */) noexcept {
  }

  virtual void trackedByteEventAck(const ByteEvent& /* event */) noexcept {
  }

  virtual void egressBufferEmpty() noexcept {
  }

  virtual void headerBytesGenerated(HTTPHeaderSize& size) noexcept = 0;

  virtual void headerBytesReceived(const HTTPHeaderSize& size) noexcept = 0;

  virtual void bodyBytesGenerated(size_t nbytes) noexcept = 0;

  virtual void bodyBytesReceived(size_t size) noexcept = 0;

  virtual void lastEgressHeaderByteAcked() noexcept {
  }

  virtual void bodyBytesDelivered(uint64_t /* bodyOffset */) noexcept {
  }

  virtual void bodyBytesDeliveryCancelled(uint64_t /* bodyOffset */) noexcept {
  }

  virtual ~HTTPTransactionTransportCallback() {
  }
};

class HTTPTransaction
    : public folly::HHWheelTimer::Callback
    , public folly::DelayedDestructionBase {
 public:
  using Handler = HTTPTransactionHandler;
  using PushHandler = HTTPPushTransactionHandler;

  using PeekCallback =
      const folly::Function<void(HTTPCodec::StreamID streamId,
                                 uint64_t /* bodyOffset */,
                                 const folly::IOBufQueue& /* chain */) const>&;

  class Transport {
   public:
    virtual ~Transport() {
    }

    virtual void pauseIngress(HTTPTransaction* txn) noexcept = 0;

    virtual void resumeIngress(HTTPTransaction* txn) noexcept = 0;

    virtual void transactionTimeout(HTTPTransaction* txn) noexcept = 0;

    virtual void sendHeaders(HTTPTransaction* txn,
                             const HTTPMessage& headers,
                             HTTPHeaderSize* size,
                             bool eom) noexcept = 0;

    virtual size_t sendBody(HTTPTransaction* txn,
                            std::unique_ptr<folly::IOBuf>,
                            bool eom,
                            bool trackLastByteFlushed) noexcept = 0;

    virtual size_t sendChunkHeader(HTTPTransaction* txn,
                                   size_t length) noexcept = 0;

    virtual size_t sendChunkTerminator(HTTPTransaction* txn) noexcept = 0;

    virtual size_t sendEOM(HTTPTransaction* txn,
                           const HTTPHeaders* trailers) noexcept = 0;

    virtual size_t sendAbort(HTTPTransaction* txn,
                             ErrorCode statusCode) noexcept = 0;

    virtual size_t sendPriority(HTTPTransaction* txn,
                                const http2::PriorityUpdate& pri) noexcept = 0;

    virtual size_t sendWindowUpdate(HTTPTransaction* txn,
                                    uint32_t bytes) noexcept = 0;

    virtual void notifyPendingEgress() noexcept = 0;

    virtual void detach(HTTPTransaction* txn) noexcept = 0;

    virtual void notifyIngressBodyProcessed(uint32_t bytes) noexcept = 0;

    virtual void notifyEgressBodyBuffered(int64_t bytes) noexcept = 0;

    virtual const folly::SocketAddress& getLocalAddress() const noexcept = 0;

    virtual const folly::SocketAddress& getPeerAddress() const noexcept = 0;

    virtual void describe(std::ostream&) const = 0;

    virtual const wangle::TransportInfo& getSetupTransportInfo() const
        noexcept = 0;

    virtual bool getCurrentTransportInfo(wangle::TransportInfo* tinfo) = 0;

    virtual const HTTPCodec& getCodec() const noexcept = 0;

    /*
     * Drain the underlying session. This will affect other transactions
     * running on the same session and is discouraged unless you are confident
     * that the session is broken.
     */
    virtual void drain() = 0;

    virtual bool isDraining() const = 0;

    virtual HTTPTransaction* newPushedTransaction(
        HTTPCodec::StreamID assocStreamId,
        HTTPTransaction::PushHandler* handler) noexcept = 0;

    virtual HTTPTransaction* newExTransaction(HTTPTransaction::Handler* handler,
                                              HTTPCodec::StreamID controlStream,
                                              bool unidirectional) noexcept = 0;

    virtual std::string getSecurityProtocol() const = 0;

    virtual void addWaitingForReplaySafety(
        folly::AsyncTransport::ReplaySafetyCallback* callback) noexcept = 0;

    virtual void removeWaitingForReplaySafety(
        folly::AsyncTransport::ReplaySafetyCallback* callback) noexcept = 0;

    virtual bool needToBlockForReplaySafety() const = 0;

    virtual const folly::AsyncTransportWrapper* getUnderlyingTransport() const
        noexcept = 0;

    /**
     * Returns true if the underlying transport has completed full handshake.
     */
    virtual bool isReplaySafe() const = 0;

    virtual void setHTTP2PrioritiesEnabled(bool enabled) = 0;
    virtual bool getHTTP2PrioritiesEnabled() const = 0;

    virtual folly::Optional<const HTTPMessage::HTTPPriority> getHTTPPriority(
        uint8_t level) = 0;

    virtual folly::Expected<folly::Unit, ErrorCode> peek(
        PeekCallback /* peekCallback */) {
      LOG(FATAL) << __func__ << " not supported";
      folly::assume_unreachable();
    }

    virtual folly::Expected<folly::Unit, ErrorCode> consume(
        size_t /* amount */) {
      LOG(FATAL) << __func__ << " not supported";
      folly::assume_unreachable();
    }

    /**
     * Notify peer that the data below the offset isn't going to be sent.
     */
    virtual folly::Expected<folly::Optional<uint64_t>, ErrorCode> skipBodyTo(
        HTTPTransaction* /* txn */, uint64_t /* nextBodyOffset */) {
      LOG(FATAL) << __func__ << " not supported";
      folly::assume_unreachable();
    }

    /**
     * Notify peer that the data below the offset is not needed anymore.
     */
    virtual folly::Expected<folly::Optional<uint64_t>, ErrorCode> rejectBodyTo(
        HTTPTransaction* /* txn */, uint64_t /* nextBodyOffset */) {
      LOG(FATAL) << __func__ << " not supported";
      folly::assume_unreachable();
    }

    /**
     * Ask transport to track and ack body delivery.
     */
    virtual folly::Expected<folly::Unit, ErrorCode> trackEgressBodyDelivery(
        uint64_t /* bodyOffset */) {
      LOG(FATAL) << __func__ << " not supported";
      folly::assume_unreachable();
    }
  };

  using TransportCallback = HTTPTransactionTransportCallback;

  /**
   * readBufLimit and sendWindow are only used if useFlowControl is
   * true. Furthermore, if flow control is enabled, no guarantees can be
   * made on the borders of the L7 chunking/data frames of the outbound
   * messages.
   *
   * priority is only used by SPDY. The -1 default makes sure that all
   * plain HTTP transactions land up in the same queue as the control data.
   */
  HTTPTransaction(
      TransportDirection direction,
      HTTPCodec::StreamID id,
      uint32_t seqNo,
      Transport& transport,
      HTTP2PriorityQueueBase& egressQueue,
      folly::HHWheelTimer* timer = nullptr,
      const folly::Optional<std::chrono::milliseconds>& defaultTimeout =
          folly::Optional<std::chrono::milliseconds>(),
      HTTPSessionStats* stats = nullptr,
      bool useFlowControl = false,
      uint32_t receiveInitialWindowSize = 0,
      uint32_t sendInitialWindowSize = 0,
      http2::PriorityUpdate = http2::DefaultPriority,
      folly::Optional<HTTPCodec::StreamID> assocStreamId = HTTPCodec::NoStream,
      folly::Optional<HTTPCodec::ExAttributes> exAttributes =
          HTTPCodec::NoExAttributes);

  ~HTTPTransaction() override;

  void reset(bool useFlowControl,
             uint32_t receiveInitialWindowSize,
             uint32_t receiveStreamWindowSize,
             uint32_t sendInitialWindowSize);

  HTTPCodec::StreamID getID() const {
    return id_;
  }

  uint32_t getSequenceNumber() const {
    return seqNo_;
  }

  const Transport& getTransport() const {
    return transport_;
  }

  Transport& getTransport() {
    return transport_;
  }

  virtual void setHandler(Handler* handler) {
    handler_ = handler;
    if (handler_) {
      handler_->setTransaction(this);
    }
  }

  const Handler* getHandler() const {
    return handler_;
  }

  http2::PriorityUpdate getPriority() const {
    return priority_;
  }

  std::tuple<uint64_t, uint64_t, double> getPrioritySummary() const {
    return std::make_tuple(insertDepth_,
                           currentDepth_,
                           egressCalls_ > 0 ? cumulativeRatio_ / egressCalls_
                                            : 0);
  }

  bool getPriorityFallback() const {
    return priorityFallback_;
  }

  HTTPTransactionEgressSM::State getEgressState() const {
    return egressState_;
  }

  HTTPTransactionIngressSM::State getIngressState() const {
    return ingressState_;
  }

  bool isUpstream() const {
    return direction_ == TransportDirection::UPSTREAM;
  }

  bool isDownstream() const {
    return direction_ == TransportDirection::DOWNSTREAM;
  }

  void getLocalAddress(folly::SocketAddress& addr) const {
    addr = transport_.getLocalAddress();
  }

  void getPeerAddress(folly::SocketAddress& addr) const {
    addr = transport_.getPeerAddress();
  }

  const folly::SocketAddress& getLocalAddress() const noexcept {
    return transport_.getLocalAddress();
  }

  const folly::SocketAddress& getPeerAddress() const noexcept {
    return transport_.getPeerAddress();
  }

  const wangle::TransportInfo& getSetupTransportInfo() const noexcept {
    return transport_.getSetupTransportInfo();
  }

  void getCurrentTransportInfo(wangle::TransportInfo* tinfo) const {
    transport_.getCurrentTransportInfo(tinfo);
  }

  HTTPSessionStats* getSessionStats() const {
    return stats_;
  }

  /**
   * Check whether more response is expected. One or more 1xx status
   * responses can be received prior to the regular response.
   * Note: 101 is handled by the codec using a separate onUpgrade callback
   */
  virtual bool extraResponseExpected() const {
    return (lastResponseStatus_ >= 100 && lastResponseStatus_ < 200) &&
           lastResponseStatus_ != 101;
  }

  /**
   * Change the size of the receive window and propagate the change to the
   * remote end using a window update.
   *
   * TODO: when HTTPSession sends a SETTINGS frame indicating a
   * different initial window, it should call this function on all its
   * transactions.
   */
  virtual void setReceiveWindow(uint32_t capacity);

  /**
   * Get the receive window of the transaction
   */
  virtual const Window& getReceiveWindow() const {
    return recvWindow_;
  }

  uint32_t getMaxDeferredSize() {
    return maxDeferredIngress_;
  }

  /**
   * Invoked by the session when the ingress headers are complete
   */
  void onIngressHeadersComplete(std::unique_ptr<HTTPMessage> msg);

  /**
   * Invoked by the session when some or all of the ingress entity-body has
   * been parsed.
   */
  void onIngressBody(std::unique_ptr<folly::IOBuf> chain, uint16_t padding);

  /**
   * Invoked by the session when a chunk header has been parsed.
   */
  void onIngressChunkHeader(size_t length);

  /**
   * Invoked by the session when the CRLF terminating a chunk has been parsed.
   */
  void onIngressChunkComplete();

  /**
   * Invoked by the session when the ingress trailers have been parsed.
   */
  void onIngressTrailers(std::unique_ptr<HTTPHeaders> trailers);

  /**
   * Invoked by the session when the session and transaction need to be
   * upgraded to a different protocol
   */
  void onIngressUpgrade(UpgradeProtocol protocol);

  /**
   * Invoked by the session when the ingress message is complete.
   */
  void onIngressEOM();

  /**
   * Invoked by the session when there is an error (e.g., invalid syntax,
   * TCP RST) in either the ingress or egress stream. Note that this
   * message is processed immediately even if this transaction normally
   * would queue ingress.
   *
   * @param error Details for the error. This exception also has
   * information about whether the error applies to the ingress, egress,
   * or both directions of the transaction
   */
  void onError(const HTTPException& error);

  /**
   * Invoked by the session when a GOAWAY frame is received.
   * TODO: we may consider exposing the additional debug data here in the
   * future.
   *
   * @param code The error code received in the GOAWAY frame
   */
  void onGoaway(ErrorCode code);

  /**
   * Invoked by the session when there is a timeout on the ingress stream.
   * Note that each transaction has its own timer but the session
   * is the effective target of the timer.
   */
  void onIngressTimeout();

  /**
   * Invoked by the session when the remote endpoint of this transaction
   * signals that it has consumed 'amount' bytes. This is only for
   * versions of HTTP that support per transaction flow control.
   */
  void onIngressWindowUpdate(uint32_t amount);

  /**
   * Invoked by the session when the remote endpoint signals that we
   * should change our send window. This is only for
   * versions of HTTP that support per transaction flow control.
   */
  void onIngressSetSendWindow(uint32_t newWindowSize);

  /**
   * Ivoked by the session when it gets the start of the unframed body.
   */
  void onIngressUnframedBodyStarted(uint64_t offset) {
    partiallyReliable_ = true;
    if (handler_) {
      handler_->onUnframedBodyStarted(offset);
    }
  }

  /**
   * Notify this transaction that it is ok to egress.  Returns true if there
   * is additional pending egress
   */
  bool onWriteReady(uint32_t maxEgress, double ratio);

  /**
   * Invoked by the session when there is a timeout on the egress stream.
   */
  void onEgressTimeout();

  /**
   * Invoked by the session when the first header byte is flushed.
   */
  void onEgressHeaderFirstByte();

  /**
   * Invoked by the session when the first byte is flushed.
   */
  void onEgressBodyFirstByte();

  /**
   * Invoked by the session when the last byte is flushed.
   */
  void onEgressBodyLastByte();

  /**
   * Invoked by the session when the tracked byte is flushed.
   */
  void onEgressTrackedByte();

  /**
   * Invoked when the ACK_LATENCY event is delivered
   *
   * @param latency the time between the moment when the last byte was sent
   *        and the moment when we received the ACK from the client
   */
  void onEgressLastByteAck(std::chrono::milliseconds latency);

  /**
   * Invoked by the session when last egress headers have been acked by the
   * peer.
   */
  void onLastEgressHeaderByteAcked();

  /**
   * Invoked by the session when egress body has been acked by the
   * peer. Called for each sendBody() call if body bytes tracking is enabled.
   */
  void onEgressBodyBytesAcked(uint64_t bodyOffset);

  /**
   * Invoked by the session when egress body delivery has been cancelled by the
   * peer.
   */
  void onEgressBodyDeliveryCanceled(uint64_t bodyOffset);

  /**
   * Invoked by the session when a tracked ByteEvent is transmitted by NIC.
   */
  void onEgressTrackedByteEventTX(const ByteEvent& event);

  /**
   * Invoked by the session when a tracked ByteEvent is ACKed by remote peer.
   *
   * LAST_BYTE events are processed by legacy functions.
   */
  void onEgressTrackedByteEventAck(const ByteEvent& event);

  /**
   * Invoked by the session when data to peek into is available on trasport
   * layer.
   */
  void onIngressBodyPeek(uint64_t bodyOffset, const folly::IOBufQueue& chain);

  /**
   * Invoked by the session when transaction receives a skip from the peer.
   *
   * @param nextBodyOffset      Next body offset set by the sender.
   */
  void onIngressBodySkipped(uint64_t nextBodyOffset);

  /**
   * Invoked by the session when transaction receives a reject from the peer.
   *
   * @param nextBodyOffset  Next body offset set by the receiver.
   */
  void onIngressBodyRejected(uint64_t nextBodyOffset);

  /**
   * Invoked by the handlers that are interested in tracking
   * performance stats.
   */
  void setTransportCallback(TransportCallback* cb) {
    transportCallback_ = cb;
  }

  /**
   * @return true if ingress has started on this transaction.
   */
  bool isIngressStarted() const {
    return ingressState_ != HTTPTransactionIngressSM::State::Start;
  }

  /**
   * @return true iff the ingress EOM has been queued in HTTPTransaction
   * but the handler has not yet been notified of this event.
   */
  bool isIngressEOMQueued() const {
    return ingressState_ == HTTPTransactionIngressSM::State::EOMQueued;
  }

  /**
   * @return true iff the handler has been notified of the ingress EOM.
   */
  bool isIngressComplete() const {
    return ingressState_ == HTTPTransactionIngressSM::State::ReceivingDone;
  }

  /**
   * @return true iff onIngressEOM() has been called.
   */
  bool isIngressEOMSeen() const {
    return isIngressEOMQueued() || isIngressComplete();
  }

  /**
   * @return true if egress has started on this transaction.
   */
  bool isEgressStarted() const {
    return egressState_ != HTTPTransactionEgressSM::State::Start;
  }

  /**
   * @return true iff sendEOM() has been called, but the eom has not been
   * flushed to the socket yet.
   */
  bool isEgressEOMQueued() const {
    return egressState_ == HTTPTransactionEgressSM::State::EOMQueued;
  }

  /**
   * @return true iff the egress EOM has been flushed to the socket.
   */
  bool isEgressComplete() const {
    return egressState_ == HTTPTransactionEgressSM::State::SendingDone;
  }

  /**
   * @return true iff the remote side initiated this transaction.
   */
  bool isRemoteInitiated() const {
    return (direction_ == TransportDirection::DOWNSTREAM && id_ % 2 == 1) ||
           (direction_ == TransportDirection::UPSTREAM && id_ % 2 == 0);
  }

  /**
   * @return true iff sendEOM() has been called.
   */
  bool isEgressEOMSeen() const {
    return isEgressEOMQueued() || isEgressComplete();
  }

  /**
   * @return true if we can send headers on this transaction
   *
   * Here's the logic:
   *  1) state machine says sendHeaders is OK AND
   *   2a) this is an upstream (allows for mid-stream headers) OR
   *   2b) this downstream has not sent a response
   *   2c) this downstream has only sent 1xx responses
   */
  virtual bool canSendHeaders() const {
    return HTTPTransactionEgressSM::canTransit(
               egressState_, HTTPTransactionEgressSM::Event::sendHeaders) &&
           (isUpstream() || lastResponseStatus_ == 0 ||
            extraResponseExpected());
  }

  /**
   * Send the egress message headers to the Transport. This method does
   * not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Note: This method should be called once per message unless the first
   * headers sent indicate a 1xx status.
   *
   * sendHeaders will not set EOM flag in header frame, whereas
   * sendHeadersWithEOM will. sendHeadersWithOptionalEOM backs both of them.
   *
   * @param headers  Message headers
   */
  virtual void sendHeaders(const HTTPMessage& headers);
  virtual void sendHeadersWithEOM(const HTTPMessage& headers);
  virtual void sendHeadersWithOptionalEOM(const HTTPMessage& headers, bool eom);

  /**
   * Send part or all of the egress message body to the Transport. If flow
   * control is enabled, the chunk boundaries may not be respected.
   * This method does not actually write the message out on the wire
   * immediately. All writes happen at the end of the event loop at the
   * earliest.
   * Note: This method may be called zero or more times per message.
   *
   * @param body Message body data; the Transport will take care of
   *             applying any necessary protocol framing, such as
   *             chunk headers.
   */
  virtual void sendBody(std::unique_ptr<folly::IOBuf> body);

  /**
   * Write any protocol framing required for the subsequent call(s)
   * to sendBody(). This method does not actually write the message out on
   * the wire immediately. All writes happen at the end of the event loop
   * at the earliest.
   * @param length  Length in bytes of the body data to follow.
   */
  virtual void sendChunkHeader(size_t length) {
    CHECK(HTTPTransactionEgressSM::transit(
        egressState_, HTTPTransactionEgressSM::Event::sendChunkHeader));
    CHECK(!partiallyReliable_)
        << __func__ << ": chunking not supported in partially reliable mode.";
    // TODO: move this logic down to session/codec
    if (!transport_.getCodec().supportsParallelRequests()) {
      chunkHeaders_.emplace_back(Chunk(length));
    }
  }

  /**
   * Write any protocol syntax needed to terminate the data. This method
   * does not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Frame begun by the last call to sendChunkHeader().
   */
  virtual void sendChunkTerminator() {
    CHECK(HTTPTransactionEgressSM::transit(
        egressState_, HTTPTransactionEgressSM::Event::sendChunkTerminator));
    CHECK(!partiallyReliable_)
        << __func__ << ": chunking not supported in partially reliable mode.";
  }

  /**
   * Send message trailers to the Transport. This method does
   * not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Note: This method may be called at most once per message.
   *
   * @param trailers  Message trailers.
   */
  virtual void sendTrailers(const HTTPHeaders& trailers) {
    CHECK(HTTPTransactionEgressSM::transit(
        egressState_, HTTPTransactionEgressSM::Event::sendTrailers));
    CHECK(!partiallyReliable_)
        << __func__
        << ": trailers are not supported in partially reliable mode.";
    trailers_.reset(new HTTPHeaders(trailers));
  }

  /**
   * Finalize the egress message; depending on the protocol used
   * by the Transport, this may involve sending an explicit "end
   * of message" indicator. This method does not actually write the
   * message out on the wire immediately. All writes happen at the end
   * of the event loop at the earliest.
   *
   * If the ingress message also is complete, the transaction may
   * detach itself from the Handler and Transport and delete itself
   * as part of this method.
   *
   * Note: Either this method or sendAbort() should be called once
   *       per message.
   */
  virtual void sendEOM();

  /**
   * Terminate the transaction. Depending on the underlying protocol, this
   * may cause the connection to close or write egress bytes. This method
   * does not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   *
   * This function may also cause additional callbacks such as
   * detachTransaction() to the handler either immediately or after it returns.
   */
  virtual void sendAbort();

  /**
   * Pause ingress processing.  Upon pause, the HTTPTransaction
   * will call its Transport's pauseIngress() method.  The Transport
   * should make a best effort to stop invoking the HTTPTransaction's
   * onIngress* callbacks.  If the Transport does invoke any of those
   * methods while the transaction is paused, however, the transaction
   * will queue the ingress events and data and delay delivery to the
   * Handler until the transaction is unpaused.
   */
  virtual void pauseIngress();

  /**
   * Resume ingress processing. Only useful after a call to pauseIngress().
   */
  virtual void resumeIngress();

  /**
   * @return true iff ingress processing is paused for the handler
   */
  bool isIngressPaused() const {
    return ingressPaused_;
  }

  /**
   * Pause egress generation. HTTPTransaction may call its Handler's
   * onEgressPaused() method if there is a state change as a result of
   * this call.
   *
   * On receiving onEgressPaused(), the Handler should make a best effort
   * to stop invoking the HTTPTransaction's egress generating methods.  If
   * the Handler does invoke any of those methods while the transaction is
   * paused, however, the transaction will forward them anyway, unless it
   * is a body event. If flow control is enabled, body events will be
   * buffered for later transmission when egress is unpaused.
   */
  void pauseEgress();

  /**
   * Resume egress generation. The Handler's onEgressResumed() will not be
   * invoked if the HTTP/2 send window is full or there is too much
   * buffered egress data on this transaction already. In that case,
   * once the send window is not full or the buffer usage decreases, the
   * handler will finally get onEgressResumed().
   */
  void resumeEgress();

  /**
   * Specify a rate limit for egressing bytes.
   * The transaction will buffer extra bytes if doing so would cause it to go
   * over the specified rate limit.  Setting to a value of 0 will cause no
   * rate-limiting to occur.
   */
  void setEgressRateLimit(uint64_t bitsPerSecond);

  /**
   * @return true iff egress processing is paused for the handler
   */
  bool isEgressPaused() const {
    return handlerEgressPaused_;
  }

  /**
   * @return true iff egress processing is paused due to flow control
   * to the handler
   */
  bool isFlowControlPaused() const {
    return flowControlPaused_;
  }

  /**
   * @return true iff this transaction can be used to push resources to
   * the remote side.
   */
  bool supportsPushTransactions() const {
    return direction_ == TransportDirection::DOWNSTREAM &&
           transport_.getCodec().supportsPushTransactions();
  }

  /**
   * Create a new pushed transaction associated with this transaction,
   * and assign the given handler and priority.
   *
   * @return the new transaction for the push, or nullptr if a new push
   * transaction is impossible right now.
   */
  virtual HTTPTransaction* newPushedTransaction(
      HTTPPushTransactionHandler* handler) {
    // Pushed transactions do support partially reliable mode, however push
    // promises should be only generated on a fully reliable transaction.
    CHECK(!partiallyReliable_)
        << __func__
        << ": push promises not supported in partially reliable mode.";
    if (isEgressEOMSeen()) {
      return nullptr;
    }
    auto txn = transport_.newPushedTransaction(id_, handler);
    if (txn) {
      pushedTransactions_.insert(txn->getID());
    }
    return txn;
  }

  /**
   * Create a new extended transaction associated with this transaction,
   * and assign the given handler and priority.
   *
   * @return the new transaction for pubsub, or nullptr if a new push
   * transaction is impossible right now.
   */
  virtual HTTPTransaction* newExTransaction(HTTPTransactionHandler* handler,
                                            bool unidirectional = false) {
    auto txn = transport_.newExTransaction(handler, id_, unidirectional);
    if (txn) {
      exTransactions_.insert(txn->getID());
    }
    return txn;
  }

  /**
   * Invoked by the session (upstream only) when a new pushed transaction
   * arrives.  The txn's handler will be notified and is responsible for
   * installing a handler.  If no handler is installed in the callback,
   * the pushed transaction will be aborted.
   */
  bool onPushedTransaction(HTTPTransaction* txn);

  /**
   * Invoked by the session when a new ExTransaction arrives.  The txn's handler
   * will be notified and is responsible for installing a handler.  If no
   * handler is installed in the callback, the transaction will be aborted.
   */
  bool onExTransaction(HTTPTransaction* txn);

  /**
   * True if this transaction is a server push transaction
   */
  bool isPushed() const {
    return assocStreamId_.has_value();
  }

  bool isExTransaction() const {
    return exAttributes_.has_value();
  }

  bool isUnidirectional() const {
    return isExTransaction() && exAttributes_->unidirectional;
  }

  /**
   * @return true iff we should notify the error occured on EX_TXN
   * This logic only applies to EX_TXN with QoS 0
   */
  bool shouldNotifyExTxnError(HTTPException::Direction errorDirection) const {
    if (isUnidirectional()) {
      if (isRemoteInitiated()) {
        // We care about EGRESS errors in this case,
        // because we marked EGRESS state to be completed
        // If EGRESS error is happening, we need to know
        // Same for INGRESS direction, when EX_TXN is not remoteInitiated()
        return errorDirection == HTTPException::Direction::EGRESS;
      } else {
        return errorDirection == HTTPException::Direction::INGRESS;
      }
    }
    return false;
  }

  /**
   * Sets a transaction timeout value. If such a timeout was set, this
   * timeout will be used instead of the default timeout interval configured
   * in transactionIdleTimeouts_.
   */
  void setIdleTimeout(std::chrono::milliseconds transactionTimeout);

  /**
   * Does this transaction have an idle timeout set?
   */
  bool hasIdleTimeout() const {
    return transactionTimeout_.hasValue();
  }

  /**
   * Returns the transaction timeout if exists. An OptionalEmptyException is
   * raised if the timeout isn't set.
   */
  std::chrono::milliseconds getIdleTimeout() const {
    return transactionTimeout_.value();
  }

  /**
   * Returns the associated transaction ID for pushed transactions, 0 otherwise
   */
  folly::Optional<HTTPCodec::StreamID> getAssocTxnId() const {
    return assocStreamId_;
  }

  /**
   * Returns the control channel transaction ID for this transaction,
   * folly::none otherwise
   */
  folly::Optional<HTTPCodec::StreamID> getControlStream() const {
    return exAttributes_ ? exAttributes_->controlStream : HTTPCodec::NoStream;
  }

  /*
   * Returns attributes of EX stream (folly::none if not an EX transaction)
   */
  folly::Optional<HTTPCodec::ExAttributes> getExAttributes() const {
    return exAttributes_;
  }

  /**
   * Get a set of server-pushed transactions associated with this transaction.
   */
  const std::set<HTTPCodec::StreamID>& getPushedTransactions() const {
    return pushedTransactions_;
  }

  /**
   * Get a set of exTransactions associated with this transaction.
   */
  std::set<HTTPCodec::StreamID> getExTransactions() const {
    return exTransactions_;
  }

  /**
   * Remove the pushed txn ID from the set of pushed txns
   * associated with this txn.
   */
  void removePushedTransaction(HTTPCodec::StreamID pushStreamId) {
    pushedTransactions_.erase(pushStreamId);
  }

  /**
   * Remove the exTxn ID from the control stream txn.
   */
  void removeExTransaction(HTTPCodec::StreamID exStreamId) {
    exTransactions_.erase(exStreamId);
  }

  /**
   * Schedule or refresh the timeout for this transaction
   */
  void refreshTimeout() {
    if (timer_ && hasIdleTimeout()) {
      timer_->scheduleTimeout(this, transactionTimeout_.value());
    }
  }

  /**
   * Tests if the first byte has already been sent, and if it
   * hasn't yet then it marks it as sent.
   */
  bool testAndSetFirstByteSent() {
    bool ret = firstByteSent_;
    firstByteSent_ = true;
    return ret;
  }

  bool testAndClearActive() {
    bool ret = inActiveSet_;
    inActiveSet_ = false;
    return ret;
  }

  /**
   * Tests if the very first byte of Header has already been set.
   * If it hasn't yet, it marks it as sent.
   */
  bool testAndSetFirstHeaderByteSent() {
    bool ret = firstHeaderByteSent_;
    firstHeaderByteSent_ = true;
    return ret;
  }

  /**
   * HTTPTransaction will not detach until it has 0 pending byte events.  If
   * you call incrementPendingByteEvents, you must make a corresponding call
   * to decrementPendingByteEvents or the transaction will never be destroyed.
   */
  void incrementPendingByteEvents() {
    CHECK_LT(pendingByteEvents_, std::numeric_limits<uint8_t>::max());
    pendingByteEvents_++;
  }

  void decrementPendingByteEvents() {
    DestructorGuard dg(this);
    CHECK_GT(pendingByteEvents_, 0);
    pendingByteEvents_--;
  }

  /**
   * Timeout callback for this transaction.  The timer is active
   * until the ingress message is complete or terminated by error.
   */
  void timeoutExpired() noexcept override {
    transport_.transactionTimeout(this);
  }

  /**
   * Write a description of the transaction to a stream
   */
  void describe(std::ostream& os) const;

  /**
   * Change the priority of this transaction, may generate a PRIORITY frame
   */
  void updateAndSendPriority(int8_t newPriority);
  void updateAndSendPriority(const http2::PriorityUpdate& pri);

  /**
   * Notify of priority change, will not generate a PRIORITY frame
   */
  void onPriorityUpdate(const http2::PriorityUpdate& priority);

  /**
   * Add a callback waiting for this transaction to have a transport with
   * replay protection.
   */
  virtual void addWaitingForReplaySafety(
      folly::AsyncTransport::ReplaySafetyCallback* callback) {
    transport_.addWaitingForReplaySafety(callback);
  }

  /**
   * Remove a callback waiting for replay protection (if it was canceled).
   */
  virtual void removeWaitingForReplaySafety(
      folly::AsyncTransport::ReplaySafetyCallback* callback) {
    transport_.removeWaitingForReplaySafety(callback);
  }

  virtual bool needToBlockForReplaySafety() const {
    return transport_.needToBlockForReplaySafety();
  }

  int32_t getRecvToAck() const;

  bool isPrioritySampled() const {
    return prioritySample_ != nullptr;
  }

  void setPrioritySampled(bool sampled);
  void updateContentionsCount(uint64_t contentions);
  void updateRelativeWeight(double ratio);
  void updateSessionBytesSheduled(uint64_t bytes);
  void updateTransactionBytesSent(uint64_t bytes);
  void checkIfEgressRateLimitedByUpstream();

  struct PrioritySampleSummary {
    struct WeightedAverage {
      double byTransactionBytes_{0};
      double bySessionBytes_{0};
    };
    WeightedAverage contentions_;
    WeightedAverage depth_;
    double expected_weight_;
    double measured_weight_;
  };

  bool getPrioritySampleSummary(PrioritySampleSummary& summary) const;

  const CompressionInfo& getCompressionInfo() const;

  bool hasPendingBody() const {
    return deferredEgressBody_.chainLength() > 0;
  }

  size_t getOutstandingEgressBodyBytes() const {
    return deferredEgressBody_.chainLength();
  }

  void setLastByteFlushedTrackingEnabled(bool enabled) {
    enableLastByteFlushedTracking_ = enabled;
  }

  folly::Expected<folly::Unit, ErrorCode>
  setBodyLastByteDeliveryTrackingEnabled(bool enabled) {
    if (!partiallyReliable_) {
      return folly::makeUnexpected(ErrorCode::PROTOCOL_ERROR);
    }
    enableBodyLastByteDeliveryTracking_ = enabled;
    return folly::unit;
  }

  /**
   * Allows the caller to peek into underlying transport's read buffer.
   * This, together with consume(), forms a scatter/gather API.
   *
   * @param peekCallback  A callback that will be executed on each contiguous
   *                      byte range in transport's read buffer. Number of byte
   *                      ranges is determined by the number of gaps in the
   *                      read buffer.
   */
  folly::Expected<folly::Unit, ErrorCode> peek(PeekCallback peekCallback);

  /**
   * Allows the caller to consume bytes from the beginning of the read buffer in
   * the underlying transport's read buffer.
   * This is useful when e.g. we know that the transaction is
   * head-of-line-blocked and we are willing to get rid of existing bytes in the
   * buffer to allow the transaction to proceed. For example:
   *    - read buffer pointer is at 3
   *    - we have bytes range [5, 18] ready in the buffer
   * If we don't want to wait to receive bytes [3, 4], we can call consume(2)
   * and the read buffer pointer would be moved to 5 allowing read operations to
   * proceed.
   *
   * @param amount  Number of bytes to consume from transport's read buffer.
   *                Gaps will be consumed together with received bytes in the
   *                buffer. Bytes will be always consumed from current read
   *                buffer front pointer.
   */
  folly::Expected<folly::Unit, ErrorCode> consume(size_t amount);

  /**
   * Allows the sender to skip part of the egress body. Calls can be interleaved
   * with sendBody() calls.
   * Upon receipt by the peer, this signals that the body up to bodyOffset shall
   * not be expected. Note that some bytes before this new advertised offset
   * might still be received by the peer if they were already in transmission
   * between the peers.
   *
   * @param bodyOffset  New offset the sender is going to start sending the body
   *                    from going forward.
   */
  folly::Expected<folly::Optional<uint64_t>, ErrorCode> skipBodyTo(
      uint64_t nextBodyOffset);

  /**
   * Similar to skipBodyTo() above, rejectBodyTo() allows the receiver to signal
   * to the sender that body bytes below bodyOffset are not expected anymore and
   * the sender needs to start sending from the new offset.
   * Note that the receiver may still receive some bytes below this new
   * advertised offset if any were in flight between the two peers.
   *
   * @param bodyOffset  New offset the sender needs to start sending from.
   */
  folly::Expected<folly::Optional<uint64_t>, ErrorCode> rejectBodyTo(
      uint64_t nextBodyOffset);

 private:
  HTTPTransaction(const HTTPTransaction&) = delete;
  HTTPTransaction& operator=(const HTTPTransaction&) = delete;

  void onDelayedDestroy(bool delayed) override;

  /**
   * Invokes the handler's onEgressPaused/Resumed if the handler's pause
   * state needs updating
   */
  void updateHandlerPauseState();

  /**
   * Update the CompressionInfo (tableInfo_) struct
   */
  void updateEgressCompressionInfo(const CompressionInfo&);

  void updateIngressCompressionInfo(const CompressionInfo&);

  bool mustQueueIngress() const;

  /**
   * Check if deferredIngress_ points to some queue before pushing HTTPEvent
   * to it.
   */
  void checkCreateDeferredIngress();

  /**
   * Implementation of sending an abort for this transaction.
   */
  void sendAbort(ErrorCode statusCode);

  // Internal implementations of the ingress-related callbacks
  // that work whether the ingress events are immediate or deferred.
  void processIngressHeadersComplete(std::unique_ptr<HTTPMessage> msg);
  void processIngressBody(std::unique_ptr<folly::IOBuf> chain, size_t len);
  void processIngressChunkHeader(size_t length);
  void processIngressChunkComplete();
  void processIngressTrailers(std::unique_ptr<HTTPHeaders> trailers);
  void processIngressUpgrade(UpgradeProtocol protocol);
  void processIngressEOM();

  void sendBodyFlowControlled(std::unique_ptr<folly::IOBuf> body = nullptr);
  size_t sendBodyNow(std::unique_ptr<folly::IOBuf> body,
                     size_t bodyLen,
                     bool eom);
  size_t sendEOMNow();
  void onDeltaSendWindowSize(int32_t windowDelta);

  void notifyTransportPendingEgress();

  size_t sendDeferredBody(uint32_t maxEgress);

  bool maybeDelayForRateLimit();

  bool isEnqueued() const {
    return queueHandle_->isEnqueued();
  }

  void dequeue() {
    DCHECK(isEnqueued());
    egressQueue_.clearPendingEgress(queueHandle_);
  }

  bool hasPendingEOM() const {
    return deferredEgressBody_.chainLength() == 0 && isEgressEOMQueued();
  }

  bool isExpectingIngress() const;

  bool isExpectingWindowUpdate() const;

  void updateReadTimeout();

  /**
   * Causes isIngressComplete() to return true, removes any queued
   * ingress, and cancels the read timeout.
   */
  void markIngressComplete();

  /**
   * Causes isEgressComplete() to return true, removes any queued egress,
   * and cancels the write timeout.
   */
  void markEgressComplete();

  /**
   * Validates the ingress state transition. Returns false and sends an
   * abort with PROTOCOL_ERROR if the transition fails. Otherwise it
   * returns true.
   */
  bool validateIngressStateTransition(HTTPTransactionIngressSM::Event);

  /**
   * Flushes any pending window updates.  This can happen from setReceiveWindow
   * or sendHeaders depending on transaction state.
   */
  void flushWindowUpdate();

  bool updateContentLengthRemaining(size_t len);

  void rateLimitTimeoutExpired();

  void trimDeferredEgressBody(uint64_t bodyOffset);

  class RateLimitCallback : public folly::HHWheelTimer::Callback {
   public:
    explicit RateLimitCallback(HTTPTransaction& txn) : txn_(txn) {
    }

    void timeoutExpired() noexcept override {
      txn_.rateLimitTimeoutExpired();
    }
    void callbackCanceled() noexcept override {
      // no op
    }

   private:
    HTTPTransaction& txn_;
  };

  RateLimitCallback rateLimitCallback_{*this};

  /**
   * Queue to hold any events that we receive from the Transaction
   * while the ingress is supposed to be paused.
   */
  std::unique_ptr<std::queue<HTTPEvent>> deferredIngress_;

  uint32_t maxDeferredIngress_{0};

  /**
   * Queue to hold any body bytes to be sent out
   * while egress to the remote is supposed to be paused.
   */
  folly::IOBufQueue deferredEgressBody_{folly::IOBufQueue::cacheChainLength()};

  const TransportDirection direction_;
  HTTPCodec::StreamID id_;
  uint32_t seqNo_;
  Handler* handler_{nullptr};
  Transport& transport_;
  HTTPTransactionEgressSM::State egressState_{
      HTTPTransactionEgressSM::getNewInstance()};
  HTTPTransactionIngressSM::State ingressState_{
      HTTPTransactionIngressSM::getNewInstance()};

  HTTPSessionStats* stats_{nullptr};

  CompressionInfo tableInfo_;

  /**
   * The recv window and associated data. This keeps track of how many
   * bytes we are allowed to buffer.
   */
  Window recvWindow_;

  /**
   * The send window and associated data. This keeps track of how many
   * bytes we are allowed to send and have outstanding.
   */
  Window sendWindow_;

  TransportCallback* transportCallback_{nullptr};

  /**
   * Trailers to send, if any.
   */
  std::unique_ptr<HTTPHeaders> trailers_;

  struct Chunk {
    explicit Chunk(size_t inLength) : length(inLength), headerSent(false) {
    }
    size_t length;
    bool headerSent;
  };
  std::list<Chunk> chunkHeaders_;

  /**
   * Reference to our priority queue
   */
  HTTP2PriorityQueueBase& egressQueue_;

  /**
   * Handle to our position in the priority queue.
   */
  HTTP2PriorityQueueBase::Handle queueHandle_;

  /**
   * bytes we need to acknowledge to the remote end using a window update
   */
  int32_t recvToAck_{0};

  /**
   * ID of request transaction (for pushed txns only)
   */
  folly::Optional<HTTPCodec::StreamID> assocStreamId_;

  /**
   * Attributes of http2 Ex_HEADERS
   */
  folly::Optional<HTTPCodec::ExAttributes> exAttributes_;

  /**
   * Set of all push transactions IDs associated with this transaction.
   */
  std::set<HTTPCodec::StreamID> pushedTransactions_;

  /**
   * Set of all exTransaction IDs associated with this transaction.
   */
  std::set<HTTPCodec::StreamID> exTransactions_;

  /**
   * Priority of this transaction
   */
  http2::PriorityUpdate priority_;

  /**
   * Information about this transaction's priority.
   *
   * insertDepth_ is the depth of this node in the tree when the txn was created
   * currentDepth_ is the depth of this node in the tree after the last
   *               onPriorityUpdate. It may not reflect its real position in
   *               realtime, since after the last onPriorityUpdate, it may get
   *               reparented as parent transactions complete.
   * cumulativeRatio_ / egressCalls_ is the average relative weight of this
   *                                 txn during egress
   */
  uint64_t insertDepth_{0};
  uint64_t currentDepth_{0};
  double cumulativeRatio_{0};
  uint64_t egressCalls_{0};

  /**
   * If this transaction represents a request (ie, it is backed by an
   * HTTPUpstreamSession) , this field indicates the last response status
   * received from the server. If this transaction represents a response,
   * this field indicates the last status we've sent. For instances, this
   * could take on multiple 1xx values, and then take on 200.
   */
  uint16_t lastResponseStatus_{0};
  uint8_t pendingByteEvents_{0};
  folly::Optional<uint64_t> expectedIngressContentLength_;
  folly::Optional<uint64_t> expectedIngressContentLengthRemaining_;
  folly::Optional<uint64_t> expectedResponseLength_;
  folly::Optional<uint64_t> actualResponseLength_{0};
  // Keeps track of how many bytes the transaction passed to the transport so
  // far.
  uint64_t egressBodyBytesCommittedToTransport_{0};

  bool ingressPaused_ : 1;
  bool egressPaused_ : 1;
  bool flowControlPaused_ : 1;
  bool handlerEgressPaused_ : 1;
  bool egressRateLimited_ : 1;
  bool useFlowControl_ : 1;
  bool aborted_ : 1;
  bool deleting_ : 1;
  bool firstByteSent_ : 1;
  bool firstHeaderByteSent_ : 1;
  bool inResume_ : 1;
  bool inActiveSet_ : 1;
  bool ingressErrorSeen_ : 1;
  bool priorityFallback_ : 1;
  bool headRequest_ : 1;
  bool enableLastByteFlushedTracking_ : 1;
  bool enableBodyLastByteDeliveryTracking_ : 1;

  static uint64_t egressBufferLimit_;

  uint64_t egressLimitBytesPerMs_{0};
  proxygen::TimePoint startRateLimit_;
  uint64_t numLimitedBytesEgressed_{0};

  /**
   * Optional transaction timeout value.
   */
  folly::Optional<std::chrono::milliseconds> transactionTimeout_;

  folly::HHWheelTimer* timer_;

  class PrioritySample;
  std::unique_ptr<PrioritySample> prioritySample_;

  // Signals if the transaction is partially reliable.
  // Set on first sendHeaders() call on egress or with setPartiallyReliable() on
  // ingress.
  bool partiallyReliable_{false};

  // Prevents the application from calling skipBodyTo() before egress
  // headers have been delivered.
  bool egressHeadersDelivered_{false};

  // Keeps track for body offset processed so far.
  // Includes skipped bytes for partially reliable transactions.
  uint64_t ingressBodyOffset_{0};
};

/**
 * Write a description of an HTTPTransaction to an ostream
 */
std::ostream& operator<<(std::ostream& os, const HTTPTransaction& txn);

} // namespace proxygen
