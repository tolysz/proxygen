#include <proxygen/httpserver/samples/hq/HQServer.h>

#include <ostream>
#include <string>

#include <boost/algorithm/string.hpp>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/HTTPTransactionHandlerAdaptor.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>
#include <proxygen/httpserver/samples/hq/HQLoggerHelper.h>
#include <proxygen/httpserver/samples/hq/HQParams.h>
#include <proxygen/httpserver/samples/hq/SampleHandlers.h>
#include <proxygen/lib/http/session/HQDownstreamSession.h>
#include <proxygen/lib/http/session/HTTPSessionController.h>
#include <proxygen/lib/utils/WheelTimerInstance.h>
#include <quic/congestion_control/CongestionControllerFactory.h>
#include <quic/logging/FileQLogger.h>
#include <quic/server/QuicServer.h>
#include <quic/server/QuicServerTransport.h>
#include <quic/server/QuicSharedUDPSocketFactory.h>

namespace quic { namespace samples {
using fizz::server::FizzServerContext;
using proxygen::HQDownstreamSession;
using proxygen::HQSession;
using proxygen::HTTPException;
using proxygen::HTTPMessage;
using proxygen::HTTPSessionBase;
using proxygen::HTTPTransaction;
using proxygen::HTTPTransactionHandler;
using proxygen::HTTPTransactionHandlerAdaptor;
using proxygen::RequestHandler;
using quic::QuicServerTransport;
using quic::QuicSocket;

static std::atomic<bool> shouldPassHealthChecks{true};

HTTPTransactionHandler* Dispatcher::getRequestHandler(HTTPMessage* msg,
                                                      const HQParams& params) {
  DCHECK(msg);
  const std::string& path = msg->getPath();
  if (path == "/" || path == "/echo") {
    return new EchoHandler(params);
  }
  if (path == "/continue") {
    return new ContinueHandler(params);
  }
  if (path.size() > 1 && path[0] == '/' && std::isdigit(path[1])) {
    return new RandBytesGenHandler(params);
  }
  if (path == "/status") {
    return new HealthCheckHandler(shouldPassHealthChecks, params);
  }
  if (path == "/status_ok") {
    shouldPassHealthChecks = true;
    return new HealthCheckHandler(true, params);
  }
  if (path == "/status_fail") {
    shouldPassHealthChecks = false;
    return new HealthCheckHandler(true, params);
  }
  if (path == "/wait" || path == "/release") {
    return new WaitReleaseHandler(
        folly::EventBaseManager::get()->getEventBase(), params);
  }
  if (path == "/pr_cat") {
    return new PrCatHandler(params);
  }
  if (boost::algorithm::starts_with(path, "/push")) {
    return new ServerPushHandler(params);
  }

  if (path == "/pr_scripted_skip") {
    return new PrSkipHandler(params);
  }

  if (path == "/pr_scripted_reject") {
    return new PrRejectHandler(params);
  }

  return new DummyHandler(params);
}

void outputQLog(const HQParams& params) {
}

HQSessionController::HQSessionController(const HQParams& params)
    : params_(params) {
}

HQSession* HQSessionController::createSession() {
  wangle::TransportInfo tinfo;
  session_ = new HQDownstreamSession(params_->txnTimeout, this, tinfo, this);
  return session_;
}

void HQSessionController::startSession(std::shared_ptr<QuicSocket> sock) {
  CHECK(session_);
  session_->setSocket(std::move(sock));
  session_->startNow();
}

void HQSessionController::onDestroy(const HTTPSessionBase&) {
}

HTTPTransactionHandler* HQSessionController::getRequestHandler(
    HTTPTransaction& /*txn*/, HTTPMessage* msg) {
  return Dispatcher::getRequestHandler(msg, params_);
}

HTTPTransactionHandler* FOLLY_NULLABLE
HQSessionController::getParseErrorHandler(
    HTTPTransaction* /*txn*/,
    const HTTPException& /*error*/,
    const folly::SocketAddress& /*localAddress*/) {
  return nullptr;
}

HTTPTransactionHandler* FOLLY_NULLABLE
HQSessionController::getTransactionTimeoutHandler(
    HTTPTransaction* /*txn*/, const folly::SocketAddress& /*localAddress*/) {
  return nullptr;
}

void HQSessionController::attachSession(HTTPSessionBase* /*session*/) {
}

void HQSessionController::detachSession(const HTTPSessionBase* /*session*/) {
  delete this;
}

HQServerTransportFactory::HQServerTransportFactory(const HQParams& params)
    : params_(params) {
}

QuicServerTransport::Ptr HQServerTransportFactory::make(
    folly::EventBase* evb,
    std::unique_ptr<folly::AsyncUDPSocket> socket,
    const folly::SocketAddress& /* peerAddr */,
    std::shared_ptr<const FizzServerContext> ctx) noexcept {
  // Session controller is self owning
  auto hqSessionController = new HQSessionController(params_);
  auto session = hqSessionController->createSession();
  CHECK_EQ(evb, socket->getEventBase());
  auto transport =
      QuicServerTransport::make(evb, std::move(socket), *session, ctx);
  if (!params_->qLoggerPath.empty()) {
    transport->setQLogger(std::make_shared<HQLoggerHelper>(
        params_->qLoggerPath, params_->prettyJson, kQLogServerVantagePoint));
  }
  hqSessionController->startSession(transport);
  return transport;
}

HQServer::HQServer(const HQParams& params)
    : params_(params), server_(quic::QuicServer::createQuicServer()) {
  server_->setCongestionControllerFactory(
      std::make_shared<DefaultCongestionControllerFactory>());
  server_->setTransportSettings(params_->transportSettings);
  server_->setQuicServerTransportFactory(
      std::make_unique<HQServerTransportFactory>(params_));
  server_->setQuicUDPSocketFactory(
      std::make_unique<QuicSharedUDPSocketFactory>());
  server_->setHealthCheckToken("health");
  server_->setSupportedVersion(params_->quicVersions);
  server_->setFizzContext(createFizzServerContext(params_));
}

void HQServer::setTlsSettings(const HQParams& params) {
  server_->setFizzContext(createFizzServerContext(params));
}

void HQServer::start() {
  server_->start(params_->localAddress.value(),
                 std::thread::hardware_concurrency());
}

void HQServer::run() {
  eventbase_.loopForever();
}

const folly::SocketAddress HQServer::getAddress() const {
  server_->waitUntilInitialized();
  const auto& boundAddr = server_->getAddress();
  LOG(INFO) << "HQ server started at: " << boundAddr.describe();
  return boundAddr;
}

void HQServer::stop() {
  server_->shutdown();
  eventbase_.terminateLoopSoon();
}

void HQServer::rejectNewConnections(bool reject) {
  server_->rejectNewConnections(reject);
}

H2Server::SampleHandlerFactory::SampleHandlerFactory(const HQParams& params)
    : params_(params) {
}

void H2Server::SampleHandlerFactory::onServerStart(
    folly::EventBase* /*evb*/) noexcept {
}

void H2Server::SampleHandlerFactory::onServerStop() noexcept {
}

RequestHandler* H2Server::SampleHandlerFactory::onRequest(
    RequestHandler*, HTTPMessage* msg) noexcept {
  return new HTTPTransactionHandlerAdaptor(
      Dispatcher::getRequestHandler(msg, params_));
}

std::unique_ptr<proxygen::HTTPServerOptions> H2Server::createServerOptions(
    const HQParams& params) {
  auto serverOptions = std::make_unique<proxygen::HTTPServerOptions>();

  serverOptions->threads = params->httpServerThreads;
  serverOptions->idleTimeout = params->httpServerIdleTimeout;
  serverOptions->shutdownOn = params->httpServerShutdownOn;
  serverOptions->enableContentCompression =
      params->httpServerEnableContentCompression;
  serverOptions->initialReceiveWindow =
      params->transportSettings.advertisedInitialBidiLocalStreamWindowSize;
  serverOptions->receiveStreamWindowSize =
      params->transportSettings.advertisedInitialBidiLocalStreamWindowSize;
  serverOptions->receiveSessionWindowSize =
      params->transportSettings.advertisedInitialConnectionWindowSize;
  serverOptions->handlerFactories = proxygen::RequestHandlerChain()
                                        .addThen<SampleHandlerFactory>(params)
                                        .build();
  return serverOptions;
}

std::unique_ptr<H2Server::AcceptorConfig> H2Server::createServerAcceptorConfig(
    const HQParams& params) {
  auto acceptorConfig = std::make_unique<AcceptorConfig>();
  proxygen::HTTPServer::IPConfig ipConfig(
      params->localH2Address.value(), proxygen::HTTPServer::Protocol::HTTP2);
  ipConfig.sslConfigs.emplace_back(createSSLContext(params));
  acceptorConfig->push_back(ipConfig);
  return acceptorConfig;
}

std::thread H2Server::run(const HQParams& params) {

  // Start HTTPServer mainloop in a separate thread
  std::thread t([params = params]() mutable {
    {
      auto acceptorConfig = createServerAcceptorConfig(params);
      auto serverOptions = createServerOptions(params);
      proxygen::HTTPServer server(std::move(*serverOptions));
      server.bind(std::move(*acceptorConfig));
      server.start();
    }
    // HTTPServer traps the SIGINT.  resignal HQServer
    raise(SIGINT);
  });

  return t;
}

void startServer(const HQParams& params) {
  // Run H2 server in a separate thread
  auto h2server = H2Server::run(params);
  // Run HQ server
  HQServer server(params);
  server.start();
  // Wait until the quic server initializes
  server.getAddress();
  // Start HQ sever event loop
  server.run();
  h2server.join();
}

}} // namespace quic::samples
