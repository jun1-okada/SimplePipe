#include "pch.h"
#include <windows.h>
#include <string>
#include <memory>
#include <numeric>
#include <algorithm>
#include <memory.h>
#include "CppUnitTest.h"
#include "../inc/SimpleNamedPipe.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace abt::comm::simple_pipe::test::receiver
{
    using namespace abt::comm::simple_pipe;

    //abt::comm::simple_pipe::SimpleNamedPipeBase::Receiverテストクラス
        //テスト用のパケットヘルパー共用体
    template<size_t N>
    union TestPacket {
        struct {
            SimpleNamedPipeBase::Header header;
            WCHAR data[N];
        };
        SimpleNamedPipeBase::Packet p;
    };

    //テスト用パケット生成ヘルパー
    template<size_t N>
    static TestPacket<N> CreatePacket(const std::wstring msg, bool startBit = true, bool endBit = true)
    {
        if (msg.size() != N) {
            throw std::length_error("unmatch size");
        }
        TestPacket<N> p;
        p.header = SimpleNamedPipeBase::Header::Create(static_cast<DWORD>((N * sizeof(WCHAR))), startBit, endBit);
        std::copy(msg.begin(), msg.end(), p.data);
        return p;
    }

    //パケットからメッセージの取り出し
    static std::wstring UnpackMsg(LPCVOID p, size_t s)
    {
        return std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR));
    }

    static std::wstring UnpackMsg(const SimpleNamedPipeBase::Buffer& buffer)
    {
        return std::wstring(reinterpret_cast<LPCWSTR>(buffer.Pointer()), 0, buffer.Size() / sizeof(WCHAR));
    }

    TEST_CLASS(TestReceiver)
    {
    public:
        TEST_METHOD(Constants)
        {
            Assert::AreEqual(sizeof(SimpleNamedPipeBase::Packet::head), 8ull);
        }

        //1受信バッファーに1つのパケット
        TEST_METHOD(SinglePacket)
        {
            std::wstring expected = L"ABCDE";
            auto testPacket = CreatePacket<5>(expected);
            std::wstring actual;
            SimpleNamedPipeBase::Receiver receiver(1024, 1024, [&](const auto packet) {
                actual = UnpackMsg(packet->Data());
            });
            receiver.Feed(&testPacket, testPacket.p.head.size);

            Assert::AreEqual(expected, actual);
        }

        //1受信バッファーに5つのパケット
        TEST_METHOD(MultiPacket)
        {
            TestPacket<5> testPackets[] = {
                CreatePacket<5>(L"ABCDE"),
                CreatePacket<5>(L"FGHIJ"),
                CreatePacket<5>(L"KLMNO"),
                CreatePacket<5>(L"PRSTU"),
                CreatePacket<5>(L"VWXYZ"),
            };
            std::vector<std::wstring> expectedValues;
            size_t totalSize = testPackets[0].header.size * _countof(testPackets);
            std::unique_ptr<BYTE[]> buffer = std::make_unique<BYTE[]>(totalSize);
            BYTE* dst = buffer.get();
            for (const auto& p : testPackets) {
                auto len = p.header.size;
                memcpy(dst, &p, len);
                dst += len;
                expectedValues.emplace_back(std::wstring(p.data, 0, _countof(p.data)));
            }

            std::vector<std::wstring> actualValues;
            SimpleNamedPipeBase::Receiver receiver(1024, 1024, [&](const auto packet) {
                actualValues.emplace_back(UnpackMsg(packet->Data()));
            });
            receiver.Feed(buffer.get(), totalSize);

            Assert::IsTrue(std::equal(expectedValues.begin(), expectedValues.end(), actualValues.begin(), actualValues.end()));
        }

        //5つの受信バッファーに1つのパケット
        TEST_METHOD(FragmentPacket)
        {
            auto packet = CreatePacket<15>(L"ABCDEFGHIJKLMNO");

            std::vector<BYTE> buffer;
            std::wstring actual;
            SimpleNamedPipeBase::Receiver receiver(1024, 1024, [&](const auto packet) {
                actual = UnpackMsg(packet->Data());
                //actual = std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR));
            });

            auto remain = packet.header.size;
            const BYTE* p = reinterpret_cast<const BYTE*>(&packet);
            constexpr DWORD fragmentSize = 8;
            while (remain > 0) {
                auto size = (std::min)(fragmentSize, remain);
                receiver.Feed(p, size);
                p += size;
                remain -= size;
            }
            Assert::IsTrue(std::equal(std::begin(packet.data), std::end(packet.data), actual.begin(), actual.end()));
        }

        //2つの受信バッファーの境界をまたぐ形で2つ目パケットが存在する
        TEST_METHOD(SplitHeaderPacket)
        {
            auto packet1 = CreatePacket<5>(L"ABCDE");
            auto packet2 = CreatePacket<5>(L"FGHIJ");

            auto expected = std::vector<std::wstring>{
                std::wstring(std::begin(packet1.data), std::end(packet1.data)),
                std::wstring(std::begin(packet2.data), std::end(packet2.data)),
            };


            auto totalSize = packet1.header.size + packet2.header.size;
            auto buffer = std::make_unique<BYTE[]>(totalSize);
            memcpy(&buffer[0], &packet1.p, packet1.header.size);
            memcpy(&buffer[packet1.header.size], &packet2.p, packet2.header.size);

            std::vector<std::wstring> acutals;
            SimpleNamedPipeBase::Receiver receiver(1024, 1024, [&](const auto packet) {
                acutals.emplace_back(UnpackMsg(packet->Data()));
                //acutals.emplace_back(std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR)));
            });

            const BYTE* p = &buffer[0];
            DWORD remain = totalSize;
            receiver.Feed(p, 16); p += 16; remain -= 16;
            receiver.Feed(p, 1);  p += 1; remain -= 1;
            receiver.Feed(p, remain);

            Assert::IsTrue(std::equal(expected.begin(), expected.end(), acutals.begin(), acutals.end()));
        }

        //Receiverのステータス全パターンを網羅するテスト
        TEST_METHOD(ComplexPackets)
        {
            auto packet1 = CreatePacket<5>(L"ABCDE");
            auto packet2 = CreatePacket<10>(L"FGHIJKLMNO");
            auto packet3 = CreatePacket<2>(L"PQ");
            auto packet4 = CreatePacket<2>(L"RS");
            auto packet5 = CreatePacket<7>(L"TUVWXYZ");

            auto expected = std::vector<std::wstring>{
                std::wstring(std::begin(packet1.data), std::end(packet1.data)),
                std::wstring(std::begin(packet2.data), std::end(packet2.data)),
                std::wstring(std::begin(packet3.data), std::end(packet3.data)),
                std::wstring(std::begin(packet4.data), std::end(packet4.data)),
                std::wstring(std::begin(packet5.data), std::end(packet5.data)),
            };


            auto totalSize = packet1.header.size + packet2.header.size + packet3.header.size + packet4.header.size + packet5.header.size;
            auto buffer = std::make_unique<BYTE[]>(totalSize);
            auto p = &buffer[0];
            memcpy(p, &packet1.p, packet1.header.size); p += packet1.header.size;
            memcpy(p, &packet2.p, packet2.header.size); p += packet2.header.size;
            memcpy(p, &packet3.p, packet3.header.size); p += packet3.header.size;
            memcpy(p, &packet4.p, packet4.header.size); p += packet4.header.size;
            memcpy(p, &packet5.p, packet5.header.size);

            std::vector<std::wstring> acutals;
            SimpleNamedPipeBase::Receiver receiver(1024, 1024, [&](const auto packet) {
                acutals.emplace_back(UnpackMsg(packet->Data()));
                //acutals.emplace_back(std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR)));
            });

            constexpr DWORD FEED_SIZE = 16;
            auto remain = static_cast<DWORD>(totalSize);
            p = &buffer[0];
            while (remain > 0) {
                auto size = (std::min)(remain, FEED_SIZE);
                receiver.Feed(p, size);
                remain -= size;
                p += size;
            }

            Assert::IsTrue(std::equal(expected.begin(), expected.end(), acutals.begin(), acutals.end()));
        }

        TEST_METHOD(LimitSizePckets)
        {
            std::wstring expected = L"ABCDE";
            auto testPacket = CreatePacket<5>(expected);
            std::wstring actual;
            SimpleNamedPipeBase::Receiver receiver(1024, 8, [&](const auto packet) {
                actual = UnpackMsg(packet->Data());
            });
            Assert::ExpectException<std::length_error>([&]() {
                receiver.Feed(&testPacket, testPacket.p.head.size);
            });
        }
    };

    TEST_CLASS(TestPipePacket)
    {
    public:
        TEST_METHOD(CreateHeader)
        {
            {
                auto header = SimpleNamedPipeBase::Header::Create(100, true, false);
                Assert::AreEqual(static_cast<size_t>(header.size), header.DataSize() + SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(100ull, header.DataSize());
                Assert::IsTrue(header.IsStart());
                Assert::IsFalse(header.IsEnd());
                Assert::IsFalse(header.IsCancel());
            }
            {
                auto header = SimpleNamedPipeBase::Header::Create(101, false, true);
                Assert::AreEqual(static_cast<size_t>(header.size), header.DataSize() + SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(101ull, header.DataSize());
                Assert::IsFalse(header.IsStart());
                Assert::IsTrue(header.IsEnd());
                Assert::IsFalse(header.IsCancel());
            }
            {
                auto header = SimpleNamedPipeBase::Header::Create(101, true, true);
                Assert::AreEqual(static_cast<size_t>(header.size), header.DataSize() + SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(101ull, header.DataSize());
                Assert::IsTrue(header.IsStart());
                Assert::IsTrue(header.IsEnd());
                Assert::IsFalse(header.IsCancel());
            }
            {
                auto header = SimpleNamedPipeBase::Header::Create(101, false, false);
                Assert::AreEqual(static_cast<size_t>(header.size), header.DataSize() + SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(101ull, header.DataSize());
                Assert::IsFalse(header.IsStart());
                Assert::IsFalse(header.IsEnd());
                Assert::IsFalse(header.IsCancel());
            }
            {
                auto header = SimpleNamedPipeBase::Header::CreateCancel();
                Assert::AreEqual(static_cast<size_t>(header.size), header.DataSize() + SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(0ull, header.DataSize());
                Assert::IsFalse(header.IsStart());
                Assert::IsFalse(header.IsEnd());
                Assert::IsTrue(header.IsCancel());
            }
        }

        TEST_METHOD(PacketToBuffer)
        {
            std::wstring expected = L"ABCDE";
            auto testPacket = CreatePacket<5>(expected);
            auto acutal = UnpackMsg(testPacket.p.Data());
            Assert::AreEqual(expected, acutal);
        }
    };

}
