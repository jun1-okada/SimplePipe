//SimpleNamedPipe.h 用サンプル エコーメッセージサーバー
#include "pch.h"
#include <iostream>
#include <sstream>
#include <conio.h>
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
        TypicalSimpleNamedPipeServer pipeServer(PIPE_NAME, nullptr, [&](auto& ps, const auto& param) {
            //※イベントコールバックはスレッドが異なる可能性がある
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
                    std::wostringstream oss;
                    oss << L"echo: " << message;
                    auto echoMessage(oss.str());
                    ps.WriteAsync(echoMessage.c_str(), echoMessage.size() * sizeof(WCHAR), concurrency::cancellation_token::none()).wait();
                }
                break;
            case PipeEventType::EXCEPTION:
                //監視タスクで例外発生
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

        std::wcout << L"Press 'Q' to exit" << std::endl;
        bool bExit = false;
        do
        {
            int key = _getch();
            if (key == 'Q' || key == 'q') {
                bExit = true;
            }
        } while (!bExit);
    }
    catch (winrt::hresult_error& ex)
    {
        //接続先がないなどの例外は winrt::hresult_error で捕捉する。
        //例外の詳細はex.code にWindowsシステムエラーコードが格納させている
        // エラーコードの詳細は以下のページを参照
        // https://learn.microsoft.com/ja-jp/windows/win32/debug/system-error-codes--0-499-
        std::wcerr << ex.message().c_str() << std::endl;

        return 1;
    }
    catch (std::exception& ex)
    {
        std::wcerr << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
