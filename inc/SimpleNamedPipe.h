#pragma once

#include <windows.h>
#include <memory>
#include <functional>
#include <optional>

#include <winrt/base.h>

#include <ppl.h>
#include <ppltasks.h>

namespace abt::comm::simple_pipe
{
    //バッファーサイズ
    constexpr DWORD TYPICAL_BUFFER_SIZE = 2048;

    /// <summary>
    /// イベント種別
    /// </summary>
    enum PipeEventType {
        //接続
        CONNECTED,
        //切断
        DISCONNECTED,
        //受信
        RECEIVED,
        //例外発生
        EXCEPTION,
    };

    /// <summary>
    /// 受信イベント
    /// </summary>
    struct PipeEventParam {
        //イベント種別
        const PipeEventType type;
        //受信データ。受信データはコールバック中でのみ有効。
        LPCVOID readBuffer;
        //受信データサイズ
        const size_t readedSize;
        //例外発生時の監視タスク
        const std::optional<concurrency::task<void>> errTask;
    };

    /// <summary>
    /// 名前付きパイプ共通ベースクラス
    /// </summary>
    template<DWORD BUF_SIZE>
    class SimpleNamedPipeBase
    {
    public:
        using ReceivedCallback = std::function<void(LPCVOID, size_t)>;

    private:
        //パイプハンドル
        winrt::file_handle handlePipe;
        //受信コールバック
        ReceivedCallback receivedCallback;
        //受信用オーバーラップ構造体
        OVERLAPPED readOverlap ;
        //受信バッファー
        std::unique_ptr<BYTE[]> readBuffer;
        //受信イベント
        winrt::handle readEvent;

    public:
        static const DWORD BUFFER_SIZE = BUF_SIZE;

        SimpleNamedPipeBase() = default;
        SimpleNamedPipeBase(SimpleNamedPipeBase&&) = default;
        SimpleNamedPipeBase(const SimpleNamedPipeBase&) = delete;

        SimpleNamedPipeBase& operator=(SimpleNamedPipeBase&&) = default;
        SimpleNamedPipeBase& operator=(const SimpleNamedPipeBase&) = delete;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="handlePipe">パイプハンドル</param>
        /// <param name="receivedCallback">データ受信コールバック。受信データは関数呼び出し中でのみ有効。</param>
        SimpleNamedPipeBase(winrt::file_handle&& handlePipe, ReceivedCallback receivedCallback)
            : handlePipe(std::move(handlePipe))
            , receivedCallback(receivedCallback)
            , readOverlap({ 0 })
            , readBuffer(std::make_unique<BYTE[]>(BUF_SIZE))
        {
            readEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(static_cast<bool>(readEvent));
            readOverlap.hEvent = readEvent.get();
        }

        virtual ~SimpleNamedPipeBase() {}

        const HANDLE Handle() { return handlePipe.get(); }
        const bool Valid() { return static_cast<bool>(handlePipe); }
        const std::unique_ptr<BYTE[]>& ReadBuffer() { return readBuffer; }
        const winrt::handle& ReadEvent() { return readEvent; }

        /// <summary>
        /// 非同期受信開始
        /// </summary>
        /// <returns>false時は切断状態</returns>
        virtual bool OverappedRead()
        {
            //受信イベントリセット
            winrt::check_bool(ResetEvent(readEvent.get()));
            readOverlap.Offset = 0;
            readOverlap.OffsetHigh = 0;
            //受信処理
            // 同期的の受信できる限りは受信処理を継続
            while (ReadFile(handlePipe.get(), readBuffer.get(), BUF_SIZE, nullptr, &readOverlap)) {
                if (!OnRead()) {
                    //切断状態となった
                    return false;
                }
            }
            //同期的に受信データを取得できないかエラーの場合
            auto lastErr = GetLastError();
            if (ERROR_HANDLE_EOF == lastErr || ERROR_BROKEN_PIPE == lastErr) {
                //切断状態
                return false;
            }
            if (ERROR_IO_PENDING != lastErr) {
                //I/O完了待ち以外ではエラー
                winrt::throw_last_error();
            }
            return true;
        }

        /// <summary>
        /// 非同期受信完了時の処理
        /// </summary>
        /// <returns>false時は切断状態</returns>
        virtual bool OnRead()
        {
            DWORD readSize = 0;
            if (!GetOverlappedResult(handlePipe.get(), &readOverlap, &readSize, FALSE)) {
                auto lastErr = GetLastError();
                if (lastErr == ERROR_IO_INCOMPLETE) {
                    //データ受信を完了していない
                    return true;
                }
                if (ERROR_HANDLE_EOF == lastErr || ERROR_BROKEN_PIPE == lastErr) {
                    //切断状態
                    return false;
                }
                //エラー
                winrt::throw_last_error();
            }
            //データ受信
            receivedCallback(readBuffer.get(), readSize);
            return true;
        }

        /// <summary>
        /// 受信イベントシグナル時の処理
        /// </summary>
        /// <returns>false時は切断状態</returns>
        virtual bool OnSignalRead()
        {
            //非同期受信完了時処理実行
            auto connected = OnRead();
            if (connected) {
                //切断していなければ、次回分の非同期受信処理を実行
                connected = OverappedRead();
            }
            return connected;
        }

        /// <summary>
        /// 非同期送信処理
        /// </summary>
        /// <param name="buffer">送信バッファー</param>
        /// <param name="size">送信サイズ</param>
        /// <param name="ct">キャンセルトークン</param>
        /// <returns>非同期タスク</returns>
        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct)
        {
            if (!static_cast<bool>(handlePipe)) {
                //handleが無効
                throw std::runtime_error("this instance is invalid");
            }

            if (size > BUF_SIZE) {
                //バッファーサイズより大きい場合はエラー
                throw std::length_error("size is too long");
            }

            //送信完了イベント
            winrt::handle writeEvent{CreateEventW(nullptr, true, false, nullptr)};
            winrt::check_bool(static_cast<bool>(writeEvent));
            //オーバーラップ構造体の設定
            OVERLAPPED overlapped = { 0 };
            overlapped.hEvent = writeEvent.get();
            //非同期送信
            if (WriteFile(handlePipe.get(), buffer, static_cast<DWORD>(size), nullptr, &overlapped)) {
                //同期で処理完了
                return concurrency::task_from_result();
            }
            auto lastErr = GetLastError();
            if (ERROR_IO_PENDING != lastErr) {
                //完了待ち以外ではエラー
                winrt::throw_last_error();
            }
            //taskに引き継ぐためイベントはデタッチする
            writeEvent.detach();

            //非同期時の送信完了町のためのタスク
            return concurrency::create_task([this, overlapped, ct]() {
                std::vector<HANDLE> handles;
                winrt::handle cancelEvent;
                if (ct.is_cancelable()) {
                    //キャンセル可能な場合のイベントディスパッチ処理
                    cancelEvent.attach(CreateEventW(nullptr, true, false, nullptr));
                    winrt::check_bool(static_cast<bool>(cancelEvent));
                    concurrency::cancellation_token_registration cookie;
                    cookie = ct.register_callback([&cancelEvent, ct, &cookie]() {
                        SetEvent(cancelEvent.get());
                        ct.deregister_callback(cookie);
                        });
                    handles.emplace_back(cancelEvent.get());
                }

                auto writeEvent = winrt::handle(overlapped.hEvent);
                handles.emplace_back(writeEvent.get());

                //書き込み完了、もしくは中断まで待機
                auto res = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), &handles[0], false, INFINITE);
                auto index = res - WAIT_OBJECT_0;
                if (0 <= index && index < handles.size()) {
                    //書き込み完了、もしくはキャンセル
                    ResetEvent(handles[index]);
                }
                return;
                }, concurrency::task_options(ct));
        }

        void Close()
        {
            if (static_cast<bool>(handlePipe)) {
                handlePipe.close();
            }
        }
    };

    /// <summary>
    /// 名前付きパイプサーバークラス
    /// </summary>
    template<DWORD BUF_SIZE>
    class SimpleNamedPipeServer
    {
    public:
        static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
        using Callback = std::function<void(SimpleNamedPipeServer<BUF_SIZE>&, const PipeEventParam&)>;
    private:
        using Base = SimpleNamedPipeBase<BUF_SIZE>;
        Base base;

        Callback callback;
        OVERLAPPED connectionOverlap;
        winrt::handle connectionEvent;
        concurrency::task<void> watcherTask;

        void Received(LPCVOID buffer, size_t size)
        {
            callback(*this, PipeEventParam{ PipeEventType::RECEIVED, buffer, size });
        }

        /// <summary>
        /// イベント監視タスク
        /// </summary>
        /// <returns>非同期タスク</returns>
        virtual concurrency::task<void> WatchAsync()
        {
            return concurrency::create_task([this]() {
                while (true) {
                    //接続、受信イベントを監視
                    const HANDLE handles[]{ connectionEvent.get(), base.ReadEvent().get() };
                    auto res = WaitForMultipleObjects(_countof(handles), handles, false, INFINITE);
                    if (res == WAIT_FAILED) {
                        //エラー
                        winrt::throw_last_error();
                    }
                    if (!base.Valid()) {
                        //ハンドルが破棄されていたら終了
                        break;
                    }
                    auto index = res - WAIT_OBJECT_0;
                    if (0 <= index && index < _countof(handles)) {
                        bool connected = false;
                        ResetEvent(handles[index]);
                        if (connectionEvent.get() == handles[index]) {
                            //非同期受信処理開始
                            connected = base.OverappedRead();
                            //接続イベント
                            OnConnected();
                        }
                        else if (base.ReadEvent().get() == handles[index]) {
                            //受信イベント
                            connected = base.OnSignalRead();
                        }
                        if (!connected) {
                            //切断検知
                            //切断コールバック
                            callback(*this, PipeEventParam{ PipeEventType::DISCONNECTED, nullptr, 0 });
                            //終了した接続の後始末をして次回接続の待機
                            winrt::check_bool(DisconnectNamedPipe(base.Handle()));
                            BeginConnect();
                        }
                    }
                    else {
                        auto index2 = res - WAIT_ABANDONED_0;
                        if (0 <= index2 && index2 < _countof(handles)) {
                            //いずれかのハンドルが破棄されたのならインスタンスが破棄されている
                            break;
                        }
                    }
                }
                //監視スレッド終了後はこのインスタンスは利用できない
                Close();
                });
        }

        /// <summary>
        /// 次回の接続待ち開始
        /// </summary>
        virtual void BeginConnect()
        {
            if (!ConnectNamedPipe(base.Handle(), &connectionOverlap)) {
                auto lastErr = GetLastError();
                if (ERROR_PIPE_LISTENING != lastErr && ERROR_IO_PENDING != lastErr && ERROR_PIPE_CONNECTED != lastErr) {
                    //ペンディングではない場合の名前付きパイプ接続失敗
                    winrt::throw_last_error();
                }
            }
        }

        /// <summary>
        /// 接続時処理
        /// </summary>
        virtual void OnConnected()
        {
            callback(*this, PipeEventParam{ PipeEventType::CONNECTED, nullptr, 0 });
        }

    public:
        SimpleNamedPipeServer() = default;
        SimpleNamedPipeServer(SimpleNamedPipeServer&&) = default;
        SimpleNamedPipeServer(const SimpleNamedPipeServer&) = delete;

        SimpleNamedPipeServer& operator=(SimpleNamedPipeServer&&) = default;
        SimpleNamedPipeServer& operator=(const SimpleNamedPipeServer&) = delete;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="psa">セキュリティディスクリプタ</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeServer(LPCWSTR name, LPSECURITY_ATTRIBUTES psa, Callback callback)
            : callback(callback)
            , connectionOverlap({ 0 })
        {
            //名前付きパイプの作成
            // 接続可能クライアント数=1; ローカルマシン接続のみ許可
            winrt::file_handle handlePipe{
                CreateNamedPipeW(
                    name,
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
                    1,
                    BUF_SIZE,
                    BUF_SIZE,
                    0,
                    psa)
            };
            if (!static_cast<bool>(handlePipe)) {
                winrt::throw_last_error();
            }
            //ベース処理の移譲用クラスを作成
            base = Base(std::move(handlePipe), std::bind(&SimpleNamedPipeServer::Received, this, std::placeholders::_1, std::placeholders::_2));

            //接続イベント作成
            connectionEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(static_cast<bool>(connectionEvent));
            //接続用オーバーラップ構造体の初期化
            connectionOverlap.hEvent = connectionEvent.get();

            //接続待ち開始
            BeginConnect();

            //監視タスク開始
            watcherTask = WatchAsync().then([this](concurrency::task<void> prevTask) {
                try {
                    prevTask.wait();
                }
                catch (...)
                {
                    //監視タスクが例外発生で終了していた場合
                    this->callback(*this, PipeEventParam{ PipeEventType::EXCEPTION, nullptr, 0, prevTask });
                }
                });
        }

        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct)
        {
            return base.WriteAsync(buffer, size, ct);
        }

        bool Valid()
        {
            return base.Valid();
        }

        void Close()
        {
            base.Close();
        }

        virtual ~SimpleNamedPipeServer()
        {
        }
    };

    using TypicalSimpleNamedPipeServer = SimpleNamedPipeServer<TYPICAL_BUFFER_SIZE>;

    /// <summary>
    /// 名前付きパイプクライアント
    /// </summary>
    template<DWORD BUF_SIZE>
    class SimpleNamedPipeClient
    {
    public:
        static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
        using Callback = std::function<void(SimpleNamedPipeClient<BUF_SIZE>&, const PipeEventParam&)>;
    private:
        using Base = SimpleNamedPipeBase<BUF_SIZE>;
        Base base;

        Callback callback;
        concurrency::task<void> watcherTask;

        void Received(LPCVOID buffer, size_t size)
        {
            callback(*this, PipeEventParam{ PipeEventType::RECEIVED, buffer, size });
        }

        /// <summary>
        /// イベント監視タスク
        /// </summary>
        /// <returns>非同期タスク</returns>
        virtual concurrency::task<void> WatchAsync()
        {
            return concurrency::create_task([this]() {
                while (true) {
                    const HANDLE handles[]{ base.ReadEvent().get() };
                    auto res = WaitForMultipleObjects(_countof(handles), handles, false, INFINITE);
                    if (res == WAIT_FAILED) {
                        //エラー
                        winrt::throw_last_error();
                    }
                    if (!base.Valid()) {
                        //名前付きパイプのハンドルが無効値ならばインスタンスは破棄されている
                        break;
                    }
                    auto index = res - WAIT_OBJECT_0;
                    if (0 <= index && index < _countof(handles)) {
                        ResetEvent(handles[index]);
                        if (base.ReadEvent().get() == handles[index]) {
                            //読み込みイベント
                            if (!base.OnSignalRead()) {
                                //切断状態の場合
                                callback(*this, PipeEventParam{ PipeEventType::DISCONNECTED, nullptr, 0 });
                            }
                        }
                        else {
                            auto index2 = res - WAIT_ABANDONED_0;
                            if (0 <= index2 && index2 < _countof(handles)) {
                                //いずれかのハンドルが破棄されたのならインスタンスが破棄されている
                                break;
                            }
                        }
                    }
                }
                //監視スレッド終了後はこのインスタンスは利用できない
                Close();
                });
        }

    public:
        SimpleNamedPipeClient() = default;
        SimpleNamedPipeClient(const SimpleNamedPipeClient&) = delete;
        SimpleNamedPipeClient& operator=(const SimpleNamedPipeClient&) = delete;

        SimpleNamedPipeClient(SimpleNamedPipeClient&&) = default;
        SimpleNamedPipeClient& operator=(SimpleNamedPipeClient&&) = default;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeClient(LPCWSTR name, Callback callback)
            : callback(callback)
        {
            //サーバーへ接続
            winrt::file_handle handlePipe(CreateFileW(
                name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr));

            if (!static_cast<bool>(handlePipe)) {
                winrt::throw_last_error();
            }
            //ベース処理の以上用クラスを作成
            base = Base(std::move(handlePipe), std::bind(&SimpleNamedPipeClient::Received, this, std::placeholders::_1, std::placeholders::_2));

            //監視タスク開始
            watcherTask = WatchAsync().then([this](concurrency::task<void> prevTask) {
                try {
                    prevTask.wait();
                }
                catch (...)
                {
                    //監視タスクが例外発生で終了していた場合
                    this->callback(*this, PipeEventParam{ PipeEventType::EXCEPTION, nullptr, 0, prevTask });
                }
                });
            //非同期受信処理開始
            base.OverappedRead();
        }

        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct)
        {
            return base.WriteAsync(buffer, size, ct);
        }

        bool Valid()
        {
            return base.Valid();
        }

        void Close()
        {
            base.Close();
        }

        virtual ~SimpleNamedPipeClient()
        {
        }
    };

    using TypicalSimpleNamedPipeClient = SimpleNamedPipeClient<TYPICAL_BUFFER_SIZE>;
}
