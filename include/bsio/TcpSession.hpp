#pragma once

#include <algorithm>
#include <memory>
#include <functional>
#include <mutex>
#include <deque>
#include <cmath>
#include <iostream>

#include <asio/socket_base.hpp>
#include <asio.hpp>

namespace bsio { namespace net {

    const size_t MinReceivePrepareSize = 1024;

    class TcpSession :  public asio::noncopyable, 
                        public std::enable_shared_from_this<TcpSession>
    {
    public:
        using Ptr = std::shared_ptr<TcpSession>;
        using DataHandler = std::function<size_t(Ptr, const char*, size_t)>;
        using ClosedHandler = std::function<void(Ptr)>;
        using SendCompletedCallback = std::function<void()>;

        static Ptr Make(
            asio::ip::tcp::socket socket,
            size_t maxRecvBufferSize,
            DataHandler dataHandler,
            ClosedHandler closedHandler)
        {
            class make_shared_enabler : public TcpSession
            {
            public:
                make_shared_enabler(
                    asio::ip::tcp::socket socket,
                    size_t maxRecvBufferSize,
                    DataHandler dataHandler,
                    ClosedHandler closedHandler)
                    :
                    TcpSession(std::move(socket), 
                        maxRecvBufferSize, 
                        std::move(dataHandler),
                        std::move(closedHandler))
                {}
            };

            auto session = std::make_shared<make_shared_enabler>(
                std::move(socket), 
                maxRecvBufferSize, 
                std::move(dataHandler),
                std::move(closedHandler));

            session->startRecv();

            return std::static_pointer_cast<TcpSession>(session);
        }

        virtual ~TcpSession() = default;

        auto    runAfter(std::chrono::nanoseconds timeout, std::function<void(void)> callback)
        {
            auto timer = std::make_shared<asio::steady_timer>(mSocket.get_executor());
            timer->expires_from_now(timeout);
            timer->async_wait([callback = std::move(callback), timer](const asio::error_code & ec)
                              {
                                  if (!ec)
                                  {
                                      callback();
                                  }
                              });
            return timer;
        }

        void    asyncSetDataHandler(DataHandler dataHandler)
        {
            asio::post(mSocket.get_executor(),
                       [self = shared_from_this(), this, dataHandler = std::move(dataHandler)]() mutable
                       {
                            mDataHandler = std::move(dataHandler);
                            tryProcessRecvBuffer();
                            tryAsyncRecv();
                       });
        }

        void    postClose() noexcept
        {
            asio::post(mSocket.get_executor(),
                       [self = shared_from_this(), this]()
                       {
                           mSocket.close();
                       });
        }

        void    postShutdown(asio::ip::tcp::socket::shutdown_type type) noexcept
        {
            asio::post(mSocket.get_executor(),
                       [self = shared_from_this(), this, type]()
                       {
                           mSocket.shutdown(type);
                       });
        }

        void    send(std::shared_ptr<std::string> msg, SendCompletedCallback callback = nullptr) noexcept
        {
            {
                std::lock_guard<std::mutex> lck(mSendGuard);
                mPendingSendMsg.push_back({ 0, std::move(msg), std::move(callback) });
            }
            trySend();
        }

        void    send(std::string msg, SendCompletedCallback callback = nullptr) noexcept
        {
            send(std::make_shared<std::string>(std::move(msg)), std::move(callback));
        }

    private:
        TcpSession(
            asio::ip::tcp::socket socket,
            size_t maxRecvBufferSize,
            DataHandler dataHandler,
            ClosedHandler closedHandler)
            :
                mSocket(std::move(socket)),
                mSending(false),
                mDataHandler(std::move(dataHandler)),
                mReceiveBuffer(std::max<size_t>(MinReceivePrepareSize, maxRecvBufferSize)),
                mCurrentPrepareSize(std::min<size_t>(MinReceivePrepareSize, maxRecvBufferSize)),
                mClosedHandler(std::move(closedHandler)),
                mCurrentTanhXDiff(0)
        {
            mSocket.non_blocking(true);
            mSocket.set_option(asio::ip::tcp::no_delay(true));
        }

        void    startRecv()
        {
            std::call_once(mRecvInitOnceFlag,
                           [self = shared_from_this(), this]()
                           {
                               tryAsyncRecv();
                           });
        }

        void    tryAsyncRecv()
        {
            if(mInRecv)
            {
                return;
            }

            try
            {
                mSocket.async_receive(
                        mReceiveBuffer.prepare(mCurrentPrepareSize - mReceiveBuffer.in_avail()),
                        [self = shared_from_this(), this](std::error_code ec, size_t bytesTransferred)
                        {
                            onRecvCompleted(ec, bytesTransferred);
                        });
            }
            catch (const std::length_error& ec)
            {
                std::cout << "do recv, cause error of async receive:" << ec.what() << std::endl;
                //TODO::callback to user
            }
        }

        void    onRecvCompleted(std::error_code ec, size_t bytesTransferred)
        {
            mInRecv = false;

            if (ec)
            {
                //TODO::处理error code
                return;
            }

            if((mCurrentPrepareSize-mReceiveBuffer.in_avail()) == bytesTransferred)
            {
                const auto TanhXDiff = 0.2;

                const auto oldTanh = std::tanh(mCurrentTanhXDiff);
                mCurrentTanhXDiff += TanhXDiff;
                const auto newTanh = std::tanh(mCurrentTanhXDiff);
                const auto maxSizeDiff = mReceiveBuffer.max_size() -
                        std::min<size_t>(mReceiveBuffer.max_size(), MinReceivePrepareSize);
                const auto sizeDiff = maxSizeDiff * (newTanh-oldTanh);

                mCurrentPrepareSize += sizeDiff;
                mCurrentPrepareSize = std::min<size_t>(mCurrentPrepareSize, mReceiveBuffer.max_size());
            }
            mReceiveBuffer.commit(bytesTransferred);

            tryProcessRecvBuffer();

            tryAsyncRecv();
        }

        void    trySend()
        {
            std::lock_guard<std::mutex> lck(mSendGuard);
            if (mSending || mPendingSendMsg.empty())
            {
                return;
            }

            mBuffers.resize(mPendingSendMsg.size());
            for (std::size_t i = 0; i < mPendingSendMsg.size(); ++i)
            {
                auto& msg = mPendingSendMsg[i];
                mBuffers[i] = asio::const_buffer(msg.msg->c_str() + msg.sendPos,
                    msg.msg->size() - msg.sendPos);
            }

            mSending = true;
            mSocket.async_send(mBuffers,
                    [self = shared_from_this(), this](std::error_code ec, size_t bytesTransferred)
                    {
                        onSendCompleted(ec, bytesTransferred);
                    });
        }

        void    onSendCompleted(std::error_code ec, size_t bytesTransferred)
        {
            std::vector<SendCompletedCallback> completedCallbacks;
            {
                std::lock_guard<std::mutex> lck(mSendGuard);
                mSending = false;
                if (ec)
                {
                    // TODO::错误回调
                    return;
                }
                completedCallbacks = adjustSendBuffer(bytesTransferred);
            }
            for (const auto& callback : completedCallbacks)
            {
                callback();
            }

            trySend();
        }

        std::vector<SendCompletedCallback>  adjustSendBuffer(size_t bytesTransferred)
        {
            std::vector<SendCompletedCallback> completedCallbacks;

            while (bytesTransferred > 0)
            {
                auto& frontMsg = mPendingSendMsg.front();
                const auto len = std::min<size_t>(bytesTransferred, frontMsg.msg->size() - frontMsg.sendPos);
                frontMsg.sendPos += len;
                bytesTransferred -= len;
                if (frontMsg.sendPos == frontMsg.msg->size())
                {
                    if (frontMsg.callback)
                    {
                        completedCallbacks.push_back(std::move(frontMsg.callback));
                    }
                    mPendingSendMsg.pop_front();
                }
            }

            return completedCallbacks;
        }

        void    tryProcessRecvBuffer()
        {
            if (mDataHandler == nullptr)
            {
                return;
            }

            const auto validReadBuffer = mReceiveBuffer.data();
            const auto procLen = mDataHandler(shared_from_this(),
                                              static_cast<const char* >(validReadBuffer.data()),
                                              validReadBuffer.size());
            assert(procLen <= validReadBuffer.size());
            if (procLen <= validReadBuffer.size())
            {
                mReceiveBuffer.consume(procLen);
            }
            else
            {
                ;//throw
            }
        }
    private:
        asio::ip::tcp::socket               mSocket;

        // 同时只能发起一次send writev请求
        bool                                mSending;
        std::mutex                          mSendGuard;
        struct PendingMsg
        {
            size_t  sendPos;
            std::shared_ptr<std::string>    msg;
            SendCompletedCallback           callback;
        };

        std::deque<PendingMsg>              mPendingSendMsg;
        std::vector<asio::const_buffer>     mBuffers;

        std::once_flag                      mRecvInitOnceFlag;
        bool                                mInRecv = false;
        DataHandler                         mDataHandler;
        asio::streambuf                     mReceiveBuffer;
        size_t                              mCurrentPrepareSize;
        ClosedHandler                       mClosedHandler;
        double                              mCurrentTanhXDiff;
    };

    using TcpSessionEstablishHandler = std::function<void(TcpSession::Ptr)>;

} }