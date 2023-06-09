﻿//SimpleNamedPipe.h 用サンプル クライアント
#include "pch.h"
#include <iostream>
#include <exception>
#include <sstream>
#include <tuple>
#include <memory.h>
#include "..\inc\SimpleNamedPipe.h"

using namespace winrt;
using namespace Windows::Foundation;

constexpr static LPCWSTR PIPE_NAME = L"\\\\.\\pipe\\SimplePipeTest";

int main()
{
    using namespace abt::comm::simple_pipe;

    init_apartment();

    std::wcout.imbue(std::locale(""));
    std::wcerr.imbue(std::locale(""));
    try {
        concurrency::event receivedEvent;

        constexpr LONG repeat = 1000;
        LONG remain = repeat;

        TypicalSimpleNamedPipeClient pipeClient(PIPE_NAME, [&](auto&, const auto& param) {
            //※イベントコールバックはスレッドが異なる可能性がある
            switch (param.type) {
            case PipeEventType::DISCONNECTED:
                std::wcout << L"diconnected" << std::endl;
                //デッドロック回避でここでもシグナルにする
                receivedEvent.set();
                break;
            case PipeEventType::RECEIVED:
                //データ受信
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wcout << m << std::endl;
                    if (0 == InterlockedDecrement(&remain)) {
                        //echo受信完了
                        receivedEvent.set();
                    }
                }
                break;
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
                //通信の継続はできないのでシグナルにする
                receivedEvent.set();
                break;
            }
        });

        for (int i = 0; i < repeat; ++i) {
            //メッセージを送信
            std::wstringstream oss;
            oss << L"HELLO WORLD! [" << i << "]";
            std::wstring msg = oss.str();
            pipeClient.WriteAsync(&msg[0], msg.size() * sizeof(WCHAR), concurrency::cancellation_token::none()).wait();
        }

        //受信データ待ち
        receivedEvent.wait();
    }
    catch (winrt::hresult_error& ex)
    {
        //接続先がないなどの例外は winrt::hresult_error で捕捉する。
        //例外の詳細はex.code() でHRESULT型に変換されたWindowsシステムエラーコードを取得する
        // エラーコードの詳細は以下のページを参照
        // https://learn.microsoft.com/en-us/windows/win32/api/winerror/nf-winerror-hresult_from_win32
        // https://learn.microsoft.com/ja-jp/windows/win32/debug/system-error-codes#system-error-codes
        if (ex.code().value == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            //サーバーとなるpipeが存在しない
            std::wcerr << L"接続先が存在しません: " << PIPE_NAME << std::endl;
        }
        else {
            std::wcerr << ex.message().c_str() << std::endl;
        }
        return 1;
    }
    catch (std::exception& ex)
    {
        std::wcerr << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
