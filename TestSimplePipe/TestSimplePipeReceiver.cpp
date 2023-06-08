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

    //テスト用のパケットヘルパー共用体
    template<size_t N>
    union TestPacket {
        struct {
            alignas(4) DWORD size;
            WCHAR data[N];
        };
        Packet p;
    };

    //テスト用パケット生成ヘルパー
    template<size_t N>
    TestPacket<N> CreatePacket(const std::wstring msg)
    {
        if (msg.size() != N) {
            throw std::length_error("unmatch size");
        }
        TestPacket<N> p;
        p.size = static_cast<DWORD>((N * sizeof(WCHAR)) + sizeof(Packet::size));
        std::copy(msg.begin(), msg.end(), p.data);
        return p;
    }

    //パケットからメッセージの取り出し
    std::wstring UnpackMsg(LPCVOID p, size_t s)
    {
        return std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR));
    }

    //abt::comm::simple_pipe::Receiverテストクラス
    TEST_CLASS(TestReceiver)
    {
    public:

        //1受信バッファーに1つのパケット
        TEST_METHOD(SinglePacket)
        {
            std::wstring expected = L"ABCDE";
            auto testPacket = CreatePacket<5>(expected);
            std::wstring actual;
            Receiver receiver(1024, [&](const auto p, auto s) {
                actual = UnpackMsg(p, s);
            });
            receiver.Feed(&testPacket, 14);

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
            size_t totalSize = testPackets[0].size * _countof(testPackets);
            std::unique_ptr<BYTE[]> buffer = std::make_unique<BYTE[]>(totalSize);
            BYTE* dst = buffer.get();
            for (const auto& p : testPackets) {
                auto len = p.size;
                memcpy(dst, &p, len);
                dst += len;
                expectedValues.emplace_back(std::wstring(p.data, 0, _countof(p.data)));
            }

            std::vector<std::wstring> actualValues;
            Receiver receiver(1024, [&](const auto p, auto s) {
                actualValues.emplace_back(UnpackMsg(p, s));
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
            Receiver receiver(1024, [&](const auto p, auto s) {
                actual = std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR));
            });

            auto remain = packet.size;
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


            auto totalSize = packet1.size + packet2.size;
            auto buffer = std::make_unique<BYTE[]>(totalSize);
            memcpy(&buffer[0], &packet1.p, packet1.size);
            memcpy(&buffer[packet1.size], &packet2.p, packet2.size);

            std::vector<std::wstring> acutals;
            Receiver receiver(1024, [&](const auto p, auto s) {
                acutals.emplace_back(std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR)));
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


            auto totalSize = packet1.size + packet2.size + packet3.size + packet4.size + packet5.size;
            auto buffer = std::make_unique<BYTE[]>(totalSize);
            auto p = &buffer[0];
            memcpy(p, &packet1.p, packet1.size); p += packet1.size;
            memcpy(p, &packet2.p, packet2.size); p += packet2.size;
            memcpy(p, &packet3.p, packet3.size); p += packet3.size;
            memcpy(p, &packet4.p, packet4.size); p += packet4.size;
            memcpy(p, &packet5.p, packet5.size);

            std::vector<std::wstring> acutals;
            Receiver receiver(1024, [&](const auto p, auto s) {
                acutals.emplace_back(std::wstring(reinterpret_cast<LPCWSTR>(p), 0, s / sizeof(WCHAR)));
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
    };
}
