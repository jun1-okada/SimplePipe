#pragma once
#include <windows.h>
#include <limits>
#include <memory>
#include <functional>
#include <tuple>
#include <optional>
#include <winrt/base.h>
#include <atomic>
#include <ppl.h>
#include <ppltasks.h>

namespace abt::comm::simple_pipe
{
    //推奨バッファーサイズ
    constexpr DWORD TYPICAL_BUFFER_SIZE = 64 * 1024;
    //最少バッファーサイズ
    constexpr DWORD MIN_BUFFER_SIZE = 40;
    static_assert(TYPICAL_BUFFER_SIZE >= MIN_BUFFER_SIZE, "TYPICAL_BUFFER_SIZE must be greater than or equal to MIN_BUFFER_SIZE");

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
    class SimpleNamedPipeBase
    {
    public:
        SimpleNamedPipeBase() = delete;
        SimpleNamedPipeBase(SimpleNamedPipeBase&&) = delete;
        SimpleNamedPipeBase(const SimpleNamedPipeBase&) = delete;

        SimpleNamedPipeBase& operator=(SimpleNamedPipeBase&&) = delete;
        SimpleNamedPipeBase& operator=(const SimpleNamedPipeBase&) = delete;
        virtual ~SimpleNamedPipeBase()
        {
            //監視タスク終了待ち（例外は無視）
            Close();
            try { watcherTask.wait(); }
            catch (...) {};
        }

#pragma region Receiver
        /// <summary>
        /// 受信バッファー管理クラス
        /// </summary>
        class Buffer final
        {
        private:
            const BYTE* sp;
            const BYTE* ep;
        public:
            explicit Buffer(LPCVOID buffer, size_t size)
                : sp{ reinterpret_cast<const BYTE*>(buffer) }
                , ep{ reinterpret_cast<const BYTE*>(buffer) + size }
            {}
            const BYTE* Begin() const { assert(sp != nullptr); return sp; }
            const BYTE* End() const { assert(ep != nullptr); return ep; }
            bool Empty() const { return sp == ep; }
            const BYTE* Pointer() const { assert(sp != nullptr); return sp; }
            size_t Size() const { assert(sp != nullptr); return std::distance(sp, ep); }
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

        /// <summary>
        /// データパケット
        /// </summary>
        struct alignas(4) Header {
            DWORD size;
            union {
                DWORD reserve;
                struct {
                    WORD dataOffset;    //パケットの先頭からデータまでのオフセット
                    WORD startBit : 1;
                    WORD endBit : 1;
                    WORD cancelBit : 1;
                    WORD reserve : 13;
                } info;
            };
            inline size_t DataOffset() const { return info.dataOffset; }
            inline size_t DataSize() const
            {
                assert(size >= info.dataOffset);
                return size - info.dataOffset;
            }
            inline bool IsStart() const { return info.startBit != 0; }
            inline bool IsEnd() const { return info.endBit != 0; }
            inline bool IsCancel() const { return info.cancelBit != 0; }
            static inline Header Create(DWORD dataSize, bool startBit, bool endBit)
            {
                Header header{ 0 };
                header.size = static_cast<DWORD>(dataSize + HeaderSize);
                header.info.dataOffset = HeaderSize;
                header.info.startBit = startBit ? 1 : 0;
                header.info.endBit = endBit ? 1 : 0;
                return header;
            }
            static inline Header CreateCancel()
            {
                Header header{ 0 };
                header.size = HeaderSize;
                header.info.dataOffset = HeaderSize;
                header.info.cancelBit = 1;
                return header;
            }
        };
        inline static constexpr size_t HeaderSize = sizeof(Header);
        static_assert((std::numeric_limits<WORD>::max)() >= HeaderSize);

        struct Packet
        {
            //size分も含めた全体のサイズ
            Header head;
            BYTE data[1];
            Buffer Data() const
            {
                return Buffer(reinterpret_cast<const BYTE*>(this) + head.DataOffset(), head.DataSize());
            }
        };

        using ReceivedCallback = std::function<void(const Packet*)> ;

        /// <summary>
        /// 受信データ復号クラス
        /// </summary>
        class Receiver final
        {
        private:
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
                StateBase(Receiver* owner) : owner{ owner } {}

            protected:
                inline DWORD Limit() const { return owner->limitSize; }
                inline std::vector<BYTE>& Pool() { return owner->pool; }
                inline Idle& Idle() { return owner->idle; }
                inline Continuation& Continuation() { return owner->continuation; }
                inline Insufficient& Insufficient() { return owner->insufficient; }
                inline void TrhowIfBadHeader(const Header *head) const
                {
                    if (head->size < HeaderSize || head->info.dataOffset < HeaderSize) {
                        throw std::length_error("bad packet header");
                    }
                    if ((head->size - HeaderSize) > Limit()) {
                        throw std::length_error("too long packet size");
                    }
                }

            public:
                /// <summary>
                /// 受信データ処理
                /// </summary>
                /// <param name="buffer">入力受信データ</param>
                /// <returns>{次のステート, 出力パケット(Empty()==true時は取得パケットなし) }</returns>
                virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) = 0;
                virtual ~StateBase() {}
            };

            /// <summary>
            /// パケットが受信データをまたがない状態
            /// </summary>
            class Idle final : public StateBase
            {
            public:
                Idle(Receiver* owner) : StateBase(owner) {}
                virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) override
                {
                    if (buffer.Size() < HeaderSize) {
                        //ヘッダー部を完全に受信できていない
                        // Insufficientステートをセットアップして次回以降に続きを受信
                        Insufficient().Continue(buffer.Consume(buffer.Size()));
                        return { &Insufficient(), Buffer(buffer.End(),0) };
                    }
                    const Packet* packet = reinterpret_cast<const Packet*>(buffer.Pointer());
                    TrhowIfBadHeader(&packet->head);
                    if (packet->head.size > buffer.Size()) {
                        //パケットサイズが受信バッファー残サイズより大きい場合
                        // Continuationをセットアップして続きは次回以降に取得
                        Continuation().Continue(buffer.Consume(buffer.Size()), packet->head.size - buffer.Size());
                        return { &Continuation(), Buffer(buffer.End(),0) };
                    }
                    //1パケット受信。パケットサイズ分を受信データから切り出し。
                    return { this, buffer.Consume(packet->head.size) };
                }
            };

            /// <summary>
            /// 連続した受信をまたぐ状態
            /// </summary>
            class Continuation final : public StateBase
            {
            private:
                size_t remain{ 0 };
            public:
                Continuation(Receiver* owner) : StateBase(owner) {}

                /// <summary>
                /// 受信済みデータをプール領域にセットアップ
                /// </summary>
                /// <param name="buffer">プール領域へ保存するバッファー</param>
                /// <param name="totalRemain">未受信データサイズ</param>
                void Continue(Buffer buffer, size_t totalRemain)
                {
                    Pool().clear();
                    Pool().insert(Pool().end(), buffer.Begin(), buffer.End());
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
                    Pool().insert(Pool().end(), appendBuf.Begin(), appendBuf.End());
                    remain -= appendSize;
                    if (0 == remain) {
                        //分割されたパケットを結合したものを戻り値とする
                        return { &Idle(), Buffer(&Pool()[0], Pool().size()) };
                    }
                    //まだ必要サイズに満たないので受信処理を継続。
                    return { this, Buffer(buffer.End(),0) };
                }
            };

            /// <summary>
            /// ヘッダー領域途中で分割されている場合
            /// </summary>
            class Insufficient final : public StateBase
            {
            public:
                Insufficient(Receiver* owner) : StateBase(owner) {}

                /// <summary>
                /// 受信済みデータをプール領域にセットアップ
                /// </summary>
                /// <param name="buffer">プール領域へ保存するバッファー</param>
                void Continue(Buffer buffer)
                {
                    Pool().clear();
                    Pool().insert(Pool().end(), buffer.Begin(), buffer.End());
                }

                virtual std::tuple<StateBase*, Buffer> Feed(Buffer& buffer) override
                {
                    auto prevSize = static_cast<DWORD>(Pool().size());
                    //パケットサイズが分からないので、受信データ全てをプール領域へコピーする
                    Pool().insert(Pool().end(), buffer.Begin(), buffer.End());
                    if (Pool().size() < HeaderSize) {
                        //ヘッダー領域が受信できていない
                        auto consumed = buffer.Consume(buffer.Size());
                        return { this, Buffer(buffer.End(),0) };
                    }
                    const Packet* packet = reinterpret_cast<const Packet*>(&Pool()[0]);
                    TrhowIfBadHeader(&packet->head);
                    const DWORD remain = packet->head.size - prevSize;
                    if (remain > buffer.Size()) {
                        //パケットサイズが受信バッファー残サイズより大きい場合
                        Continuation().Continue(remain - buffer.Size());
                        buffer.Consume(buffer.Size());
                        //足らないパケットデータは次回以降で受信する
                        return { &Continuation(), Buffer(buffer.End(),0) };
                    }
                    //完全なパケットが取得できた
                    buffer.Consume(remain);
                    return { &Idle(),  Buffer(&Pool()[0], packet->head.size) };
                }

            };

            //受信バッファーをまたいだ場合の一時保存領域
            std::vector<BYTE> pool;

            //受信コールバック
            ReceivedCallback callback;

            Idle idle;
            Continuation continuation;
            Insufficient insufficient;
            StateBase* state;

            const DWORD limitSize;

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
            Receiver(size_t reserveSize, DWORD limitSize, ReceivedCallback callback)
                : limitSize(limitSize)
                , callback(callback)
                , idle(this)
                , continuation(this)
                , insufficient(this)
                , state(&idle)
            {
                if (!callback) {
                    throw std::invalid_argument("bad callback error");
                }
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
                        //受信パケットデータが存在したら受信コールバクを実行
                        const BYTE* ptr = std::get<1>(res).Pointer();
                        const Packet* packet = reinterpret_cast<const Packet*>(ptr);
                        callback(packet);
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

        /// <summary>
        /// データを複数パケットに変換
        /// </summary>
        class Serializer final
        {
        private:
            Buffer buffer;
            const DWORD splitSize;
            bool beginning{ true };
        public:
            Serializer() = delete;
            Serializer(Serializer&&) = delete;
            Serializer(const Serializer&) = delete;
            Serializer& operator=(Serializer&&) = delete;
            Serializer& operator=(const Serializer&) = delete;
            explicit Serializer(Buffer buffer, DWORD splitSize)
                : buffer{ buffer }, splitSize{ splitSize } {};

            std::tuple<Buffer, Header> Next()
            {
                if (buffer.Empty()) {
                    return { buffer, {0} };
                }
                auto size = (std::min)(static_cast<size_t>(splitSize), buffer.Size());
                auto fragment = buffer.Consume(size);
                auto header = Header::Create(static_cast<DWORD>(size), beginning, buffer.Empty());
                beginning = buffer.Empty();
                return { fragment, header };
            }
        };

        /// <summary>
        /// 複数パケットからデータに変換
        /// </summary>
        class Deserializer final
        {
        private:
            bool beginning{ true };
            std::vector<BYTE> pool;
            const size_t limitSize;
            std::function<void(Buffer)> completed;
        public:
            Deserializer() = delete;
            Deserializer(Deserializer&&) = delete;
            Deserializer(const Deserializer&) = delete;
            Deserializer& operator=(Serializer&&) = delete;
            Deserializer& operator=(const Deserializer&) = delete;
            Deserializer(size_t reserveSize, size_t limitSize, std::function<void(Buffer)> completed)
                : limitSize(limitSize)
                , completed(completed)
            {
                if (!completed) {
                    throw std::invalid_argument("bad callback error");
                }
                pool.reserve(reserveSize);
            }

            void Reset()
            {
                beginning = true;
            }

            bool Feed(const Packet* packet)
            {
                if (packet->head.IsCancel()) {
                    beginning = true;
                    pool.clear();
                    return false;
                }
                if (beginning) {
                    pool.clear();
                    //最初のパケット
                    if (!packet->head.IsStart()) {
                        //データに矛盾
                        throw std::runtime_error("inconsistent feed data");
                    }
                    beginning = false;
                }
                auto packetData = packet->Data();
                if(limitSize < pool.size() + packetData.Size()){
                    throw std::length_error("size is too long");
                }
                pool.insert(pool.end(), packetData.Begin(), packetData.End());
                if (packet->head.IsEnd()) {
                    beginning = true;
                    completed(Buffer(&pool[0], pool.size()));
                }
                return true;
            }
        };

#pragma endregion
    private:
        //パイプハンドル
        winrt::file_handle handlePipe;
        //受信用オーバーラップ構造体
        OVERLAPPED readOverlap = { 0 };
        //受信バッファー
        std::unique_ptr<BYTE[]> readBuffer;
        //Closeイベント
        winrt::handle closeEvent;
        //受信イベント
        winrt::handle readEvent;
        //カスタムイベント
        std::vector<winrt::handle> customEvents;
        //監視タスク
        concurrency::task<void> watcherTask;
        //受信パケット処理
        Receiver receiver;
        //デシリアライズ処理
        Deserializer deserializer;
        //送信バッファーロック
        concurrency::critical_section writeCs;
        //クローズ中フラグ
        std::atomic_bool closing{false};
        //送受信バッファーサイズ
        const DWORD bufferSize;
        //送受信上限サイズ
        const DWORD limitSize;

        /// <summary>
        /// RAIIヘルパー
        /// </summary>
        struct Defer {
            std::function<void(void)> func;
            Defer(std::function<void(void)> func) : func(func) {}
            virtual ~Defer() { if (func) func(); }
        };

        /// <summary>
        /// 非同期Write用のワーク領域
        /// </summary>
        struct WriteOverlapTag {
            //呼び出し元のポインター
            SimpleNamedPipeBase* owner;
            //書き込みバッファー
            Buffer buffer;
            //Win32システムエラー
            DWORD lastErr;
            //成功フラグ
            bool success;
            //書き込み完了チェック
            bool Completed() const { return buffer.Empty(); }
        };

        // 非同期書き込み用コールバック関数
        // https://learn.microsoft.com/ja-jp/windows/win32/api/minwinbase/nc-minwinbase-lpoverlapped_completion_routine
        static void WriteOverlapComplete(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
        {
            WriteOverlapTag* tag = reinterpret_cast<WriteOverlapTag*>(lpOverlapped->hEvent);
            tag->lastErr = dwErrorCode;
            if (ERROR_SUCCESS != tag->lastErr) {
                //Win32エラー発生
                tag->success = false;
                return;
            }
            try {
                //書き込み済み領域を消費
                tag->buffer.Consume(dwNumberOfBytesTransfered);
            }
            catch (std::length_error&)
            {
                //書き込み済み領域と書き込んだバイト数に不整合あり
                tag->success = false;
                return;
            }
            //書き込み成功
            tag->success = true;
            return;
        }

        /// <summary>
        /// 非同期書き込みを同期的に実行
        /// </summary>
        /// <param name="buffer">書き込みバッファー</param>
        /// <param name="size">バッファーサイズ</param>
        /// <param name="ct">キャンセルトークン</param>
        /// <returns>キャンセル時はfalse</returns>
        bool WriteRaw(LPCVOID buffer, DWORD size, winrt::handle& cancelEvent)
        {
            //オーバーラップ構造体の設定
            WriteOverlapTag tag{ this, Buffer(buffer, size), ERROR_SUCCESS, true};
            OVERLAPPED overlapped = { 0 };
            while (!tag.Completed() && tag.success) {
                //一度に送信するサイズをコンストラクタ引数のbufferSizeまでに制限
                DWORD writeSize = (std::min)(static_cast<DWORD>(tag.buffer.Size()), bufferSize);
                //WriteFileExはhEventは利用しないので、ワーク領域のポインターを格納する
                // https://learn.microsoft.com/ja-jp/windows/win32/api/fileapi/nf-fileapi-writefileex
                overlapped.hEvent = reinterpret_cast<HANDLE>(&tag);
                winrt::check_bool(WriteFileEx(handlePipe.get(), tag.buffer.Pointer(), writeSize, &overlapped, &SimpleNamedPipeBase::WriteOverlapComplete));
                auto res = WaitForSingleObjectEx(cancelEvent.get(), INFINITE, true);
                if (WAIT_OBJECT_0 == res) {
                    //非同期書き込みをキャンセル
                    CancelIoEx(handlePipe.get(), &overlapped);
                }
                else if (WAIT_IO_COMPLETION == res) {
                    // I/O完了
                    if (!tag.success) {
                        if (ERROR_OPERATION_ABORTED == tag.lastErr) {
                            return false;
                        }
                        else if (ERROR_SUCCESS != tag.lastErr) {
                            //Win32エラーが発生していたら例外送出
                            winrt::throw_hresult(HRESULT_FROM_WIN32(tag.lastErr));
                        }
                        //不明なエラーの場合
                        throw std::logic_error("WriteRaw logic error");
                    }
                }
                else {
                    winrt::throw_last_error();
                }
            }
            return true;
        }

        /// <summary>
        /// イベント監視タスク
        /// </summary>
        /// <returns>非同期タスク</returns>
        concurrency::task<void> WatchAsync()
        {
            //タスクを実行
            return concurrency::create_task([this](){
                //既知のイベント登録 Close要求イベント, 非同期I/Oの読み込み完了イベント
                std::vector<HANDLE> handles {closeEvent.get(), readEvent.get()};
                //派生クラスで利用するイベントを追加
                std::vector<HANDLE> customEventHandels;
                std::transform(customEvents.begin(), customEvents.end(), std::back_inserter(customEventHandels), [](const auto& e) {return e.get(); });
                handles.insert(handles.end(), customEventHandels.begin(), customEventHandels.end());

                Defer defer([this]() {ClosePipeHandle(); });
                while (true) {
                    //接続、受信イベントを監視
                    auto res = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), &handles[0], false, INFINITE);
                    if (res == WAIT_FAILED) {
                        //エラー
                        winrt::throw_last_error();
                    }
                    if (!Valid()) {
                        //ハンドルが破棄されていたら終了
                        break;
                    }
                    auto index = res - WAIT_OBJECT_0;
                    if (0 <= index && index < handles.size()) {
                        auto signaled = handles[index];
                        if (closeEvent.get() == signaled) {
                            //Close要求イベント
                            break;
                        }
                        else if (readEvent.get() == signaled) {
                            //受信イベント
                            winrt::check_bool(ResetEvent(signaled));
                            auto state = OnSignalRead();
                            if(state.IsDisconn()){
                                if (!OnDisconnected()) {
                                    //クローズ要求時
                                    break;
                                }
                            }
                        } else {
                            //継承先のイベントハンドラを呼び出し
                            if (!OnFireEvent(signaled)) {
                                //クローズ要求時
                                break;
                            }
                        }
                    }
                    else {
                        //いずれかのハンドルが破棄されたのならインスタンスが破棄されている
                        break;
                    }
                }
            });
        }

        void OnReceivedPacket(const Packet* packet)
        {
            deserializer.Feed(packet);
        }

    protected:
        /// <summary>
        /// 名前付きパイプのGetLastError値のラッパークラス
        /// </summary>
        class WrapReadState
        {
        private:
            DWORD lastErr;
        public:
            static constexpr DWORD SUCCESSES[]
            { ERROR_SUCCESS, ERROR_PIPE_LISTENING, ERROR_IO_INCOMPLETE ,ERROR_IO_PENDING , ERROR_PIPE_CONNECTED,ERROR_OPERATION_ABORTED };
            static constexpr DWORD DISCONNECT[]
            { ERROR_PIPE_NOT_CONNECTED, ERROR_PIPE_LISTENING, ERROR_NO_DATA, ERROR_BROKEN_PIPE };

            WrapReadState() = delete;
            inline WrapReadState(DWORD lastErr) : lastErr(lastErr) {}
            inline const DWORD LastErr() const noexcept { return lastErr; }

            /// <summary>
            /// エラー値であったらwinrt::hresult_error例外を送出
            /// </summary>
            inline void ThrowIfInvalid() {
                if (IsInvalid()) {
                    winrt::throw_hresult(HRESULT_FROM_WIN32(lastErr));
                }
            }
            inline bool IsSuccess() const noexcept { return std::find(std::begin(SUCCESSES), std::end(SUCCESSES), lastErr) != std::end(SUCCESSES); }
            inline bool IsDisconn() const noexcept { return std::find(std::begin(DISCONNECT), std::end(DISCONNECT), lastErr) != std::end(DISCONNECT); }
            inline bool IsInvalid() const noexcept { return !IsSuccess() && !IsDisconn(); }
        };

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="handle">パイプハンドル</param>
        /// <param name="bufferSize">送信・受信バッファーサイズ</param>
        /// <param name="limitSize">送信・受信上限サイズ</param>
        /// <param name="costomEventCount">継承先のOnFireEvent呼び出し対象のイベント作成数。作成したイベントハンドルはCustomEventsで取得する。</param>
        SimpleNamedPipeBase(HANDLE handle, DWORD bufferSize, DWORD limitSize, size_t costomEventCount = 0)
            : handlePipe(handle)
            , bufferSize(bufferSize)
            , limitSize(limitSize)
            , readBuffer(std::make_unique<BYTE[]>(bufferSize))
            , receiver(bufferSize, limitSize, std::bind(&SimpleNamedPipeBase::OnReceivedPacket, this, std::placeholders::_1))
            , deserializer(bufferSize, limitSize, std::bind(&SimpleNamedPipeBase::OnReceived, this, std::placeholders::_1))
        {
            if( bufferSize < MIN_BUFFER_SIZE) {
                throw std::invalid_argument("BUF_SIZE is too short");
            }
            if (!handle) {
                throw std::invalid_argument("handle is invalid");
            }

            //受信イベント
            readEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(bool{ readEvent });
            readOverlap.hEvent = readEvent.get();

            //Close要求イベント
            closeEvent = winrt::handle{ CreateEventW(nullptr, true, false, nullptr) };
            winrt::check_bool(bool{ closeEvent });

            //引数で指定された継承クラス用のカスタムイベント
            for (size_t i = 0; i < costomEventCount; ++i) {
                winrt::handle h({ CreateEventW(nullptr, true , false, nullptr) });
                winrt::check_bool(bool{ h });
                customEvents.emplace_back(std::move(h));
            }

            //監視タスク開始
            watcherTask = WatchAsync().then([this](concurrency::task<void> prevTask) {
                try {
                    prevTask.wait();
                }
                catch (...)
                {
                    //監視タスクが例外発生で終了していた場合
                    OnTrapException(prevTask);
                }
            });
        }

        /// <summary>
        /// パイプハンドル
        /// </summary>
        /// <returns>パイプハンドル</returns>
        const HANDLE Handle() const { return handlePipe.get(); }

        /// <summary>
        /// 継承クラスで指定したカスタムイベントのリスト
        /// </summary>
        /// <returns>継承クラスで指定したカスタムイベントのリスト</returns>
        const std::vector<winrt::handle>& CustomEvents() const { return customEvents; }

        /// <summary>
        /// レシーバーを初期化
        /// </summary>
        void ResetReceiver() {
            receiver.Reset();
            deserializer.Reset();
        }

        /// 受信イベント
        /// </summary>
        /// <param name="buffer">受信データ</param>
        virtual void OnReceived(Buffer buffer) = 0;

        /// <summary>
        /// 切断イベント
        /// </summary>
        /// <returns>false:時はハンドルを閉じて以降は利用不可能とする。</returns>
        virtual bool OnDisconnected() = 0;

        /// <summary>
        /// 監視タスクでの捕捉例外通知
        /// </summary>
        /// <param name="errTask">エラーが発生したタスクオブジェクト</param>
        virtual void OnTrapException(concurrency::task<void> errTask) = 0;

        /// <summary>
        /// 継承クラスで設定したイベント発生通知
        /// </summary>
        /// <param name="handle">イベントハンドル</param>
        /// <returns>false:時はハンドルを閉じて以降は利用不可能とする。</returns>
        virtual bool OnFireEvent(HANDLE handle) = 0;

        /// <summary>
        /// 非同期受信完了時の処理
        /// </summary>
        /// <returns>パイプデータ読み込みステータス</returns>
        virtual WrapReadState OnRead()
        {
            DWORD readSize = 0;
            if (!GetOverlappedResult(handlePipe.get(), &readOverlap, &readSize, FALSE)) {
                auto state = WrapReadState{ GetLastError() };
                if (state.IsInvalid()) {
                    //クローズ中のエラーは無視
                    if (!closing.load()) {
                        state.ThrowIfInvalid();
                    }
                }
                return state;
            }
            //データ受信
            receiver.Feed(readBuffer.get(), readSize);
            return WrapReadState{ ERROR_SUCCESS };
        }

        /// <summary>
        /// 非同期受信開始
        /// </summary>
        /// <returns>パイプデータ読み込みステータス</returns>
        virtual WrapReadState OverappedRead()
        {
            //受信イベントリセット
            readOverlap.Offset = 0;
            readOverlap.OffsetHigh = 0;
            //受信処理
            // 同期的の受信できる限りは受信処理を継続
            while (ReadFile(handlePipe.get(), readBuffer.get(), bufferSize, nullptr, &readOverlap)) {
                auto state = OnRead();
                if(state.IsDisconn()) {
                    //切断状態となった
                    return state;
                }
                readOverlap.Offset = 0;
                readOverlap.OffsetHigh = 0;
            }
            //同期的に受信データを取得できないかエラーの場合
            auto state = WrapReadState{ GetLastError() };
            if (state.IsInvalid()) {
                if (!closing.load()) {
                    state.ThrowIfInvalid();
                }
            }
            return state;
        }

        /// <summary>
        /// 受信イベントシグナル時の処理
        /// </summary>
        /// <returns>false時は切断状態</returns>
        virtual WrapReadState OnSignalRead()
        {
            //非同期受信完了時処理実行
            auto state = OnRead();
            if (!state.IsDisconn()) {
                //切断していなければ、次回分の非同期受信処理を実行
                state = OverappedRead();
            }
            return state;
        }

        /// <summary>
        /// パイプハンドルを閉じる
        /// </summary>
        virtual void ClosePipeHandle()
        {
            bool expected = false;
            if (closing.compare_exchange_weak(expected, true)) {
                if (handlePipe) {
                    FlushFileBuffers(handlePipe.get());
                    handlePipe.close();
                }
            }
        }
    public:
        /// <summary>
        /// 非同期送信処理
        /// </summary>
        /// <param name="buffer">送信バッファー</param>
        /// <param name="size">送信サイズ</param>
        /// <param name="ct">キャンセルトークン</param>
        /// <returns>非同期タスク</returns>
        virtual concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size, concurrency::cancellation_token ct)
        {
            if (!handlePipe) {
                //handleが無効
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
            }
            if (size > limitSize) {
                throw std::length_error("size is too long");
            }
            //非同期時の送信完了待機のタスク
            return concurrency::create_task([this, buffer, size, ct]() {
                std::atomic_bool cancelFlag{false};
                concurrency::cancellation_token_registration cookie;
                if (ct.is_cancelable()) {
                    //キャンセル可能な場合のイベントディスパッチ処理
                    cookie = ct.register_callback([&cancelFlag, ct, &cookie]() {
                        cancelFlag.store(true);
                        try {
                            ct.deregister_callback(cookie);
                        }
                        catch (...) {}
                    });
                }
                //書き込み出来るのは同時に１つのみ
                concurrency::critical_section::scoped_lock lock(writeCs);
                //バッファーサイズ単位に分割して送信
                Serializer serialier(Buffer(buffer, size), bufferSize);
                winrt::handle dummyEvent{CreateEventW(nullptr, true, false, nullptr)};
                while(!cancelFlag.load()){
                    auto [packetData, header] = serialier.Next();
                    if (packetData.Empty()) {
                        //完了
                        break;
                    }
                    //ヘッダーを送信
                    WriteRaw(&header, sizeof(header), dummyEvent);
                    //データ本体を送信
                    WriteRaw(packetData.Pointer(), static_cast<DWORD>(packetData.Size()), dummyEvent);
#ifdef SNP_TEST_MODE
                    //テスト用の定義
                    if (onWritePacket) {
                        onWritePacket();
                    }
#endif
                }
                if (cancelFlag.load()) {
                    //キャンセル発生を送信
                    auto cancelHeader = Header::CreateCancel();
                    WriteRaw(&cancelHeader, sizeof(cancelHeader), dummyEvent);
                    if (ct.is_cancelable()) {
                        concurrency::cancel_current_task();
                    }
                }
            }, concurrency::task_options(ct));
        }

        virtual concurrency::task<void> WriteAsync(LPCVOID buffer, size_t size)
        {
            return WriteAsync(buffer, size, concurrency::cancellation_token::none());
        }

        virtual void Close()
        {
            winrt::check_bool(SetEvent(closeEvent.get()));
            watcherTask.wait();
        }

        virtual bool Valid() const { return bool{ handlePipe }; }

#ifdef SNP_TEST_MODE
        //テスト用の定義
        std::function<void(void)> onWritePacket;
#endif
    };

    //送信・受信の最大サイズ。ただし、実際はメモリー状況によるのでこの値を保証するものではない。
    inline static constexpr size_t MAX_DATA_SIZE = (std::numeric_limits<DWORD>::max)() - static_cast<DWORD>(sizeof SimpleNamedPipeBase::Header);

    /// <summary>
    /// 名前付きパイプサーバークラス
    /// </summary>
    template<DWORD BUF_SIZE, DWORD LIMIT=MAX_DATA_SIZE>
    class SimpleNamedPipeServer : public SimpleNamedPipeBase
    {
        static_assert(BUF_SIZE >= MIN_BUFFER_SIZE, "BUF_SIZE must be greater than or equal to MIN_BUFFER_SIZE");
        static_assert(LIMIT <= MAX_DATA_SIZE, "LIMIT must be less than or equal to MAX_DATA_SIZE");
    public:
        inline static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
        using Callback = std::function<void(SimpleNamedPipeServer<BUF_SIZE, LIMIT>&, const PipeEventParam&)>;

    private:
        const winrt::hstring pipeName;
        Callback callback;
        OVERLAPPED connectionOverlap { 0 };
        HANDLE connectionEvent;
        HANDLE disconnectionEvent;
        std::atomic_int connectedCount{0};

        /// <summary>
        /// 次回の接続待ち開始
        /// </summary>
        void BeginConnect()
        {
            ResetReceiver();
            connectionOverlap = { 0 };
            connectionOverlap.hEvent = connectionEvent;
            if (!ConnectNamedPipe(Handle(), &connectionOverlap)) {
                WrapReadState state{ GetLastError() };
                if (state.LastErr() == ERROR_PIPE_CONNECTED) {
                    //すでに接続済み
                    winrt::check_bool(SetEvent(connectionEvent));
                }
                state.ThrowIfInvalid();
            }
        }

    protected:
        virtual bool OnFireEvent(HANDLE handle) override
        {
            winrt::check_bool(ResetEvent(handle));
            bool connected = false;
            if (handle == connectionEvent) {
                //クライアント接続
                connectedCount.fetch_add(1);
                //接続イベント
                OnConnected();
                // 非同期データ受信処理開始
                auto state = OverappedRead();
                connected = !state.IsDisconn();
            }
            else if (handle == disconnectionEvent) {
                //切断処理実行イベント
                connected = false;
            }
            if (!connected) {
                //切断要求/検知したら切断
                //終了した接続の後始末をして次回接続の待機
                if (!OnDisconnected()) {
                    //クローズ要求時
                    return false;
                }
            }
            return true;
        }

        virtual void OnReceived(Buffer buffer) override
        {
            callback(*this, PipeEventParam{ PipeEventType::RECEIVED, buffer.Pointer(), buffer.Size()});
        }

        virtual bool OnDisconnected() override
        {
            int expceted = 0;
            if (connectedCount.compare_exchange_weak(expceted, 0)) {
                //すでに切断済み
                return true;
            }
            connectedCount.fetch_sub(1);
            callback(*this, PipeEventParam{ PipeEventType::DISCONNECTED, nullptr, 0 });
            try {
                //切断処理も行う
                DisconnectInner();
                //次回接続待ち開始
                BeginConnect();
            }
            catch (winrt::hresult_error& ex) {
                //INVALID_HANDLEの場合はすでにClose済みということなので例外とはしない
                if (ex.code() != HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)) {
                    throw;
                }
            }
            //引き続きパイプの処理は続ける
            return true;
        }

        virtual void OnTrapException(concurrency::task<void> errTask)
        {
            callback(*this, PipeEventParam{ PipeEventType::EXCEPTION, nullptr, 0, errTask });
        }

        /// <summary>
        /// 接続時処理
        /// </summary>
        virtual void OnConnected()
        {
            callback(*this, PipeEventParam{ PipeEventType::CONNECTED, nullptr, 0 });
        }

        /// <summary>
        /// 接続中のClientを切断する
        /// </summary>
        void DisconnectInner()
        {
            FlushFileBuffers(Handle());
            if (!DisconnectNamedPipe(Handle())) {
                auto lastErr = GetLastError();
                if (ERROR_PIPE_NOT_CONNECTED != lastErr) {
                    //閉じられた状態のエラーは無視
                    winrt::throw_last_error();
                }
            }
        }

    public:
        SimpleNamedPipeServer() = delete;
        SimpleNamedPipeServer(SimpleNamedPipeServer&&) = delete;
        SimpleNamedPipeServer(const SimpleNamedPipeServer&) = delete;

        SimpleNamedPipeServer& operator=(SimpleNamedPipeServer&&) = delete;
        SimpleNamedPipeServer& operator=(const SimpleNamedPipeServer&) = delete;

        /// <summary>
        /// 名前付きパイプサーバーハンドルの生成
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="psa">セキュリティディスクリプタ</param>
        /// <returns>名前付きパイプハンドル</returns>
        static HANDLE CreateServerHandle(LPCWSTR name, LPSECURITY_ATTRIBUTES psa)
        {
            //名前付きパイプの作成
            // 接続可能クライアント数=1; ローカルマシン接続のみ許可
            HANDLE handle = CreateNamedPipeW(
                name,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                BUF_SIZE,
                BUF_SIZE,
                0,
                psa);

            if (INVALID_HANDLE_VALUE == handle) {
                winrt::throw_last_error();
            }
            return handle;
        }

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="psa">セキュリティディスクリプタ</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeServer(LPCWSTR name, LPSECURITY_ATTRIBUTES psa, Callback callback)
            : SimpleNamedPipeBase(CreateServerHandle(name, psa), BUF_SIZE, LIMIT, 2)
            , pipeName(name)
            , callback(callback)
            , connectionEvent{ CustomEvents()[0].get() }
            , disconnectionEvent{ CustomEvents()[1].get() }
        {
            if (!callback) {
                throw std::invalid_argument("bad callback error");
            }
            //接続待ち開始
            BeginConnect();
        }

        virtual winrt::hstring PipeName() const { return pipeName; }

        /// <summary>
        /// クライアントを切断
        /// </summary>
        virtual void Disconnect()
        {
            int expceted = 0;
            if (connectedCount.compare_exchange_weak(expceted, 0)) {
                //すでに切断済み
                return;
            }
            //切断イベントをシグナル
            winrt::check_bool(SetEvent(disconnectionEvent));
        }

        virtual ~SimpleNamedPipeServer() {}
    };

    using TypicalSimpleNamedPipeServer = SimpleNamedPipeServer<TYPICAL_BUFFER_SIZE>;

    /// <summary>
    /// 名前付きパイプクライアント
    /// </summary>
    template<DWORD BUF_SIZE, DWORD LIMIT=MAX_DATA_SIZE>
    class SimpleNamedPipeClient : public SimpleNamedPipeBase
    {
        static_assert(BUF_SIZE >= MIN_BUFFER_SIZE, "BUF_SIZE must be greater than or equal to MIN_BUFFER_SIZE");
        static_assert(LIMIT <= MAX_DATA_SIZE, "LIMIT must be less than or equal to MAX_DATA_SIZE");
    public:
        using Callback = std::function<void(SimpleNamedPipeClient<BUF_SIZE, LIMIT>&, const PipeEventParam&)>;
        inline static constexpr DWORD BUFFER_SIZE = BUF_SIZE;
    private:
        const winrt::hstring pipeName;
        const Callback callback;

    protected:
        virtual void OnReceived(Buffer buffer) override
        {
            callback(*this, PipeEventParam{ PipeEventType::RECEIVED, buffer.Pointer(), buffer.Size() });
        }

        virtual bool OnFireEvent(HANDLE) override { return true; }

        virtual bool OnDisconnected() override
        {
            //切断時はパイプも閉じる
            callback(*this, PipeEventParam{ PipeEventType::DISCONNECTED, nullptr, 0 });
            return false;
        }

        virtual void OnTrapException(concurrency::task<void> errTask)
        {
            callback(*this, PipeEventParam{ PipeEventType::EXCEPTION, nullptr, 0, errTask });
        }

    public:
        SimpleNamedPipeClient() = delete;
        SimpleNamedPipeClient(const SimpleNamedPipeClient&) = delete;
        SimpleNamedPipeClient& operator=(const SimpleNamedPipeClient&) = delete;

        SimpleNamedPipeClient(SimpleNamedPipeClient&&) = delete;
        SimpleNamedPipeClient& operator=(SimpleNamedPipeClient&&) = delete;

        /// <summary>
        /// 名前付きパイプサーバーへの接続
        /// </summary>
        /// <param name="name">名称</param>
        /// <returns>ハンドル</returns>
        static HANDLE OpendPipeHandle(LPCWSTR name)
        {
            //接続可能状態まで待つ
            winrt::check_bool(WaitNamedPipe(name, NMPWAIT_USE_DEFAULT_WAIT));
            //サーバーへ接続
            HANDLE handle = CreateFileW(
                name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr);

            if(INVALID_HANDLE_VALUE == handle){
                winrt::throw_last_error();
            }
            return handle;
        }

        /// <summary>
        /// コンストラクタ
        /// </summary>
        /// <param name="name">名前付きパイプ名称</param>
        /// <param name="callback">イベント通知コールバック</param>
        SimpleNamedPipeClient(LPCWSTR name, Callback callback)
            : SimpleNamedPipeBase(OpendPipeHandle(name), BUF_SIZE, LIMIT)
            , pipeName(name)
            , callback(callback)
        {
            if (!callback) {
                throw std::invalid_argument("bad callback error");
            }
            //非同期受信処理開始
            OverappedRead();
        }

        virtual winrt::hstring PipeName() const { return pipeName; }

        virtual ~SimpleNamedPipeClient() {}
    };

    using TypicalSimpleNamedPipeClient = SimpleNamedPipeClient<TYPICAL_BUFFER_SIZE>;
}
