#pragma once

#include <bsio/wrapper/internal/Option.hpp>
#include <bsio/wrapper/internal/Common.hpp>

namespace bsio { namespace net { namespace wrapper {

    class HttpConnectorBuilder
    {
    public:
        virtual ~HttpConnectorBuilder() = default;

        HttpConnectorBuilder& WithConnector(TcpConnector::Ptr connector) noexcept
        {
            mConnector = std::move(connector);
            return *this;
        }

        HttpConnectorBuilder& WithEndpoint(asio::ip::tcp::endpoint endpoint) noexcept
        {
            mSocketOption.endpoint = std::move(endpoint);
            return *this;
        }

        HttpConnectorBuilder& WithTimeout(std::chrono::nanoseconds timeout) noexcept
        {
            mSocketOption.timeout = timeout;
            return *this;
        }

        HttpConnectorBuilder& WithFailedHandler(SocketFailedConnectHandler handler) noexcept
        {
            mSocketOption.failedHandler = std::move(handler);
            return *this;
        }

        HttpConnectorBuilder& AddSocketProcessingHandler(SocketProcessingHandler handler) noexcept
        {
            mSocketOption.socketProcessingHandlers.push_back(std::move(handler));
            return *this;
        }

        // TODO::WithClosedHandler

        HttpConnectorBuilder& WithEnterCallback(http::HttpSession::EnterCallback callback) noexcept
        {
            mEnterCallback = std::move(callback);
            return *this;
        }

        HttpConnectorBuilder& WithParserCallback(http::HttpSession::HttpParserCallback callback) noexcept
        {
            mParserCallback = std::move(callback);
            return *this;
        }

        HttpConnectorBuilder& WithWsCallback(http::HttpSession::WsCallback handler) noexcept
        {
            mWsCallback = std::move(handler);
            return *this;
        }

        HttpConnectorBuilder& WithRecvBufferSize(size_t size) noexcept
        {
            mTcpSessionOption.recvBufferSize = size;
            return *this;
        }

        void asyncConnect()
        {
            if (mConnector == nullptr)
            {
                throw std::runtime_error("connector is nullptr");
            }

            setupHttp();

            mConnector->asyncConnect(
                mSocketOption.endpoint,
                mSocketOption.timeout,
                mSocketOption.establishHandler,
                mSocketOption.failedHandler,
                mSocketOption.socketProcessingHandlers);
        }

    private:
        void setupHttp()
        {
            mSocketOption.establishHandler = common::generateHttpEstablishHandler(
                    mTcpSessionOption,
                    mEnterCallback,
                    mParserCallback,
                    mWsCallback);
        }

    private:
        TcpConnector::Ptr                       mConnector;
        internal::SocketConnectOption           mSocketOption;
        internal::TcpSessionOption              mTcpSessionOption;

        http::HttpSession::EnterCallback        mEnterCallback;
        http::HttpSession::HttpParserCallback   mParserCallback;
        http::HttpSession::WsCallback           mWsCallback;
    };

} } }