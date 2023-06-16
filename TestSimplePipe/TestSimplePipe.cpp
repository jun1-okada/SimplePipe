#include "pch.h"
#include <random>
#include <windows.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <limits>
#include <algorithm>
#include <memory.h>
#include <ppl.h>
#include <ppltasks.h>

#include "CppUnitTest.h"

#define SNP_TEST_MODE   //テストモード有効
#include "../inc/SimpleNamedPipe.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace abt::comm::simple_pipe::test
{
    using WaitCnt = std::tuple<int, bool>;

    WaitCnt WC(int n = 1, bool isTimeout = false)
    {
        return WaitCnt(n, isTimeout);
    }
}

namespace Microsoft::VisualStudio::CppUnitTestFramework {
    template<>
    inline std::wstring ToString(const abt::comm::simple_pipe::test::WaitCnt& c)
    {
        std::wostringstream oss;
        oss << L"{" << std::get<0>(c) << "," << std::get<1>(c) << L"}";
        return oss.str();
    }
}

namespace abt::comm::simple_pipe::test
{
    using namespace abt::comm::simple_pipe;

    std::wstring hresultToStr(const winrt::hresult_error& e) {
        std::wostringstream oss;
        oss << L"HRESULT: " << std::hex << e.code() << " " << e.message().c_str();
        return oss.str();
    }


    struct EventCounter
    {
        std::atomic_int cnt;
        concurrency::event evt;
        void set() { cnt.fetch_add(1); evt.set(); }
        void reset() { cnt.fetch_sub(1); evt.reset(); }
        int count() { return cnt.load(); }

        WaitCnt wait(unsigned int timeout=concurrency::COOPERATIVE_TIMEOUT_INFINITE)
        {
            auto res = evt.wait(timeout);
            return { cnt.load(), res == concurrency::COOPERATIVE_WAIT_TIMEOUT };
        }
    };

    //abt::comm::simple_pipe::Receiverテストクラス
    TEST_CLASS(TestSimplePipe)
    {
    public:
        TEST_METHOD(Constants)
        {
            Assert::AreEqual(MAX_DATA_SIZE, static_cast<size_t>((std::numeric_limits<DWORD>::max)() - static_cast<DWORD>(sizeof SimpleNamedPipeBase::Header)));
            Assert::AreEqual(TypicalSimpleNamedPipeServer::BUFFER_SIZE, TYPICAL_BUFFER_SIZE);
            Assert::AreEqual(TypicalSimpleNamedPipeClient::BUFFER_SIZE, TYPICAL_BUFFER_SIZE);
        }

        TEST_METHOD(HelloEcho)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
                    break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(server.PipeName().c_str()));

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
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
            Assert::AreEqual(WC(), serverConnected.wait(1000));

            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(client.PipeName().c_str()));

            WCHAR hello[] = L"HELLO WORLD!";

            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            Assert::AreEqual(WC(), echoComplete.wait(1000));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));

            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::AreEqual(std::wstring(L"echo: HELLO WORLD!"), echoMessage);
        }

        void HelloNtimes(const ULONG repeat)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
                    break;
                }
            });

            auto remain = repeat;

            std::vector<std::wstring> actualValues;
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter echoComplete;
            EventCounter clientDisconnected;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
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

            Assert::AreEqual(WC(), serverConnected.wait(1000));
            Assert::AreEqual(WC(), echoComplete.wait(1000));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));

            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

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
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            std::wstring echoMessage;
            for (auto i = 0ul; i < repeat; ++i) {
                EventCounter echoComplete;
                EventCounter clientDisconnected;
                TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                    switch (param.type) {
                    case PipeEventType::DISCONNECTED:
                        clientDisconnected.set();
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
                Assert::AreEqual(WC(), serverConnected.wait(1000));

                std::wstringstream oss;
                oss << L"HELLO WORLD![" << i << "]";
                auto message = oss.str();

                client.WriteAsync(&message[0], message.size() * sizeof(WCHAR)).wait();

                Assert::AreEqual(WC(), echoComplete.wait(1000));
                client.Close();
                Assert::AreEqual(WC(), clientDisconnected.wait(1000));
                try {
                    serverErrTask.wait();
                    clientErrTask.wait();
                }
                catch (winrt::hresult_error& ex)
                {
                    Assert::Fail(hresultToStr(ex).c_str());
                }
                catch (std::exception& ex)
                {
                    Assert::Fail(winrt::to_hstring(ex.what()).c_str());
                }
                Assert::AreEqual(std::wstring(L"echo: ") + message, echoMessage);

                serverConnected.reset();
                echoComplete.reset();
                serverDisconnected.reset();
            }

            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));
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
            EventCounter serverDisconnected;
            EventCounter serverClosed;
            EventCounter connectedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    connectedEvent.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            {
                TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                    switch (param.type) {
                    case PipeEventType::DISCONNECTED:
                        clientDisconnected.set();
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

                Assert::AreEqual(WC(), connectedEvent.wait(1000));

                WCHAR hello[] = L"HELLO WORLD![1]";

                try {
                    client.WriteAsync(&hello[0], sizeof(hello)).wait();
                }
                catch (winrt::hresult_error& ex) {
                    Assert::Fail(hresultToStr(ex).c_str());
                }

                Assert::AreEqual(WC(), echoComplete.wait(1000));
                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![1]"), echoMessage);

                Assert::AreEqual(WC(), serverDisconnected.wait(1000));
                client.Close();
                Assert::AreEqual(WC(), clientDisconnected.wait(1000));

                serverErrTask.wait();
                clientErrTask.wait();
            }

            connectedEvent.reset();
            echoComplete.reset();
            clientDisconnected.reset();
            serverDisconnected.reset();

            //切断後も再接続できるか？
            {
                TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                    switch (param.type) {
                    case PipeEventType::DISCONNECTED:
                        clientDisconnected.set();
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
                try {
                    client.WriteAsync(&hello[0], sizeof(hello)).wait();
                }
                catch (winrt::hresult_error& ex) {
                    Assert::Fail(hresultToStr(ex).c_str());
                }

                Assert::AreEqual(WC(), echoComplete.wait(1000));
                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![2]"), echoMessage);

                Assert::AreEqual(WC(), serverDisconnected.wait(1000));
                client.Close();
                Assert::AreEqual(WC(), clientDisconnected.wait(1000));

                serverErrTask.wait();
                clientErrTask.wait();
            }
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));
        }

        TEST_METHOD(WriteCancel)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());
            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;
            constexpr size_t BUFFER_SIZE = 512;

            std::vector<int> expected(512*4);
            for (int i = 0; i < expected.size(); ++i) {
                expected[i] = std::rand();
            }
            std::vector<int> actual;
            actual.reserve(expected.size());

            SimpleNamedPipeServer<BUFFER_SIZE> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                    //エコーバック
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                    break;
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    auto p = reinterpret_cast<const int*>(param.readBuffer);
                    actual.insert(actual.end(), p, p + (param.readedSize / sizeof(int)));
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

            Assert::AreEqual(WC(), serverConnected.wait(1000));

            concurrency::cancellation_token_source cts;
            client.onWritePacket = [&]() {
                //送信途中でキャンセルする
                cts.cancel();
            };

            Assert::ExpectException<concurrency::task_canceled>([&]() {
                client.WriteAsync(&expected[0], expected.size() * sizeof(int), cts.get_token()).get();
            });

            //0.1秒待機して受信データが来なければキャンセル成功
            Assert::AreEqual(WC(0,true), echoComplete.wait(100));

            //キャンセル後に送信可能かチェック
            client.onWritePacket = nullptr;

            client.WriteAsync(&expected[0], expected.size() * sizeof(int)).wait();
            Assert::AreEqual(WC(), echoComplete.wait(1000));
            Assert::IsTrue(expected == actual);

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));

            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();

        }

        TEST_METHOD(WriteCancelImmediate)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            constexpr size_t BUFFER_SIZE = 512;

            SimpleNamedPipeServer<BUFFER_SIZE> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                    break;
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::vector<int> actual;

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    auto p = reinterpret_cast<const int*>(param.readBuffer);
                    actual.insert(actual.end(), p, p + (param.readedSize / sizeof(int)));
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
            Assert::AreEqual(WC(), serverConnected.wait(1000));

            constexpr size_t SAMPLE_SIZE = BUFFER_SIZE ;
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

            //1秒待機して受信データが来なければキャンセル成功
            Assert::AreEqual(WC(0, true), echoComplete.wait(100));

            //キャンセル後に通信可能かチェック
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();

            Assert::AreEqual(WC(), echoComplete.wait(1000));
            Assert::IsTrue(std::equal(expected.get(), expected.get() + SAMPLE_SIZE, actual.begin(), actual.end()));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();
        }

        TEST_METHOD(TooLongWriteSize)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
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
            Assert::AreEqual(WC(), serverConnected.wait(1000));

            WCHAR dummy[] = L"DUMMY";

            Assert::ExpectException<std::length_error>([&]() {
                client.WriteAsync(&dummy[0], MAX_DATA_SIZE + 1).wait();
            });
            Assert::AreEqual(WC(0,true), echoComplete.wait(100));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));
        }

        TEST_METHOD(OverbufferTransfer)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            constexpr size_t BUFFER_SIZE = 1024;
            constexpr size_t SAMPLE_SIZE = 1024;
            auto expected = std::make_unique<int[]>(SAMPLE_SIZE);
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }

            SimpleNamedPipeServer<BUFFER_SIZE> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    //そのままか返す
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                }
                break;
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;

            auto actual = std::make_unique<int[]>(SAMPLE_SIZE);
            constexpr size_t SAMPLE_BYTE_SIZE = SAMPLE_SIZE * sizeof(int);

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
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
            Assert::AreEqual(WC(), serverConnected.wait(1000));

            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();
            Assert::AreEqual(WC(), echoComplete.wait(1000));
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            echoComplete.reset();

            //もう1回
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();
            Assert::AreEqual(WC(), echoComplete.wait(1000));
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));

            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();
        }

        TEST_METHOD(MultiWrite)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                }
                break;
                case PipeEventType::CLOSED:
                    serverClosed.set();
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
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::vector<std::wstring> actual;

            constexpr ULONG REPEAT = 20;
            auto remain = REPEAT;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                    {
                        std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                        actual.emplace_back(m);;
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

            Assert::AreEqual(WC(), serverConnected.wait(1000));

            std::vector<std::wstring> expected;
            for (auto i = 0ul; i < REPEAT; ++i) {
                std::wostringstream oss;
                oss << L"HELLO WORLD! [" << std::setw(2) << i << L"]";
                expected.emplace_back(oss.str());
            }

            concurrency::parallel_for_each(expected.begin(), expected.end(), [&](std::wstring m) {
                client.WriteAsync(m.c_str(), m.size() * sizeof(TCHAR)).wait();
            });

            Assert::AreEqual(WC(), echoComplete.wait(1000));

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));

            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();
            std::sort(actual.begin(), actual.end());
            Assert::IsTrue(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
        }

        BEGIN_TEST_METHOD_ATTRIBUTE(TransferMaxDataSize)
            TEST_PRIORITY(2)
        END_TEST_METHOD_ATTRIBUTE()
        TEST_METHOD(TransferMaxDataSize)
        {
            if (4 >= sizeof(INT_PTR)) {
                //32bitプラットフォームではサイズが大きすぎて実行できないので何もしない
                return;
            }

            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverConnected;
            EventCounter serverDisconnected;
            EventCounter serverClosed;
            EventCounter errEvent;

            using DataVector = std::vector<unsigned int>;

            auto dataCount = MAX_DATA_SIZE / sizeof(DataVector::value_type);
            auto expected = DataVector(dataCount);
            int cnt = 0;
            std::generate(expected.begin(), expected.end(), [&cnt]() {return cnt++; });

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    serverConnected.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                }
                break;
                case PipeEventType::CLOSED:
                    serverClosed.set();
                    break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    errEvent.set();
                    break;
                }
            });

            auto actual = DataVector(dataCount);
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter echoComplete;
            EventCounter clientDisconnected;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    if (param.readedSize == expected.size() * sizeof(DataVector::value_type)) {
                        auto p = reinterpret_cast<const DataVector::value_type*>(param.readBuffer);
                        std::copy(p, p + expected.size(), actual.begin());
                    }
                    echoComplete.set();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    errEvent.set();
                    break;
                }
            });

            Assert::AreEqual(WC(), serverConnected.wait(1000));

            client.WriteAsync(&expected[0], expected.size() * sizeof(DataVector::value_type)).wait();
            {
                concurrency::event* waitEvents[] = { &errEvent.evt, &echoComplete.evt };
                auto idx = concurrency::event::wait_for_multiple(&waitEvents[0], _countof(waitEvents), false, 180 * 1000);

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == idx) {
                    Assert::Fail(L"'timeout: client.WriteAsync");
                }
                if (waitEvents[idx] == &errEvent.evt) {
                    try {
                        clientErrTask.wait();
                    }
                    catch (winrt::hresult_error& ex) {
                        Assert::Fail(ex.message().c_str());
                    }
                    catch (std::exception& ex) {
                        Assert::Fail(winrt::to_hstring(ex.what()).c_str());
                    }
                    catch (...) {
                        Assert::Fail(L"error!");
                    }
                }
            }

            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server.Close();
            Assert::AreEqual(WC(), serverClosed.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::IsTrue(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
        }

        TEST_METHOD(WatcherTaskException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverErrEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    throw std::exception("server exception");
                case PipeEventType::RECEIVED:
                {
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    serverErrEvent.set();
                    break;
                }
            });
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter clientErrEvent;
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    throw std::exception("client exception");
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    clientErrEvent.set();
                    break;
                }
            });

            WCHAR hello[] = L"HELLO ERROR!";
            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            concurrency::event* events[] {&serverErrEvent.evt, &clientErrEvent.evt };
            auto res = concurrency::event::wait_for_multiple(&events[0], _countof(events), true, 1000);
            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, res);

            try {
                serverErrTask.wait();
                Assert::Fail();
            }
            catch (std::exception& ex) {
                Assert::AreEqual(std::string("server exception"), std::string(ex.what()));
            }
            try {
                clientErrTask.wait();
                Assert::Fail();
            }
            catch (std::exception& ex) {
                Assert::AreEqual(std::string("client exception"), std::string(ex.what()));
            }
            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));

            server.Close();
        }

        TEST_METHOD(LimitSizeException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverErrEvent;

            SimpleNamedPipeServer<1024,8> server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    break;
                case PipeEventType::RECEIVED:
                {
                    ps.WriteAsync(param.readBuffer, param.readedSize).wait();
                }
                break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    serverErrEvent.set();
                    break;
                }
            });
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            SimpleNamedPipeClient<1024,18> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
                    break;
                case PipeEventType::RECEIVED:
                    break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        clientErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            {
                WCHAR data[] = L"0123456789";
                Assert::ExpectException<std::length_error>([&] {
                    client.WriteAsync(&data[0], sizeof(data)).wait();
                });
            }
            {
                WCHAR data[] = L"01234567";
                try {
                    client.WriteAsync(&data[0], sizeof(data)).wait();
                }
                catch (winrt::hresult_error& ex) {
                    Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_NO_DATA)), static_cast<int>(ex.code()));
                }
            }
            Assert::AreEqual(WC(), serverErrEvent.wait(1000));
            try {
                serverErrTask.wait();
            }
            catch (std::length_error& )
            {}
            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));

            server.Close();
        }

        TEST_METHOD(CreateException)
        {
            auto pipeName1 = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            EventCounter serverDisconnected;
            EventCounter clientDisconnected;

            TypicalSimpleNamedPipeServer server1(pipeName1.c_str(), nullptr, [&](auto& ps, const auto& param)
                {
                    if (param.type == PipeEventType::DISCONNECTED) { serverDisconnected.set(); }
                });
            Assert::ExpectException<winrt::hresult_error>([&]() {
                //同名のパイプが既に存在する
                TypicalSimpleNamedPipeServer server2(pipeName1.c_str(), nullptr, [&](auto& ps, const auto& param) {});
            });

            TypicalSimpleNamedPipeClient client1(pipeName1.c_str(), [&](auto& ps, const auto& param)
                {
                    if (param.type == PipeEventType::DISCONNECTED) { clientDisconnected.set(); }
                });
            Assert::ExpectException<winrt::hresult_error>([&]() {
                //クライアントは同時に1つのみ
                TypicalSimpleNamedPipeClient client2(pipeName1.c_str(), [&](auto& ps, const auto& param) {});
            });

            auto pipeName2 = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());
            Assert::ExpectException<winrt::hresult_error>([&]() {
                //存在しないパイプに接続
                TypicalSimpleNamedPipeClient client3(pipeName2.c_str(), [&](auto& ps, const auto& param) {});
            });

            client1.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            server1.Close();
        }

        TEST_METHOD(UnreachedException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter connectedEvent;
            EventCounter disconnectedEvent;
            EventCounter clientDisconnected;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    connectedEvent.set();
                    break;
                case PipeEventType::DISCONNECTED:
                    disconnectedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                    break;
                }
            });

            //未接続の状態で送信
            WCHAR hello[] = L"HELLO WORLD!";
            Assert::ExpectException<winrt::hresult_error>([&]() {
                server.WriteAsync(&hello[0], sizeof(hello)).wait();
            });

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                if (param.type == PipeEventType::DISCONNECTED) { clientDisconnected.set(); }
                });
            Assert::AreEqual(WC(), connectedEvent.wait(1000));

            client.WriteAsync(&hello[0], sizeof(hello)).wait();
            client.Close();
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));
            Assert::AreEqual(WC(), disconnectedEvent.wait(1000));
            //切断後に送信（クライアント）
            Assert::ExpectException<winrt::hresult_error>([&]() {
                client.WriteAsync(&hello[0], sizeof(hello)).wait();
            });

            //切断後に送信（サーバー）
            Assert::ExpectException<winrt::hresult_error>([&]() {
                server.WriteAsync(&hello[0], sizeof(hello)).wait();
            });

            server.Close();
        }

        TEST_METHOD(ServerShutdown)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            EventCounter serverDisconnected;
            EventCounter serverClosed;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    serverDisconnected.set();
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
                case PipeEventType::CLOSED:
                    serverClosed.set();
                    break;
                case PipeEventType::EXCEPTION:
                    //監視タスクで例外発生
                    if (param.errTask) {
                        serverErrTask = param.errTask.value();
                    }
                    break;
                }
            });

            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(server.PipeName().c_str()));

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            EventCounter echoComplete;
            EventCounter clientDisconnected;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
                    clientDisconnected.set();
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
            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(client.PipeName().c_str()));

            WCHAR hello[] = L"HELLO WORLD!";

            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            Assert::AreEqual(WC(), echoComplete.wait(1000));

            server.Close();
            Assert::AreEqual(WC(), serverDisconnected.wait(1000));
            Assert::AreEqual(WC(), serverClosed.wait(1000));
            Assert::AreEqual(WC(), clientDisconnected.wait(1000));

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::AreEqual(std::wstring(L"echo: HELLO WORLD!"), echoMessage);
        }
    };
}
