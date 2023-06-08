#include "pch.h"
#include <random>
#include <windows.h>
#include <string>
#include <sstream>
#include <memory>
#include <numeric>
#include <limits>
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
            Assert::AreEqual(TypicalSimpleNamedPipeServer::MAX_DATA_SIZE, static_cast<size_t>((std::numeric_limits<DWORD>::max)() - static_cast<DWORD>(sizeof DWORD)));

            Assert::AreEqual(TypicalSimpleNamedPipeClient::BUFFER_SIZE, TYPICAL_BUFFER_SIZE);
            Assert::AreEqual(TypicalSimpleNamedPipeClient::MAX_DATA_SIZE, static_cast<size_t>((std::numeric_limits<DWORD>::max)() - static_cast<DWORD>(sizeof DWORD)));
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

            Assert::AreEqual(static_cast<std::wstring_view>(pipeName), static_cast<std::wstring_view>(server.PipeName()));

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
            Assert::AreEqual(static_cast<std::wstring_view>(pipeName), static_cast<std::wstring_view>(client.PipeName()));

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

        void HelloNtimes(const ULONG repeat)
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

            auto remain = repeat;

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
            for (auto i = 0ul; i < repeat; ++i) {
                std::wstringstream oss;
                oss << L"HELLO WORLD![" << i << "]";
                auto message = oss.str();
                expectedValues.emplace_back(std::wstring(L"echo: ") + message);

                client.WriteAsync(&message[0], message.size() * sizeof(WCHAR)).wait();

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

        BEGIN_TEST_METHOD_ATTRIBUTE(Hello1000times)
            TEST_PRIORITY(2)
        END_TEST_METHOD_ATTRIBUTE()
        TEST_METHOD(Hello1000times)
        {
            HelloNtimes(1000);
        }

        TEST_METHOD(Hello3times)
        {
            HelloNtimes(3);
        }

        void ConnectNtimes(const ULONG repeat)
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
            for (auto i = 0ul; i < repeat; ++i) {
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

        BEGIN_TEST_METHOD_ATTRIBUTE(Connect1000times)
            TEST_PRIORITY(2)
        END_TEST_METHOD_ATTRIBUTE()
        TEST_METHOD(Connect1000times)
        {
            ConnectNtimes(1000);
        }

        TEST_METHOD(Connect3times)
        {
            ConnectNtimes(3);
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
                    ps.Disconnect();
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

            //未接続のときにDionnectを読んでも副作用はない
            server.Disconnect();

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::wstring echoMessage;

            {
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

                WCHAR hello[] = L"HELLO WORLD![1]";

                client.WriteAsync(&hello[0], sizeof(hello)).wait();

                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                    Assert::Fail();
                }

                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                    Assert::Fail();
                }

                serverErrTask.wait();
                clientErrTask.wait();

                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![1]"), echoMessage);
            }

            echoComplete.reset();
            closedEvent.reset();

            //切断後も再接続できるか？
            {
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

                WCHAR hello[] = L"HELLO WORLD![2]";

                client.WriteAsync(&hello[0], sizeof(hello)).wait();

                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                    Assert::Fail();
                }

                if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                    Assert::Fail();
                }
                serverErrTask.wait();
                clientErrTask.wait();

                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![2]"), echoMessage);
            }
            server.Close();
        }

        TEST_METHOD(WriteCancel)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            constexpr size_t BUFFER_SIZE = 512;

            SimpleNamedPipeServer<BUFFER_SIZE> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
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

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
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

            constexpr size_t SAMPLE_SIZE = BUFFER_SIZE * 8;
            constexpr size_t SAMPLE_BYTE_SIZE = SAMPLE_SIZE * sizeof(int);
            auto expected = std::make_unique<int[]>(SAMPLE_SIZE);
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }

            {
                auto cts = concurrency::cancellation_token_source();
                cts.cancel();
                Assert::ExpectException<concurrency::task_canceled>([&]() {
                    client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE, cts.get_token()).get();
                });
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
        }

        TEST_METHOD(TooLongWriteSize)
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

            WCHAR dummy[] = L"DUMMY";

            Assert::ExpectException<std::length_error>([&]() {
                client.WriteAsync(&dummy[0], client.MAX_DATA_SIZE + 1).wait();
            });
        }

        TEST_METHOD(OverbufferTransfer)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            constexpr size_t BUFFER_SIZE = 1024;
            constexpr size_t SAMPLE_SIZE = 4 * 1024;
            auto expected = std::make_unique<int[]>(SAMPLE_SIZE);
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }

            SimpleNamedPipeServer<BUFFER_SIZE> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    //そのままか返す
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
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

            Assert::AreEqual(static_cast<std::wstring_view>(pipeName), static_cast<std::wstring_view>(server.PipeName()));

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;

            auto actual = std::make_unique<int[]>(SAMPLE_SIZE);
            constexpr size_t SAMPLE_BYTE_SIZE = SAMPLE_SIZE * sizeof(int);

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    if (param.readedSize == SAMPLE_BYTE_SIZE) {
                        memcpy(&actual[0], param.readBuffer, param.readedSize);
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
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            echoComplete.reset();

            //もう1回
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == echoComplete.wait(1000)) {
                Assert::Fail();
            }
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            client.Close();
            if (concurrency::COOPERATIVE_TIMEOUT_INFINITE == closedEvent.wait(1000)) {
                Assert::Fail();
            }

            serverErrTask.wait();
            clientErrTask.wait();
        }
    };
}
