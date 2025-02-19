/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/portability/GTest.h>

#include <fizz/protocol/test/Utilities.h>
#include <fizz/server/AsyncFizzServer.h>
#include <fizz/server/test/Mocks.h>
#include <fizz/server/test/Utils.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/ssl/Init.h>
#include <proxygen/lib/http/HTTPConnectorWithFizz.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>

using namespace proxygen;
using namespace testing;
using namespace folly;
using namespace fizz::server;

class MockHTTPConnectorCallback : public HTTPConnector::Callback {
 public:
  ~MockHTTPConnectorCallback() override = default;
  MOCK_METHOD1(connectSuccess, void(HTTPUpstreamSession* session));
  MOCK_METHOD1(connectError, void(const folly::AsyncSocketException& ex));
};

class HTTPConnectorWithFizzTest : public testing::Test {
 public:
  HTTPConnectorWithFizzTest()
      : evb_(true), factory_(&handshakeCb_), server_(evb_, &factory_) {
  }

  void SetUp() override {
    folly::ssl::init();

    timer_ = HHWheelTimer::newTimer(
        &evb_,
        std::chrono::milliseconds(HHWheelTimer::DEFAULT_TICK_INTERVAL),
        AsyncTimeout::InternalEnum::NORMAL,
        std::chrono::milliseconds(5000));
  }

 protected:
  class DummyCallbackFactory
      : public fizz::server::test::FizzTestServer::CallbackFactory {
   public:
    explicit DummyCallbackFactory(fizz::server::test::MockHandshakeCallback* cb)
        : cb_(cb) {
    }

    AsyncFizzServer::HandshakeCallback* getCallback(
        std::shared_ptr<AsyncFizzServer> srv) override {
      // Keep connection alive
      conn_ = srv;
      return cb_;
    }

   private:
    AsyncFizzServer::HandshakeCallback* cb_;
    std::shared_ptr<AsyncFizzServer> conn_;
  };
  void SetupFailureCallbacks() {
    ON_CALL(handshakeCb_, _fizzHandshakeError(_))
        .WillByDefault(Invoke([&](folly::exception_wrapper ex) {
          evb_.terminateLoopSoon();
          if (ex.what().toStdString().find("readEOF()") == std::string::npos) {
            FAIL() << "Server error handler called: "
                   << ex.what().toStdString();
          }
        }));
    ON_CALL(cb_, connectError(_))
        .WillByDefault(Invoke([&](const folly::AsyncSocketException& ex) {
          evb_.terminateLoopSoon();
          FAIL() << "Client error handler called: " << ex.what();
        }));
  }
  EventBase evb_;
  fizz::server::test::MockHandshakeCallback handshakeCb_;
  DummyCallbackFactory factory_;
  fizz::server::test::FizzTestServer server_;
  HHWheelTimer::UniquePtr timer_;
  MockHTTPConnectorCallback cb_;
};

TEST_F(HTTPConnectorWithFizzTest, TestFizzConnect) {
  SetupFailureCallbacks();
  HTTPConnectorWithFizz connector(&cb_, timer_.get());
  proxygen::HTTPUpstreamSession* session = nullptr;
  EXPECT_CALL(cb_, connectSuccess(_))
      .WillOnce(
          Invoke([&](proxygen::HTTPUpstreamSession* sess) { session = sess; }));

  auto context = std::make_shared<fizz::client::FizzClientContext>();
  connector.connectFizz(&evb_, server_.getAddress(), context, nullptr);
  EXPECT_CALL(handshakeCb_, _fizzHandshakeSuccess()).WillOnce(Invoke([&]() {
    evb_.terminateLoopSoon();
  }));
  evb_.loop();
  if (session) {
    session->dropConnection();
  }
}

TEST_F(HTTPConnectorWithFizzTest, TestFizzConnectFailure) {
  HTTPConnectorWithFizz connector(&cb_, timer_.get());

  auto serverContext = server_.getFizzContext();
  serverContext->setSupportedCiphers(
      {{fizz::CipherSuite::TLS_AES_128_GCM_SHA256}});

  auto context = std::make_shared<fizz::client::FizzClientContext>();
  context->setSupportedCiphers({fizz::CipherSuite::TLS_AES_256_GCM_SHA384});

  connector.connectFizz(&evb_, server_.getAddress(), context, nullptr);
  EXPECT_CALL(handshakeCb_, _fizzHandshakeError(_));
  EXPECT_CALL(cb_, connectError(_)).WillOnce(InvokeWithoutArgs([&]() {
    evb_.terminateLoopSoon();
  }));
  evb_.loop();
}
