﻿#pragma once

#include <windows.h>
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
            inline size_t LimitSize() const { return owner->limitSize; }
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
                if (packet->size > LimitSize()) {
                    throw std::length_error("packet size is too long");
                }
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
                if (packet->size > LimitSize()) {
                    throw std::length_error("packet size is too long");
                }
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

        //パケットサイズ上限
        const size_t limitSize;

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
        /// <param name="limitSize">受信サイズ上限</param>
        /// <param name="callback">受信コールバック</param>
        Receiver(size_t limitSize, ReceivedCallback callback)
            : limitSize(limitSize)
            , callback(callback)
            , idle(this)
            , continuation(this)
            , insufficient(this)
            , state(&idle)
        {
            pool.reserve(limitSize);
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

    //バッファーサイズ
    constexpr DWORD TYPICAL_BUFFER_SIZE = 64 * 1024;

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
        //送信バッファー
        std::unique_ptr<BYTE[]> writeBuffer;
        //送信バッファー利用可イベント
        std::unique_ptr<concurrency::event> writableEvent;
        //クローズ中フラグ
        bool closing;

        struct Defer {
            std::function<void(void)> func;
            Defer(std::function<void(void)> func) : func(func) {}
            ~Defer() { if (func) func(); }
            void Detach() { func = std::function<void(void)>(); }
        };

    public:
        //送信・受信バッファーサイズ
        inline static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
        //1回の送信・受信の最大サイズ
        inline static constexpr DWORD MAX_DATA_SIZE = BUFFER_SIZE - static_cast<DWORD>(sizeof DWORD);

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
            , writeBuffer(std::make_unique<BYTE[]>(BUF_SIZE))
            , writableEvent(std::make_unique<concurrency::event>())
            , closing(false)
        {
            if constexpr(BUF_SIZE <= sizeof(Packet::size)) {
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
                if (!closing) {
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
                if (!closing) {
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
        /// 書き込み完了待ち
        /// </summary>
        /// <param name="aquireLock">true:完了時点で書き込みロックを取得</param>
        /// <param name="ct">キャンセルトークン</param>
        /// <param name="timeout">タイムアウト(defalut:タイムアウトしない)</param>
        /// <returns>非同期オブジェクト</returns>
        concurrency::task<void> WaitWriteComplete(bool aquireLock, concurrency::cancellation_token ct, unsigned int timeout = concurrency::COOPERATIVE_TIMEOUT_INFINITE)
        {
            return concurrency::create_task([this, aquireLock, timeout, ct] {
                std::vector<concurrency::event*> events;
                concurrency::event cancelEvent;
                if (ct.is_cancelable()) {
                    concurrency::cancellation_token_registration cookie;
                    cookie = ct.register_callback([&cancelEvent, ct, &cookie]() {
                        cancelEvent.set();
                        ct.deregister_callback(cookie);
                    });
                    events.emplace_back(&cancelEvent);
                }
                events.emplace_back(writableEvent.get());
                auto res = concurrency::event::wait_for_multiple(&events[0], events.size(), false, timeout);
                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == res) {
                    //タイムアウト
                    throw concurrency::task_canceled("timeout");
                }
                if (0 <= res && res < events.size()) {
                    if (events[res] == &cancelEvent) {
                        concurrency::cancel_current_task();
                    }
                    if (events[res] == writableEvent.get()) {
                        if (aquireLock) {
                            writableEvent->reset();
                        }
                    }
                }
            }, concurrency::task_options(ct));
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

            if (ct.is_canceled()) {
                throw concurrency::task_canceled();
            }

            //書き込み中ならば書き込み完了まで待って書き込みロックを取得
            WaitWriteComplete(true, ct).wait();
            //同期的に書き込み完了した際にイベントセットする
            Defer defer([this] {if(this->writableEvent) this->writableEvent->set(); });

            //送信完了イベント
            winrt::handle writeEvent{CreateEventW(nullptr, true, false, nullptr)};
            winrt::check_bool(static_cast<bool>(writeEvent));

            //送信バッファーへコピー
            auto packet = reinterpret_cast<Packet*>(writeBuffer.get());
            packet->size = static_cast<DWORD>(size + sizeof(Packet::size));
            memcpy(packet->data, buffer, size);

            //オーバーラップ構造体の設定
            OVERLAPPED overlapped = { 0 };
            overlapped.hEvent = writeEvent.get();
            //非同期送信
            if (WriteFile(handlePipe.get(), writeBuffer.get(), static_cast<DWORD>(packet->size), nullptr, &overlapped)) {
                //同期で処理完了
                return concurrency::task_from_result();
            }
            auto lastErr = GetLastError();
            if (ERROR_IO_PENDING != lastErr) {
                //完了待ち以外ではエラー
                winrt::throw_last_error();
            }

            //タスクへ引き継ぐために同期オブジェクトはデタッチして変更されないようにする
            writeEvent.detach();
            defer.Detach();

            //非同期時の送信完了待機のタスク
            return concurrency::create_task([this, overlapped, ct]() {
                //タスクから抜けた際に書き込み可能イベントをセット
                Defer defer([this] {if(this->writableEvent) this->writableEvent->set(); });
                std::vector<HANDLE> handles;
                //非同期書き込みイベントを終了時に開放するようにしておく
                winrt::handle writeEvent(overlapped.hEvent);
                winrt::handle cancelEvent;
                if (ct.is_cancelable()) {
                    //キャンセル可能な場合のイベントディスパッチ処理
                    cancelEvent.attach(CreateEventW(nullptr, true, false, nullptr));
                    winrt::check_bool(static_cast<bool>(cancelEvent));
                    concurrency::cancellation_token_registration cookie;
                    cookie = ct.register_callback([&cancelEvent, ct, &cookie]() {
                        winrt::check_bool(SetEvent(cancelEvent.get()));
                        ct.deregister_callback(cookie);
                        });
                    handles.emplace_back(cancelEvent.get());
                }
                handles.emplace_back(writeEvent.get());

                //書き込み完了、もしくは中断まで待機
                auto res = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), &handles[0], false, INFINITE);
                auto index = res - WAIT_OBJECT_0;
                if (0 <= index && index < handles.size()) {
                    //書き込み完了、もしくはキャンセル
                    winrt::check_bool(ResetEvent(handles[index]));
                    if (handles[index] == cancelEvent.get()) {
                        //タスクキャンセル処理
                        concurrency::cancel_current_task();
                    }
                }
            }, concurrency::task_options(ct));
        }

        void Close()
        {
            closing = true;
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
        inline static constexpr DWORD MAX_DATA_SIZE = Base::MAX_DATA_SIZE;
        using Callback = std::function<void(SimpleNamedPipeServer<BUF_SIZE>&, const PipeEventParam&)>;
    private:
        Base base;

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
                            //終了した接続の後始末をして次回接続の待機
                            Disconnect();
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
            : callback(callback)
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

        bool Valid()
        {
            return base.Valid();
        }

        void Close()
        {
            winrt::check_bool(SetEvent(stopEvent.get()));
            base.Close();
        }

        /// <summary>
        /// 接続中のClientを切断する
        /// </summary>
        virtual void Disconnect()
        {
            FlushFileBuffers(base.Handle());
            winrt::check_bool(DisconnectNamedPipe(base.Handle()));
            //次回接続待ち開始
            BeginConnect();
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
        inline static constexpr DWORD MAX_DATA_SIZE = Base::MAX_DATA_SIZE;
        using Callback = std::function<void(SimpleNamedPipeClient<BUF_SIZE>&, const PipeEventParam&)>;
    private:
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
            : callback(callback)
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
