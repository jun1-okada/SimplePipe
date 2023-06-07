#include "pch.h"
#include <windows.h>
#include <string>
#include <sstream>
#include <memory>
#include <numeric>
#include <algorithm>
#include <memory.h>
#include <ppl.h>
#include <ppltasks.h>
#include "CppUnitTest.h"
#include "../inc/SimpleNamedPipe.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace abt::comm::simple_pipe::test
{
    using namespace abt::comm::simple_pipe;

    //abt::comm::simple_pipe::Receiverテストクラス
    TEST_CLASS(TestSimplePipe)
    {
    public:
        TEST_METHOD(Constants)
        {
            Assert::AreEqual(TypicalSimpleNamedPipeServer::BUFFER_SIZE, TYPICAL_BUFFER_SIZE);
            Assert::AreEqual(TypicalSimpleNamedPipeServer::MAX_DATA_SIZE, TYPICAL_BUFFER_SIZE - static_cast<DWORD>(sizeof DWORD));

            Assert::AreEqual(TypicalSimpleNamedPipeClient::BUFFER_SIZE, TYPICAL_BUFFER_SIZE);
            Assert::AreEqual(TypicalSimpleNamedPipeClient::MAX_DATA_SIZE, TYPICAL_BUFFER_SIZE - static_cast<DWORD>(sizeof DWORD));
        }

        TEST_METHOD(HelloEcho)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;
            
            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    echoMessage = m;
                    echoComplete.set();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            WCHAR hello[] = L"HELLO WORLD!";

            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::AreEqual(std::wstring(L"echo: HELLO WORLD!"), echoMessage);
        }

        TEST_METHOD(Hello1000times)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            constexpr ULONG REPEAT = 1000;
            auto remain = REPEAT;

            std::vector<std::wstring> actualValues;
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    actualValues.emplace_back(m);
                    if (0 == InterlockedDecrement(&remain)) {
                        echoComplete.set();
                    }
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    break;
                }
            });


            std::vector<std::wstring> expectedValues;
            for (int i = 0; i < REPEAT; ++i) {
                std::wstringstream oss;
                oss << L"HELLO WORLD![" << i << "]";
                auto message = oss.str();
                expectedValues.emplace_back(std::wstring(L"echo: ") + message);

                client.WriteAsync(&message[0], message.size() * sizeof(WCHAR));

            }

            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::IsTrue(std::equal(expectedValues.begin(), expectedValues.end(), actualValues.begin(), actualValues.end()));
        }

        TEST_METHOD(Connect1000times)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::wstring echoMessage;
            for (auto i = 0; i < 1000; ++i) {
                TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                    switch (param.type) {
                    case PipeEventType::DISCONNECTED:
                        break;
                    case PipeEventType::RECEIVED:
                    {
                        std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                        echoMessage = m;
                        echoComplete.set();
                    }
                    break;
                    case PipeEventType::EXCEPTION:
                        //監視タスクで例外発生
                        if (param.errTask) {
                            clientErrTask = param.errTask.value();
                        }
                        break;
                    }
                });


                std::wstringstream oss;
                oss << L"HELLO WORLD![" << i << "]";
                auto message = oss.str();

                client.WriteAsync(&message[0], message.size() * sizeof(WCHAR)).wait();

                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                    Assert::Fail();
                }

                client.Close();
                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                    Assert::Fail();
                }
                try {
                    serverErrTask.wait();
                    clientErrTask.wait();
                }
                catch (winrt::hresult_error& ex)
                {
                    Assert::Fail(ex.message().c_str());
                }
                catch (std::exception& ex)
                {
                    Assert::Fail(winrt::to_hstring(ex.what()).c_str());
                }
                Assert::AreEqual(std::wstring(L"echo: ") + message, echoMessage);

                echoComplete.reset();
                closedEvent.reset();
            }

            server.Close();
        }

        TEST_METHOD(DiconnectByServer)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                    ps.Close();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    echoMessage = m;
                    echoComplete.set();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            WCHAR hello[] = L"HELLO WORLD!";

            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::AreEqual(std::wstring(L"echo: HELLO WORLD!"), echoMessage);
        }

        TEST_METHOD(WriteCancel)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    echoMessage = m;
                    echoComplete.set();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            WCHAR hello[] = L"HELLO WORLD!";

            auto cts = concurrency::cancellation_token_source();
            cts.cancel();
            Assert::ExpectException<concurrency::task_canceled>([&]() {
                client.WriteAsync(&hello[0], sizeof(hello), cts.get_token());
            });

            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();
        }
    };
}
