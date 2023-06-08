#pragma once

#include <windows.h>
#include <limits>
#include <memory>
#include <functional>
#include <optional>
#include <winrt/base.h>
#include <ppl.h>
#include <ppltasks.h>

namespace abt::comm::simple_pipe
{
    using ReceivedCallback = std::function<void(LPCVOID, size_t)>;

    /// <summary>
    /// データパケット
    /// </summary>
    struct Packet
    {
        //size分も含めた全体のサイズ
        alignas(4) DWORD size;
        BYTE data[1];
    };

#pragma region Receiver
    /// <summary>
    /// 受信データ復号クラス
    /// </summary>
    class Receiver
    {
    private:
        /// <summary>
        /// 受信バッファー管理クラス
        /// </summary>
        class Buffer
        {
        private:
            const BYTE* sp;
            const BYTE* ep;
        public:
            Buffer(LPCVOID buffer, size_t size)
                : sp(reinterpret_cast<const BYTE*>(buffer))
                , ep(reinterpret_cast<const BYTE*>(buffer) + size)
            {}

            const BYTE* Start() const { return sp; }
            const BYTE* End() const { return ep; }
            bool Empty() const { return sp == ep; }
            const BYTE* Pointer() const { return sp; }
            size_t Size() const { return std::distance(sp, ep); }
            Buffer Consume(size_t size)
            {
                if (size > Size()) {
                    throw std::length_error("size is too large");
                }
                auto preSp = sp;
                sp += size;
                return Buffer(preSp, size);
            }
        };

        class Idle;
        class Continuation;
        class Insufficient;

        /// <summary>
        /// 受信ステート基底クラス
        /// </summary>
        class StateBase
        {
        protected:
            Receiver* owner;
            StateBase() : owner(nullptr) {}
            StateBase(Receiver* owner) : owner(owner) {}

        protected:
            inline std::vector<BYTE>& Pool() { return owner->pool; }

            inline Idle& Idle() { return owner->idle; }
            inline Continuation& Continuation() { return owner->continuation; }
            inline Insufficient& Insufficient() { return owner->insufficient; }

        public:
            virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) = 0;
            virtual ~StateBase() {}
        };

        /// <summary>
        /// 連続した受信をまたがない状態
        /// </summary>
        class Idle final : public StateBase
        {
        public:
            Idle(Receiver* owner) : StateBase(owner) {}
            virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) override
            {
                if (buffer.Size() < sizeof(Packet::size)) {
                    //ヘッダー部を完全に受信できていない
                    // Insufficientステートをセットアップして次回以降に続きを受信
                    Insufficient().Continue(buffer.Consume(buffer.Size()));
                    return {&Insufficient(), Buffer(buffer.End(),0)};
                }
                const Packet* packet = reinterpret_cast<const Packet*>(buffer.Pointer());
                if (packet->size > buffer.Size()) {
                    //パケットサイズが受信バッファー残サイズより大きい場合
                    // Continuationをセットアップして続きは次回以降に取得
                    Continuation().Continue(buffer.Consume(buffer.Size()), packet->size - buffer.Size());
                    return {&Continuation(), Buffer(buffer.End(),0)};
                }
                //1パケット受信
                return { this, buffer.Consume(packet->size) };
            }
        };

        /// <summary>
        /// 連続した受信をまたぐ状態
        /// </summary>
        class Continuation final : public StateBase
        {
        private:
            size_t remain;
        public:
            Continuation(Receiver* owner)
                : StateBase(owner), remain(0)
            {
            }

            /// <summary>
            /// 受信済みデータをプール領域にセットアップ
            /// </summary>
            /// <param name="buffer">プール領域へ保存するバッファー</param>
            /// <param name="totalRemain">未受信データサイズ</param>
            void Continue(Buffer buffer, size_t totalRemain)
            {
                Pool().clear();
                Pool().insert(Pool().end(), buffer.Start(), buffer.End());
                this->remain = totalRemain;
            }

            /// <summary>
            /// 未受信データサイズをセットアップ。プール領域にはすでに受信済みデータが存在することが前提。
            /// </summary>
            /// <param name="totalRemain">未受信データサイズ</param>
            void Continue(size_t totalRemain)
            {
                this->remain = totalRemain;
            }

            virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) override
            {
                auto appendSize = (std::min)(buffer.Size(), remain);
                auto appendBuf = buffer.Consume(appendSize);
                Pool().insert(Pool().end(), appendBuf.Start(), appendBuf.End());
                remain -= appendSize;
                if (0 == remain) {
                    //分割されたパケットを結合したものを戻り値とする
                    return { &Idle(), Buffer(&Pool()[0], Pool().size())};
                }
                //まだ必要サイズに満たないので受信処理を継続
                return { this, Buffer(buffer.End(),0) };
            }
        };

        /// <summary>
        /// ヘッダー領域途中で分割されている場合
        /// </summary>
        class Insufficient final : public StateBase
        {
        public:
            Insufficient(Receiver* owner)
                : StateBase(owner)
            {
            }

            /// <summary>
            /// 受信済みデータをプール領域にセットアップ
            /// </summary>
            /// <param name="buffer">プール領域へ保存するバッファー</param>
            void Continue(Buffer buffer)
            {
                Pool().clear();
                Pool().insert(Pool().end(), buffer.Start(), buffer.End());
            }

            virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) override
            {
                auto prevSize = static_cast<DWORD>(Pool().size());
                Pool().insert(Pool().end(), buffer.Start(), buffer.End());
                if (Pool().size() < sizeof(Packet::size)) {
                    //ヘッダー領域が受信できていない
                    auto consumed = buffer.Consume(buffer.Size());
                    return { this, Buffer(buffer.End(),0) };
                }
                const Packet* packet = reinterpret_cast<const Packet*>(&Pool()[0]);
                const DWORD remain = packet->size - prevSize;
                if (remain > buffer.Size() ) {
                    //パケットサイズが受信バッファー残サイズより大きい場合
                    Continuation().Continue(remain - buffer.Size());
                    buffer.Consume(buffer.Size());
                    return { &Continuation(), Buffer(buffer.End(),0) };
                }
                //完全なパケットが取得できた
                buffer.Consume(remain);
                return { &Idle(),  Buffer(&Pool()[0], packet->size)};
            }

        };

        //受信バッファーをまたいだ場合の一時保存領域
        std::vector<BYTE> pool;

        ReceivedCallback callback;

        Idle idle;
        Continuation continuation;
        Insufficient insufficient;
        StateBase* state;

    public:
        Receiver() = delete;
        Receiver(Receiver&&) = delete;
        Receiver(const Receiver&) = delete;

        Receiver& operator=(Receiver&&) = delete;
        Receiver& operator=(const Receiver&) = delete;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="reserveSize">受信バッファー初期リザーブサイズ</param>
        /// <param name="callback">受信コールバック</param>
        Receiver(size_t reserveSize, ReceivedCallback callback)
            : callback(callback)
            , idle(this)
            , continuation(this)
            , insufficient(this)
            , state(&idle)
        {
            pool.reserve(reserveSize);
        }

        /// <summary>
        /// 受信データ処理
        /// </summary>
        /// <param name="p">受信バッファー</param>
        /// <param name="size">サイズ</param>
        void Feed(LPCVOID p, size_t size)
        {
            auto buffer = Buffer(p, size);
            //バッファー内をすべて処理するまで繰り返し
            while (!buffer.Empty()) {
                std::tuple<StateBase*, Buffer> res = state->Feed(buffer);
                state = std::get<0>(res);
                if (!std::get<1>(res).Empty()) {
                    const Packet* packet = reinterpret_cast<const Packet*>(std::get<1>(res).Pointer());
                    callback(packet->data, packet->size - sizeof(packet->size));
                }
            }
        }

        /// <summary>
        /// 初期状態にリセット
        /// </summary>
        void Reset()
        {
            state = &idle;
        }
    };
#pragma endregion

    //推奨バッファーサイズ
    constexpr DWORD TYPICAL_BUFFER_SIZE = 64 * 1024;

    //最少バッファーサイズ
    constexpr DWORD MIN_BUFFER_SIZE = 40;


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
    private:
        //パイプハンドル
        winrt::file_handle handlePipe;
        //受信用オーバーラップ構造体
        OVERLAPPED readOverlap ;
        //受信バッファー
        std::unique_ptr<BYTE[]> readBuffer;
        //受信イベント
        winrt::handle readEvent;
        //受信パケット処理
        std::unique_ptr<Receiver> receiver;
        //送信バッファー利用可イベント
        std::unique_ptr<concurrency::event> writableEvent;
        //クローズ中フラグ
        LONG closing;

        bool TestClosing()
        {
            return 1 == InterlockedCompareExchange(&closing, 1, 1);
        }

        bool SetClosing()
        {
            return 1 == InterlockedCompareExchange(&closing, 1, 0);
        }

        struct Defer {
            std::function<void(void)> func;
            Defer(std::function<void(void)> func) : func(func) {}
            ~Defer() { if (func) func(); }
            void Detach() { func = std::function<void(void)>(); }
        };

        struct WriteOverlapTag {
            SimpleNamedPipeBase* owner;
            LPCVOID buffer;
            DWORD remain;
            DWORD lastErr;
            bool success;
            bool completed;
        };

        static void WriteOverlapComplete(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
        {
            WriteOverlapTag* tag = reinterpret_cast<WriteOverlapTag*>(lpOverlapped->hEvent);
            tag->lastErr = dwErrorCode;
            if (ERROR_SUCCESS != dwErrorCode) {
                tag->success = false;
                return;
            }
            DWORD remain = tag->remain - dwNumberOfBytesTransfered;
            if (0 > remain) {
                tag->success = false;
                return;
            }
            tag->remain = remain;
            tag->completed = tag->remain == 0;
            tag->buffer = reinterpret_cast<const BYTE*>(tag->buffer) + dwNumberOfBytesTransfered;
            tag->success = true;
            return;
        }

        void WriteRaw(LPCVOID buffer, DWORD size, concurrency::cancellation_token ct)
        {
            winrt::handle cancelEvent{CreateEvent(nullptr, true, false, nullptr)};
            concurrency::cancellation_token_registration cookie;
            if (ct.is_cancelable()) {
                //キャンセル可能な場合のイベントディスパッチ処理
                cookie = ct.register_callback([&cancelEvent, ct, &cookie]() {
                    winrt::check_bool(SetEvent(cancelEvent.get()));
                    try {
                        ct.deregister_callback(cookie);
                    }
                    catch (...) {}
                });
            }
            //オーバーラップ構造体の設定
            WriteOverlapTag tag{ this, buffer, size, ERROR_SUCCESS, true, false };
            while (!tag.completed && tag.remain > 0) {
                OVERLAPPED overlapped = { 0 };
                overlapped.hEvent = reinterpret_cast<HANDLE>(&tag);
                winrt::check_bool(WriteFileEx(handlePipe.get(), tag.buffer, tag.remain, &overlapped, &SimpleNamedPipeBase::WriteOverlapComplete));
                auto res = WaitForSingleObjectEx(cancelEvent.get(), INFINITE, true);
                if (WAIT_OBJECT_0 == res) {
                    //非同期送信をキャンセル
                    CancelIoEx(handlePipe.get(), &overlapped);
                    concurrency::cancel_current_task();
                    break;
                }
                else if (WAIT_IO_COMPLETION == res) {
                    if (!tag.success) {
                        if (ERROR_SUCCESS != tag.lastErr) {
                            winrt::throw_hresult(HRESULT_FROM_WIN32(tag.lastErr));
                        }
                        throw std::logic_error("WriteRaw logic error");
                    }
                }
                else {
                    winrt::throw_last_error();
                }
            }
            if (ct.is_cancelable()) {
                ct.deregister_callback(cookie);
            }
        }

    public:
        //送信・受信バッファーサイズ
        inline static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
        //1回の送信・受信の最大サイズ
        inline static constexpr size_t MAX_DATA_SIZE = (std::numeric_limits<DWORD>::max)() - static_cast<DWORD>(sizeof DWORD);

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
            , readOverlap({ 0 })
            , readBuffer(std::make_unique<BYTE[]>(BUF_SIZE))
            , receiver(std::make_unique<Receiver>(BUF_SIZE, receivedCallback))
            , writableEvent(std::make_unique<concurrency::event>())
            , closing(0)
        {
            if constexpr(BUF_SIZE < MIN_BUFFER_SIZE) {
                throw std::invalid_argument("BUF_SIZE is too short");
            }
            readEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(static_cast<bool>(readEvent));
            readOverlap.hEvent = readEvent.get();
            writableEvent->set();
        }

        virtual ~SimpleNamedPipeBase() {}

        const HANDLE Handle() { return handlePipe.get(); }
        const bool Valid() { return static_cast<bool>(handlePipe); }
        const std::unique_ptr<BYTE[]>& ReadBuffer() { return readBuffer; }
        const winrt::handle& ReadEvent() { return readEvent; }
        void Reset() { receiver->Reset(); }

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
                readOverlap.Offset = 0;
                readOverlap.OffsetHigh = 0;
            }
            //同期的に受信データを取得できないかエラーの場合
            auto lastErr = GetLastError();
            if (ERROR_PIPE_NOT_CONNECTED == lastErr
                    || ERROR_BROKEN_PIPE == lastErr
                    || ERROR_PIPE_LISTENING == lastErr) {
                //切断状態
                return false;
            }
            if (ERROR_IO_PENDING != lastErr) {
                //I/O完了待ち以外ではエラー
                if (!TestClosing()) {
                    winrt::throw_last_error();
                }
                //クローズ中は例外を送出せずに切断状態を返す
                return false;
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
                if (ERROR_PIPE_NOT_CONNECTED == lastErr
                        || ERROR_BROKEN_PIPE == lastErr
                        || ERROR_PIPE_LISTENING == lastErr) {
                    //切断状態
                    return false;
                }
                //クローズ中のエラーは無視
                if (!TestClosing()) {
                    winrt::throw_last_error();
                }
                return false;
            }
            //データ受信
            receiver->Feed(readBuffer.get(), readSize);
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
        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct = concurrency::cancellation_token::none())
        {
            if (!static_cast<bool>(handlePipe)) {
                //handleが無効
                throw std::runtime_error("this instance is invalid");
            }

            if (size > MAX_DATA_SIZE ) {
                throw std::length_error("size is too long");
            }
            auto writeSize = static_cast<DWORD>(size);

            //非同期時の送信完了待機のタスク
            return concurrency::create_task([this, buffer, writeSize, ct]() {
                //同期的に書き込み完了した際にイベントセットする
                Defer defer([this] {if (this->writableEvent) this->writableEvent->set(); });

                auto packetSize = writeSize + static_cast<DWORD>(sizeof(DWORD));

                //データサイズを送信
                WriteRaw(&packetSize, sizeof(packetSize), ct);
                //データ保体を送信
                WriteRaw(buffer, writeSize, ct);

            }, concurrency::task_options(ct));
        }

        void Close()
        {
            if (SetClosing()) {
                //すでにclose実行済み
                return;
            }
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
        using Base = SimpleNamedPipeBase<BUF_SIZE>;
        inline static constexpr DWORD BUFFER_SIZE = Base::BUFFER_SIZE;
        inline static constexpr size_t MAX_DATA_SIZE = Base::MAX_DATA_SIZE;
        using Callback = std::function<void(SimpleNamedPipeServer<BUF_SIZE>&, const PipeEventParam&)>;
    private:
        Base base;

        const std::wstring pipeName;
        Callback callback;
        OVERLAPPED connectionOverlap;
        winrt::handle connectionEvent;
        winrt::handle stopEvent;
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
                    const HANDLE handles[]{ stopEvent.get(), connectionEvent.get(), base.ReadEvent().get() };
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
                        winrt::check_bool(ResetEvent(handles[index]));
                        if (stopEvent.get() == handles[index]) {
                            //停止イベント
                            break;
                        } else if (connectionEvent.get() == handles[index]) {
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
                            try {
                                //終了した接続の後始末をして次回接続の待機
                                Disconnect();
                                //次回接続待ち開始
                                BeginConnect();
                            }
                            catch (winrt::hresult_error& ex) {
                                if (ex.code() == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)) {
                                    //閉じる段階でハンドルが破棄された場合
                                    break;
                                }
                                throw;
                            }
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
                if (ERROR_PIPE_LISTENING != lastErr
                        && ERROR_IO_PENDING != lastErr
                        && ERROR_PIPE_CONNECTED != lastErr) {
                    //ペンディングではない場合の名前付きパイプ接続失敗
                    winrt::throw_last_error();
                }
            }
            base.Reset();
        }

        /// <summary>
        /// 接続時処理
        /// </summary>
        virtual void OnConnected()
        {
            callback(*this, PipeEventParam{ PipeEventType::CONNECTED, nullptr, 0 });
        }

    public:
        SimpleNamedPipeServer() = delete;
        SimpleNamedPipeServer(SimpleNamedPipeServer&&) = delete;
        SimpleNamedPipeServer(const SimpleNamedPipeServer&) = delete;

        SimpleNamedPipeServer& operator=(SimpleNamedPipeServer&&) = delete;
        SimpleNamedPipeServer& operator=(const SimpleNamedPipeServer&) = delete;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="psa">セキュリティディスクリプタ</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeServer(LPCWSTR name, LPSECURITY_ATTRIBUTES psa, Callback callback)
            : pipeName(name)
            , callback(callback)
            , connectionOverlap({ 0 })
        {
            //名前付きパイプの作成
            // 接続可能クライアント数=1; ローカルマシン接続のみ許可
            winrt::file_handle handlePipe{
                CreateNamedPipeW(
                    name,
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
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

            //停止イベント作成
            stopEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(static_cast<bool>(stopEvent));

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

        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct = concurrency::cancellation_token::none())
        {
            return base.WriteAsync(buffer, size, ct);
        }

        std::wstring PipeName() const { return pipeName; }

        bool Valid()
        {
            return base.Valid();
        }

        void Close()
        {
            base.Close();
            winrt::check_bool(SetEvent(stopEvent.get()));
        }

        /// <summary>
        /// 接続中のClientを切断する
        /// </summary>
        virtual void Disconnect()
        {
            FlushFileBuffers(base.Handle());
            if (!DisconnectNamedPipe(base.Handle())) {
                auto lastErr = GetLastError();
                if (ERROR_PIPE_NOT_CONNECTED != lastErr) {
                    //閉じられた状態のエラーは無視
                    winrt::throw_last_error();
                }
            }
        }

        virtual ~SimpleNamedPipeServer()
        {
            Close();
            try { watcherTask.wait(); }
            catch (...) {};
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
        using Base = SimpleNamedPipeBase<BUF_SIZE>;
        inline static constexpr DWORD BUFFER_SIZE = Base::BUFFER_SIZE;
        inline static constexpr size_t MAX_DATA_SIZE = Base::MAX_DATA_SIZE;
        using Callback = std::function<void(SimpleNamedPipeClient<BUF_SIZE>&, const PipeEventParam&)>;
    private:
        Base base;

        const std::wstring pipeName;
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
                        winrt::check_bool(ResetEvent(handles[index]));
                        if (base.ReadEvent().get() == handles[index]) {
                            //読み込みイベント
                            if (!base.OnSignalRead()) {
                                //切断状態の場合
                                callback(*this, PipeEventParam{ PipeEventType::DISCONNECTED, nullptr, 0 });
                                break;
                            }
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

    public:
        SimpleNamedPipeClient() = delete;
        SimpleNamedPipeClient(const SimpleNamedPipeClient&) = delete;
        SimpleNamedPipeClient& operator=(const SimpleNamedPipeClient&) = delete;

        SimpleNamedPipeClient(SimpleNamedPipeClient&&) = delete;
        SimpleNamedPipeClient& operator=(SimpleNamedPipeClient&&) = delete;

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeClient(LPCWSTR name, Callback callback)
            : pipeName(name)
            , callback(callback)
        {
            winrt::check_bool(WaitNamedPipe(name, NMPWAIT_USE_DEFAULT_WAIT));
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

            //非同期受信処理開始
            base.OverappedRead();

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

        concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct = concurrency::cancellation_token::none())
        {
            return base.WriteAsync(buffer, size, ct);
        }

        std::wstring PipeName() const { return pipeName; }

        bool Valid()
        {
            return base.Valid();
        }

        void Close()
        {
            FlushFileBuffers(base.Handle());
            base.Close();
        }

        virtual ~SimpleNamedPipeClient()
        {
            //監視タスク終了待ち（例外は無視）
            Close();
            try { watcherTask.wait();}
            catch (...) {};
        }
    };

    using TypicalSimpleNamedPipeClient = SimpleNamedPipeClient<TYPICAL_BUFFER_SIZE>;
}
