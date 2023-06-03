# シンプルな名前付きパイプ 
Windowsのサービスと通常アプリケーションとのあいだのプロセス間通信のコードを何度も何度も書いてきたので、機能を絞ったシンプルな実装として公開する。

# 特徴
* ローカルマシン内のアプリ間での1対1の通信を想定した実装。
* 必要最小限の実装でシンプル。ヘッダーファイル `SimpleNamedPipe.h`をインクルードするだけで動く。
* サービスやドライバーでも使えるように、`SECURITY_ATTRIBUTES` を指定可能。
* Windows10 以降に対応。レガシー環境は考慮していない。
* *winrt*, *ppl* に依存。
* 非同期処理に対応。というか、非同期オンリー。
* サイズが大きくないメッセージのやり取りを想定している。
  * 大きなデータは共有メモリーなどを利用すべき。

# 使い方
ヘッダーファイル `SimpleNamedPipe.h` をinclude する。

## サーバー
`SimpleNamedPipeServer<BUF_SIZE>` でサーバーインスタンスを生成する。

`BUF_SIZE` は受信バッファーサイズを指定する。`BUIF_SIZE` は `WriteAsync` 実行時の送信サイズ上限になる。

第2引数に `LPSECURITY_ATTRIBUTES` を指定可能。nullptr時は既定のセキュリティ記述子となる。

`TypicalSimpleNamedPipeServer` は `SimpleNamedPipeServer<2048>` のエイリアスとして定義している。

```cpp
SimpleNamedPipeServer<4096> pipeServer(PIPE_NAME, nullptr, [&](auto& ps, const auto& param) {
    switch (param.type) {
    case PipeEventType::CONNECTED:
        std::wcout << L"connected" << std::endl;
        break;
    case PipeEventType::DISCONNECTED:
        std::wcout << L"diconnected" << std::endl;
        break;
    case PipeEventType::RECEIVED:
        {
            auto message = std::wstring(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
            std::wcout << message << std::endl;
        }
        break;
    case PipeEventType::EXCEPTION:
        if (param.errTask) {
            try { param.errTask.value().wait(); }
            catch (winrt::hresult_error& ex) {
                std::wcout << L"Exception occurred: " << ex.message().c_str() << std::endl;
            }
            catch (std::exception& ex) {
                std::wcout << L"Exception occurred: " << ex.what() << std::endl;
            }
            catch (...) {
                std::wcout << L"Exception occurred" << std::endl;
            }
        }
        else {
            std::wcout << L"Exception occurred" << std::endl;
        }
        break;
    }
});
```

## クライアント
`SimpleNamedPipeClient<BUF_SIZE>` でクライアントインスタンスを生成する。

`BUF_SIZE` は受信バッファーサイズを指定する。`BUIF_SIZE` は `WriteAsync` 実行時の送信サイズ上限になる。

`TypicalSimpleNamedPipeClient` は `SimpleNamedPipeClient<2048>` のエイリアスとして定義している。

```cpp
SimpleNamedPipeClient<4096> pipeClient(PIPE_NAME, [&](auto&, const auto& param) {
    switch (param.type) {
    case PipeEventType::DISCONNECTED:
        std::wcout << L"diconnected" << std::endl;
        break;
    case PipeEventType::RECEIVED:
        {
            auto message = std::wstring(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
            std::wcout << message << std::endl;
            receivedEvent.set();
        }
        break;
    case PipeEventType::EXCEPTION:
        if (param.errTask) {
            try { param.errTask.value().wait(); }
            catch (winrt::hresult_error& ex) {
                std::wcout << L"Exception occurred: " << ex.message().c_str() << std::endl;
            }
            catch (std::exception& ex) {
                std::wcout << L"Exception occurred: " << ex.what() << std::endl;
            }
            catch (...) {
                std::wcout << L"Exception occurred" << std::endl;
            }
        }
        else {
            std::wcout << L"Exception occurred" << std::endl;
        }
        break;
    }
});
```
# 利用上の注意点

## 受信バッファーについて
イベントコールバックの受信バッファーはコールバック中の間だけしか値の保証をしない。

そのため、継続して受信バッファーの内容を利用する場合はコピーする必要がある。

## 送信データについて
`WritaAsync` が非同期で実行されている場合は、実際に送信処理が行われるまで保持する必要がある。

`wait` 関数で同期的に待機する、`then` 関数で継続タスク中で後始末をするなどの対応が必要。

*※ただし、プロセス間通信として利用している場合に非同期的に動作することは稀である。*

# 例外処理
## コンストラクタの例外
`SimpleNamedPipeServer`, `SimpleNamedPipeClient` のコンストラクタでは、`winrt::hresult_error` を送出する。

`winrt::hresult_error::code()` にてエラー発生時の `GetLastError()` のコードを取得できる。例えば、接続する名前付きパイプが存在しない場合は、`ERROR_FILE_NOT_FOUND` となる。

```cpp
try{
    TypicalSimpleNamedPipeClient pipeClinet(...);
    ...
}
catch(winrt::hresult_error& ex){
    if(ex.code() == ERROR_FILE_NOT_FOUND){
        std::wcerr << L"接続先が存在しない" << std::endl;
    } 
    else {
        //エラーコードに対応したエラーメッセージを出力
        std::wcerr << ex.message().c_str() << std::endl;
    }
}
```

## WriteAsyncの例外
データ送信メソッドの `WriteAsync` は非同期実行なので`concurrency::task` を返します。

`concurrency::task::wait` を呼び出すことで、タスクに同期しつつ例外が発生していた場合は、次の例外の再送出が行われる。

* API呼び出しのエラーは `winrt::hresult_error` 
* Closeの後に呼び出した場合は `std::runtime_error` 
* データサイズがクラスパラメータ`BUF_SIZE` より大きい場合は `std::length_error`

```cpp
try{
    pipeClient.WriteAsync(L"HELLO WORLD!", 13 * sizeof(WCHAR), concurrency::cancellation_token::none()).wait();
}
catch (winrt::hresult_error& ex)
{
    std::wcerr << ex.message().c_str() << std::endl;
}
catch(std::runtime_error& )
{
     std::wcerr << L"接続はすでに閉じられています" << std::endl;
}
catch(std::length_error& )
{
     std::wcerr << L"BUF_SIZEより大きいサイズは送信できません" << std::endl;
}
```
## 監視タスクの例外
インスタンス生成時から、別タスクで監視タスクが実行される。

監視タスク内で例外が発生した場合は、例外通知イベントから例外を検知できる。

エラー発生イベントで発生例外を確認するには、コールバック第2引数の `PipeEventParam::errTask` を再評価することで例外の再送出を実行して捕捉した例外を確認する。
```cpp
TypicalSimpleNamedPipeClient pipeClient(PIPE_NAME, [&](auto&, const auto& param) {
    // ...
    case PipeEventType::EXCEPTION:
        //監視タスクで例外発生
        if (param.errTask) {
            try { param.errTask.value().wait(); }
            catch(winrt::hresult_error& ex){
                std::wcout << L"Exception occurred: " << ex.message().c_str() << std::endl;
            }
            catch (std::exception& ex) {
                std::wcout << L"Exception occurred: " << ex.what() << std::endl;
            }
            catch (...) {
                std::wcout << L"Exception occurred" << std::endl;
            }
        }
        else {
            std::wcout << L"Exception occurred" << std::endl;
        }
        break;
    }
});
```
