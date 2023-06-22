# シンプルな名前付きパイプ 
Windowsのサービスと通常アプリケーションとのあいだのプロセス間通信のコードを何度も何度も書いてきたので、機能を絞ったシンプルな実装として公開する。

# 特徴
* ローカルマシン内のアプリ間での1対1の通信を想定した実装。
* 送信と受信イベントが1:1で対応する。
* 必要最小限の実装でシンプル。ヘッダーファイル `SimpleNamedPipe.h`をインクルードするだけで動く。
* サービスやドライバーでも使えるように、`SECURITY_ATTRIBUTES` を指定可能。
* Windows10 以降に対応。レガシー環境は考慮していない。
* *winrt*, *ppl* に依存。
* 非同期処理に対応。というか、非同期オンリー。

# 使い方
ヘッダーファイル `SimpleNamedPipe.h` をinclude する。

サーバーとクライアントは、`SimpleNamedPipeServer<BUF_SIZE,LIMIT>` と　`SimpleNamedPipeClient<BUF_SIZE,LIMIT>` の組み合わせで利用すること。

データ長保証のためヘッダー情報を付加しており、上記の組み合わせでないと正常に動作しない。

`BUF_SIZE` は通信バッファーサイズを指定する。`MIN_BUFFER_SIZE` 以上でなければコンパイルエラーとなる。この値は通信バッファーサイズとなる。

`LIMIT` は `MAX_DATA_SIZE` 以下である必要があり、そうでない場合はコンパイルエラーとなる。この値は省略可能で`MAX_DATA_SIZE` (uint32_tの上限値-8) と同値となる。

`WriteAsync` のデータサイズと受信時のデータサイズがこの値を上回っていた場合は例外を送出する。受信時にこの例外が発生した場合は接続を破棄する。

テンプレート引数の `BUF_SIZE`, `LIMIT` はサーバーとクライアントで異なる値でも動作する。 

サーバー、クライアントのクラスともにテンプレート引数を推奨値を設定したものが定義済みであり、これらの仕様を推奨する。

- `TypicalSimpleNamedPipeServer` → `SimpleNamedPipeServer<TYPICAL_BUFFER_SIZE, MAX_DATA_SIZE>` 
- `TypicalSimpleNamedPipeClient` → `SimpleNamedPipeClient<TYPICAL_BUFFER_SIZE, MAX_DATA_SIZE>` 


## サーバー
`SimpleNamedPipeServer<BUF_SIZE, LIMIT>` でサーバーインスタンスを生成する。`LIMIT`の指定は省略可能である。

### コンストラクタ
- 第1引数: パイプ名称を指定する。
- 第2引数: `LPSECURITY_ATTRIBUTES` を指定可能。nullptr時は既定のセキュリティ記述子となる。
- 第3引数: コールバック関数を指定する。

コンストラクタの例外を補足してエラー対応をおこなう。以下のコード例を参照。

```cpp
//パイプサーバーのサンプル
try{
SimpleNamedPipeServer<4096> pipeServer(L"\\\\.\\pipe\\SimplePipeTest", nullptr, [&](auto& ps, const auto& param) {
    switch (param.type) {
    case PipeEventType::CONNECTED:
        //クライアントが接続した
        break;
    case PipeEventType::DISCONNECTED:
        //クライアントが切断した
        break;
    case PipeEventType::RECEIVED:
        //データ受信
        break;
    case PipeEventType::CLOSED:
        //パイプハンドルが破棄された
        break;
    case PipeEventType::EXCEPTION:
        //管理スレッドで例外発生
        break;
    }
});
} catch (winrt::hresult_error& ex) {
    if(ex.code().value == (HRESULT_FROM_WIN32(ERROR_PIPE_BUSY))){
        //同名のパイプが既に存在する
    }
    //他のエラーについてはWin32エラーコードを参照
}
```

### データ送信
データ送信には `WriteAsync` を使用する。非同期実行するため、戻り値にタスクオブジェクト`concurrency::task<void>`を返す。

送信データサイズは、テンプレート引数の`LIMIT` 以下に制限される。これ以上の値を指定した場合は `std::length_error` が発生する。

送信データサイズがテンプレート引数 `BUF_SIZE` を超えた値であっても送信は可能である。

また、上記のデータサイズ制限未満でも実行環境のメモリーリソースによっては、メモリー不足によって動作しない場合がある。

実際に送信完了するまで、データバッファー変更せずに維持する必要がある。戻り値のタスクオブジェクトで実行状態を確認することができる。

複数のスレッドから同時に実行した場合は、各々の`WriteAsync`は独立して実行され混じることはない。ただし、送信の実行順は保証しない。

- 第1引数: データバッファーポインター
- 第2引数: データサイズ
- 第3引数: キャンセルトークン
- 戻り値: concurrency::task<void>

```cpp
// 同期的に実行
try{
    server.WriteAsync(buffer, size).wait();
}catch(winrt::hresult_error& ex){
    if(ex.code().value == HRESULT_FROM_WIN32(ERROR_PIPE_LISTENING)){
        //未接続状態
    }else if(ex.code().value == HRESULT_FROM_WIN32(ERROR_NO_DATA)){
        //パイプが閉じられた
    }else if(ex.code().value == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)){
        //パイプハンドルが破棄済み。 このインスタンスは利用できない。
    }
    //他のエラーについてはWin32エラーコードを参照
}catch(std::length_error& ex){
    //テンプレート引数 LIMIT より大きなサイズを送信した。
}
```

### 接続中のクライアントを切断
接続中のクライアントを切断するには `Disconnect` を利用する。接続していない場合でも成功する。

切断後も新たな接続を受け入れることができる。

インスタンスが有効な限りは例外を返さない。

```cpp
server.Disconnect();
```

### パイプ接続を閉じる
パイプ接続を閉じて接続中のクライアントは切断するには、`Close` を利用する。

インスタンスが有効な限りは例外を送出しない。

```cpp
server.Close();
```

### データ受信,イベント受信
コンストラクタで指定したコールバック関数に、受信データとイベント通知をコールバックする。

```cpp
TypicalSimpleNamedPipeServer pipeServer(L"\\\\.\\pipe\\SimplePipeTest", nullptr, [&](auto& ps, const auto& param) {
    switch (param.type) {
    case PipeEventType::CONNECTED:
        //クライアントが接続した
        std::wcout << L"connected" << std::endl;
        break;
    case PipeEventType::DISCONNECTED:
        //クライアントが切断した
        std::wcout << L"diconnected" << std::endl;
        break;
    case PipeEventType::RECEIVED:
        //受信データあり
        {
            auto message = std::wstring(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
            std::wcout << message << std::endl;
        }
        break;
    case PipeEventType::CLOSED:
        //パイプハンドルが破棄された
        break;
    case PipeEventType::EXCEPTION:
        //管理スレッドで例外発生
        //この時点でパイプ通信機能は利用できなくなる。
        if (param.errTask) {
            //管理タスクの例外を再取得する
            try { 
                param.errTask.value().wait(); 
            }
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
        break;
    }
});
```
#### PipeEventParam::type == PipeEventType::CONNECTED
クライアントが接続した場合にコールバックする。`SimpleNamedPipeServer` のコールバックでのみ有効。
#### PipeEventParam::type == PipeEventType::DISCONNECTED
クライアントが切断した場合にコールバックする。`Disconnect` を呼び出した場合、`SimpleNamedPipeClient` が `Close` した場合に発行する。

`SimpleNamedPipeClient` が `Close` した場合には、呼び出し元のインスタンスは利用できない。
#### PipeEventParam::type == PipeEventType::RECEIVED
データ受信時にコールバックする。このデータは送信側の `WriteAsync`と1:1 で対応する。

受信データは `PipeEventParam::readBuffer`, 受信サイズは`PipeEventParam::readedSize`に格納されている。

バッファーの内容はこの関数中でしか保証しない。事後に利用する場合はコピーする。
#### PipeEventParam::type == PipeEventType::CLOSED
パイプハンドルが閉じられた際にコールバックする。これ以降は呼び出し元のインスタンスは利用できない。 `SimpleNamedPipeServer` のコールバックでのみ有効。

#### PipeEventParam::type == PipeEventType::EXCEPTION
監視タスク中の例外をコールバックする。このコールバックがあった場合は、インスタンスは利用できない状態となっている。

エラーが発生した管理タスクのタスクオブジェクトが `PipeEventParam::errTask.value() ` に格納されていので、`Task.wait()` 関数から発生した例外を確認できる。

## クライアント
`SimpleNamedPipeClient<BUF_SIZE,LIMIT>` でクライアントインスタンスを生成する。`LIMIT`の指定は省略可能である。

```cpp
try{
    SimpleNamedPipeClient<4096> pipeClient(PIPE_NAME, [&](auto& ps, const auto& param){
    switch (param.type) {
    case PipeEventType::CONNECTED:
        //クライアントが接続した
        break;
    case PipeEventType::DISCONNECTED:
        //クライアントが切断した
        break;
    case PipeEventType::RECEIVED:
        //データ受信
        break;
    case PipeEventType::EXCEPTION:
        //管理スレッドで例外発生
        break;
    }
    });
}
catch(winrt::hresult_error& ex){
    if(ex.code().value == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
        //指定したパイプ名を持つパイプが存在しない
    } else if(HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT)){
        //すでにサーバーには別のクライアントが接続済み
    }
    //他のエラーについてはWin32エラーコードを参照
}
```

### データ送信
データ送信はサーバーと同様のプロトタイプである。

### パイプ接続を閉じる
サーバーと同様である。

### データ受信,イベント受信
```PipeEventType::CONNECTED``` イベントが存在しない以外は、サーバーと同様である。

# 注意点

## winrt::hresult_errorの注意点
APIのエラーは `winrt::hresult_error` 例外として捕捉できるが、`winrt::hresult_error::code()` によって取得できるエラーコードはHRESULT型となっている。

Win32エラーコードと比較する際は `HRESULT_FROM_WIN32` マクロで変換する必要がある。

```cpp
try{
    ...
}catch(winrt::hresult_error& ex){
   if(ex.code().value == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){
        std::wcerr << L"接続先が存在しない" << std::endl;
    }
    else if(ex.code().value == HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT) 
            || ex.code().value == HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)){
        std::wcerr << L"接続済みのクライアントがある" << std::endl;
    }
    else {
        //他のエラー
        std::wcerr << ex.message().c_str() << std::endl;
    }
}
```
