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
    using namespace abt::comm::simple_pipe;

    std::wstring hresultToStr(const winrt::hresult_error& e) {
        std::wostringstream oss;
        oss << L"HRESULT: " << std::hex << e.code() << " " << e.message().c_str();
        return oss.str();
    }

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

            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(server.PipeName().c_str()));

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
            Assert::AreEqual(std::wstring(pipeName.c_str()), std::wstring(client.PipeName().c_str()));

            WCHAR hello[] = L"HELLO WORLD!";

            client.WriteAsync(&hello[0], sizeof(hello)).wait();

            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
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

            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
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

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                    Assert::Fail(L"echoComplete.wait timeout");
                }

                client.Close();
                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                    Assert::Fail(L"closedEvent.wait timeout");
                }
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
            concurrency::event connectedEvent;

            int servcerConnectCnt = 0;
            int servcerDisconnectCnt = 0;
            int serverWritedCnt1 = 0;
            int serverWritedCnt2 = 0;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    connectedEvent.set();
                    ++servcerConnectCnt;
                    break;
                case PipeEventType::DISCONNECTED:
                    ++servcerDisconnectCnt;
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                {
                    std::wstring m(reinterpret_cast<LPCWSTR>(param.readBuffer), 0, param.readedSize / sizeof(WCHAR));
                    std::wostringstream oss;
                    oss << L"echo: " << m;
                    std::wstring echoMessage = oss.str();
                    ++serverWritedCnt1;
                    ps.WriteAsync(&echoMessage[0], echoMessage.size() * sizeof(WCHAR)).wait();
                    ++serverWritedCnt2;
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

                try {
                    client.WriteAsync(&hello[0], sizeof(hello)).wait();
                }
                catch (winrt::hresult_error& ex) {
                    Assert::Fail(hresultToStr(ex).c_str());
                }

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                    Assert::Fail(L"echoComplete is timeout[1]");
                }

                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![1]"), echoMessage);

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                    Assert::Fail(L"closedEvent is timeout[1]");
                    Assert::Fail();
                }

                serverErrTask.wait();
                clientErrTask.wait();
            }

            connectedEvent.reset();
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
                try {
                    client.WriteAsync(&hello[0], sizeof(hello)).wait();
                }
                catch (winrt::hresult_error& ex) {
                    Assert::Fail(hresultToStr(ex).c_str());
                }

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                    Assert::Fail(L"echoComplete is timeout[2]");
                }

                Assert::AreEqual(std::wstring(L"echo: HELLO WORLD![2]"), echoMessage);

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                    Assert::Fail(L"closedEvent is timeout[2]");
                }
                serverErrTask.wait();
                clientErrTask.wait();
            }
            server.Close();
        }

        TEST_METHOD(WriteCancel)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());
            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;
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
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
                    break;
                case PipeEventType::RECEIVED:
                    //エコーバック
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

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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

            concurrency::cancellation_token_source cts;
            client.onWritePacket = [&]() {
                //送信途中でキャンセルする
                cts.cancel();
            };

            Assert::ExpectException<concurrency::task_canceled>([&]() {
                client.WriteAsync(&expected[0], expected.size() * sizeof(int), cts.get_token()).get();
            });

            //0.1秒待機して受信データが来なければキャンセル成功
            Assert::AreEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, echoComplete.wait(100));

            //キャンセル後に送信可能かチェック
            client.onWritePacket = nullptr;

            client.WriteAsync(&expected[0], expected.size() * sizeof(int)).wait();
            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, echoComplete.wait(1000));
            Assert::IsTrue(expected == actual);

            client.Close();

            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, closedEvent.wait(1000));
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();

        }

        TEST_METHOD(WriteCancelImmediate)
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
            std::vector<int> actual;

            SimpleNamedPipeClient<BUFFER_SIZE> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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
            Assert::AreEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, echoComplete.wait(1000));

            //キャンセル後に通信可能かチェック
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();

            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, echoComplete.wait(1000));
            Assert::IsTrue(std::equal(expected.get(), expected.get() + SAMPLE_SIZE, actual.begin(), actual.end()));

            client.Close();

            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
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
                client.WriteAsync(&dummy[0], MAX_DATA_SIZE + 1).wait();
            });
        }

        TEST_METHOD(OverbufferTransfer)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event closedEvent;

            constexpr size_t BUFFER_SIZE = 1024;
            constexpr size_t SAMPLE_SIZE = 1024;
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
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                Assert::Fail();
            }
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            echoComplete.reset();

            //もう1回
            for (int i = 0; i < SAMPLE_SIZE; ++i) {
                expected[i] = std::rand();
            }
            client.WriteAsync(&expected[0], SAMPLE_BYTE_SIZE).wait();
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                Assert::Fail();
            }
            Assert::AreEqual(0, memcmp(&expected[0], &actual[0], SAMPLE_BYTE_SIZE));

            client.Close();
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                Assert::Fail();
            }

            serverErrTask.wait();
            clientErrTask.wait();
        }

        TEST_METHOD(MultiWrite)
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

            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;
            std::vector<std::wstring> actual;

            constexpr ULONG REPEAT = 20;
            auto remain = REPEAT;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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

            std::vector<std::wstring> expected;
            for (auto i = 0ul; i < REPEAT; ++i) {
                std::wostringstream oss;
                oss << L"HELLO WORLD! [" << std::setw(2) << i << L"]";
                expected.emplace_back(oss.str());
            }

            concurrency::parallel_for_each(expected.begin(), expected.end(), [&](std::wstring m) {
                client.WriteAsync(m.c_str(), m.size() * sizeof(TCHAR)).wait();
            });

            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == echoComplete.wait(1000)) {
                Assert::Fail();
            }

            client.Close();
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

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
            concurrency::event closedEvent;
            concurrency::event errEvent;

            using DataVector = std::vector<unsigned int>;

            auto dataCount = MAX_DATA_SIZE / sizeof(DataVector::value_type);
            auto expected = DataVector(dataCount);
            int cnt = 0;
            std::generate(expected.begin(), expected.end(), [&cnt]() {return cnt++; });

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
                    break;
                case PipeEventType::DISCONNECTED:
                    closedEvent.set();
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
                    errEvent.set();
                    break;
                }
            });

            auto actual = DataVector(dataCount);
            concurrency::task<void> clientErrTask = concurrency::task_from_result();
            concurrency::event echoComplete;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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

            client.WriteAsync(&expected[0], expected.size() * sizeof(DataVector::value_type)).wait();
            {
                concurrency::event* waitEvents[] = { &errEvent, &echoComplete };
                auto idx = concurrency::event::wait_for_multiple(&waitEvents[0], _countof(waitEvents), false, 180 * 1000);

                if (concurrency::COOPERATIVE_WAIT_TIMEOUT == idx) {
                    Assert::Fail(L"'timeout: client.WriteAsync");
                }
                if (waitEvents[idx] == &errEvent) {
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
            if (concurrency::COOPERATIVE_WAIT_TIMEOUT == closedEvent.wait(1000)) {
                Assert::Fail();
            }
            server.Close();

            serverErrTask.wait();
            clientErrTask.wait();

            Assert::IsTrue(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
        }

        TEST_METHOD(WatcherTaskException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event serverErrEvent;

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
            concurrency::event clientErrEvent;
            std::wstring echoMessage;

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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

            concurrency::event* events[] {&serverErrEvent, &clientErrEvent};
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
        }

        TEST_METHOD(LimitSizeException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event serverErrEvent;

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
            std::wstring echoMessage;

            SimpleNamedPipeClient<1024,18> client(pipeName.c_str(), [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::DISCONNECTED:
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
            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, serverErrEvent.wait(1000));
            try {
                serverErrTask.wait();
            }
            catch (std::length_error& )
            {}
        }

        TEST_METHOD(CreateException)
        {
            auto pipeName1 = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event disconnectedEvent;

            TypicalSimpleNamedPipeServer server1(pipeName1.c_str(), nullptr, [&](auto& ps, const auto& param) {});
            try {
                //同名のパイプが既に存在する
                TypicalSimpleNamedPipeServer server2(pipeName1.c_str(), nullptr, [&](auto& ps, const auto& param) {});
                Assert::Fail();
            }
            catch (winrt::hresult_error& ex) {
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_PIPE_BUSY)), static_cast<int>(ex.code()));
            }

            TypicalSimpleNamedPipeClient client1(pipeName1.c_str(), [&](auto& ps, const auto& param) {});
            try {
                TypicalSimpleNamedPipeClient client2(pipeName1.c_str(), [&](auto& ps, const auto& param) {});
                Assert::Fail();
            }
            catch (winrt::hresult_error& ex) {
                //クライアントは同時に1つのみ
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT)), static_cast<int>(ex.code()));
            }

            auto pipeName2 = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());
            try {
                TypicalSimpleNamedPipeClient client3(pipeName2.c_str(), [&](auto& ps, const auto& param) {});
                Assert::Fail();
            }
            catch (winrt::hresult_error& ex) {
                //存在しないパイプに接続
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)), static_cast<int>(ex.code()));
            }

        }

        TEST_METHOD(UnreachedException)
        {
            auto pipeName = std::wstring(L"\\\\.\\pipe\\") + winrt::to_hstring(winrt::Windows::Foundation::GuidHelper::CreateNewGuid());

            concurrency::task<void> serverErrTask = concurrency::task_from_result();
            concurrency::event disconnectedEvent;

            TypicalSimpleNamedPipeServer server(pipeName.c_str(), nullptr, [&](auto& ps, const auto& param) {
                switch (param.type) {
                case PipeEventType::CONNECTED:
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
            try
            {
                server.WriteAsync(&hello[0], sizeof(hello)).wait();
                Assert::Fail();
            }
            catch(winrt::hresult_error ex){
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_PIPE_LISTENING)), static_cast<int>(ex.code()));
            }

            TypicalSimpleNamedPipeClient client(pipeName.c_str(), [&](auto& ps, const auto& param) {});
            client.WriteAsync(&hello[0], sizeof(hello)).wait();
            client.Close();
            //切断後に送信（クライアント）
            try
            {
                client.WriteAsync(&hello[0], sizeof(hello)).wait();
                Assert::Fail();
            }
            catch (winrt::hresult_error ex) {
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)), static_cast<int>(ex.code()));
            }

            Assert::AreNotEqual(concurrency::COOPERATIVE_WAIT_TIMEOUT, disconnectedEvent.wait(1000));
            //切断後に送信（サーバー）
            try
            {
                server.WriteAsync(&hello[0], sizeof(hello)).wait();
                Assert::Fail();
            }
            catch (winrt::hresult_error ex) {
                Assert::AreEqual(static_cast<int>(HRESULT_FROM_WIN32(ERROR_PIPE_LISTENING)), static_cast<int>(ex.code()));
            }
        }
    };
}
