/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/HTTPConnectorWithFizz.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>

using namespace fizz::client;

namespace proxygen {

void HTTPConnectorWithFizz::connectFizz(
    folly::EventBase* eventBase,
    const folly::SocketAddress& connectAddr,
    std::shared_ptr<const FizzClientContext> context,
    std::shared_ptr<const fizz::CertificateVerifier> verifier,
    std::chrono::milliseconds totalTimeout,
    std::chrono::milliseconds tcpConnectTimeout,
    const folly::AsyncSocket::OptionMap& socketOptions,
    const folly::SocketAddress& bindAddr,
    folly::Optional<std::string> sni,
    folly::Optional<std::string> pskIdentity) {
  DCHECK(!isBusy());
  transportInfo_ = wangle::TransportInfo();
  transportInfo_.secure = true;

  auto fizzClient = new AsyncFizzClient(eventBase, context);
  socket_.reset(fizzClient);

  connectStart_ = getCurrentTime();

  fizzClient->connect(connectAddr,
                      this,
                      std::move(verifier),
                      std::move(sni),
                      std::move(pskIdentity),
                      std::move(totalTimeout),
                      std::move(tcpConnectTimeout),
                      socketOptions,
                      bindAddr);
}

void HTTPConnectorWithFizz::connectSuccess() noexcept {
  if (!cb_) {
    return;
  }

  auto transport = socket_->getUnderlyingTransport<AsyncFizzClient>();

  if (!transport) {
    // Not a fizz socket, fall back to the parent one.
    HTTPConnector::connectSuccess();
    return;
  }

  folly::SocketAddress localAddress, peerAddress;
  socket_->getLocalAddress(&localAddress);
  socket_->getPeerAddress(&peerAddress);

  transportInfo_.acceptTime = getCurrentTime();
  transportInfo_.appProtocol =
      std::make_shared<std::string>(transport->getApplicationProtocol());
  transportInfo_.sslSetupTime = millisecondsSince(connectStart_);
  auto negotiatedCipher = transport->getState().cipher();
  transportInfo_.sslCipher =
      negotiatedCipher
          ? std::make_shared<std::string>(fizz::toString(*negotiatedCipher))
          : nullptr;
  auto version = transport->getState().version();
  transportInfo_.sslVersion = version ? static_cast<int>(version.value()) : 0;
  auto pskType =
      transport->getState().pskType().value_or(fizz::PskType::NotAttempted);
  transportInfo_.sslResume = pskType == fizz::PskType::Resumption
                                 ? wangle::SSLResumeEnum::RESUME_TICKET
                                 : wangle::SSLResumeEnum::HANDSHAKE;
  transportInfo_.securityType = transport->getSecurityProtocol();
  std::unique_ptr<HTTPCodec> codec = httpCodecFactory_->getCodec(
      socket_->getApplicationProtocol(), TransportDirection::UPSTREAM, true);
  HTTPUpstreamSession* session = new HTTPUpstreamSession(timeout_,
                                                         std::move(socket_),
                                                         localAddress,
                                                         peerAddress,
                                                         std::move(codec),
                                                         transportInfo_,
                                                         nullptr);

  cb_->connectSuccess(session);
}
} // namespace proxygen
