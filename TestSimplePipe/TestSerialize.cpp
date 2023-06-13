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

namespace abt::comm::simple_pipe::test::serialize
{
    using namespace abt::comm::simple_pipe;

    std::wstring StrFromBuffer(const SimpleNamedPipeBase::Buffer& buffer)
    {
        return std::wstring(reinterpret_cast<LPCWSTR>(buffer.Pointer()), 0, buffer.Size() / sizeof(WCHAR));
    }

    TEST_CLASS(TestSerializer)
    {
        TEST_METHOD(Serialize)
        {
            TCHAR testData[] {L"ABCDEFGHIJKLMNOPQRSTUBWXYZ"};
            constexpr DWORD splitSize = 10 * sizeof(WCHAR);
            SimpleNamedPipeBase::Serializer serializer(SimpleNamedPipeBase::Buffer(testData,sizeof(testData) - sizeof(WCHAR)), splitSize);
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsFalse(buffer.Empty());
                Assert::AreEqual(std::wstring(L"ABCDEFGHIJ"), StrFromBuffer(buffer));
                Assert::IsTrue(header.info.startBit);
                Assert::IsFalse(header.info.endBit);
                Assert::IsFalse(header.info.cancelBit);
                Assert::AreEqual(static_cast<size_t>(header.size), SimpleNamedPipeBase::HeaderSize + splitSize);
                Assert::AreEqual(static_cast<size_t>(header.DataOffset()), SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(static_cast<size_t>(splitSize), header.DataSize());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsFalse(buffer.Empty());
                Assert::AreEqual(std::wstring(L"KLMNOPQRST"), StrFromBuffer(buffer));
                Assert::IsFalse(header.info.startBit);
                Assert::IsFalse(header.info.endBit);
                Assert::IsFalse(header.info.cancelBit);
                Assert::AreEqual(static_cast<size_t>(header.size), SimpleNamedPipeBase::HeaderSize + splitSize);
                Assert::AreEqual(static_cast<size_t>(header.DataOffset()), SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(static_cast<size_t>(splitSize), header.DataSize());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsFalse(buffer.Empty());
                Assert::AreEqual(std::wstring(L"UBWXYZ"), StrFromBuffer(buffer));
                Assert::IsFalse(header.info.startBit);
                Assert::IsTrue(header.info.endBit);
                Assert::IsFalse(header.info.cancelBit);
                Assert::AreEqual(static_cast<size_t>(header.size), SimpleNamedPipeBase::HeaderSize + 6 * sizeof(WCHAR));
                Assert::AreEqual(static_cast<size_t>(header.DataOffset()), SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(static_cast<size_t>(6 * sizeof(WCHAR)), header.DataSize());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
        }
        TEST_METHOD(SerializeSingle)
        {
            TCHAR testData[]{ L"ABCDEFGHIJKLMNOPQRSTUBWXYZ" };
            constexpr DWORD splitSize = 26 * sizeof(WCHAR);
            SimpleNamedPipeBase::Serializer serializer(SimpleNamedPipeBase::Buffer(testData, sizeof(testData) - sizeof(WCHAR)), splitSize);
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsFalse(buffer.Empty());
                Assert::AreEqual(std::wstring(L"ABCDEFGHIJKLMNOPQRSTUBWXYZ"), StrFromBuffer(buffer));
                Assert::IsTrue(header.info.startBit);
                Assert::IsTrue(header.info.endBit);
                Assert::IsFalse(header.info.cancelBit);
                Assert::AreEqual(static_cast<size_t>(header.size), SimpleNamedPipeBase::HeaderSize + splitSize);
                Assert::AreEqual(static_cast<size_t>(header.DataOffset()), SimpleNamedPipeBase::HeaderSize);
                Assert::AreEqual(static_cast<size_t>(splitSize), header.DataSize());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
        }
        TEST_METHOD(SerializeEmpty)
        {
            SimpleNamedPipeBase::Serializer serializer(SimpleNamedPipeBase::Buffer(nullptr, 0), 10);
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
            {
                auto [buffer, header] = serializer.Next();
                Assert::IsTrue(buffer.Empty());
            }
        }
    };

    class PacketBuidler
    {
    private:
        SimpleNamedPipeBase::Buffer buffer;
        const DWORD splitSize;
        bool beginning{ true };
        std::vector<BYTE> work;
    public:
        PacketBuidler(SimpleNamedPipeBase::Buffer buffer, DWORD splitSize)
            : buffer(buffer)
            , splitSize(splitSize)
        {}
        const SimpleNamedPipeBase::Packet* Next()
        {
            if (buffer.Empty()) {
                return nullptr;
            }
            work.clear();
            auto size = (std::min)(static_cast<size_t>(splitSize), buffer.Size());
            auto comsumed = buffer.Consume(size);
            auto header = SimpleNamedPipeBase::Header::Create(static_cast<DWORD>(size), beginning, buffer.Empty());
            auto p = reinterpret_cast<const BYTE*>(&header);
            work.insert(work.end(), p, p + sizeof(header));
            work.insert(work.end(), comsumed.Begin(), comsumed.End());
            beginning = false;
            return reinterpret_cast<const SimpleNamedPipeBase::Packet*>(&work[0]);
        }
    };

    TEST_CLASS(TestDeerializer)
    {
        TEST_METHOD(Deserialize)
        {
            TCHAR testData1[]{ L"ABCDEFGHIJKLMNOPQRSTUVWXYZ" };
            SimpleNamedPipeBase::Buffer testBuffer1(testData1, sizeof(testData1) - sizeof(WCHAR));
            TCHAR testData2[]{ L"abcdefghijklmnopqrstuvwxyz" };
            SimpleNamedPipeBase::Buffer testBuffer2(testData2, sizeof(testData2) - sizeof(WCHAR));

            int progress = 1;

            SimpleNamedPipeBase::Deserializer deserializer(1024, 1024, [&](auto buf) {
                if (progress == 1) {
                    Assert::AreEqual(std::wstring(testData1), StrFromBuffer(buf));
                    progress = 2;
                }
                else if (progress == 2) {
                    Assert::AreEqual(std::wstring(testData2), StrFromBuffer(buf));
                    progress = 3;
                }
            });;
            PacketBuidler builder1(testBuffer1, 10 * sizeof(WCHAR));
            Assert::IsTrue(deserializer.Feed(builder1.Next()));
            Assert::IsTrue(deserializer.Feed(builder1.Next()));
            Assert::IsTrue(deserializer.Feed(builder1.Next()));

            PacketBuidler builder2(testBuffer2, 10 * sizeof(WCHAR));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));

            Assert::AreEqual(3, progress);
        }

        TEST_METHOD(DeserializeSingle)
        {
            TCHAR testData1[]{ L"ABCDEFGHIJKLMNOPQRSTUVWXYZ" };
            SimpleNamedPipeBase::Buffer testBuffer1(testData1, sizeof(testData1) - sizeof(WCHAR));
            TCHAR testData2[]{ L"abcdefghijklmnopqrstuvwxyz" };
            SimpleNamedPipeBase::Buffer testBuffer2(testData2, sizeof(testData2) - sizeof(WCHAR));

            int progress = 1;

            SimpleNamedPipeBase::Deserializer deserializer(1024, 1024, [&](auto buf) {
                if (progress == 1) {
                    Assert::AreEqual(std::wstring(testData1), StrFromBuffer(buf));
                    progress = 2;
                }
                else if (progress == 2) {
                    Assert::AreEqual(std::wstring(testData2), StrFromBuffer(buf));
                    progress = 3;
                }
            });;
            PacketBuidler builder1(testBuffer1, static_cast<DWORD>(testBuffer1.Size()));
            Assert::IsTrue(deserializer.Feed(builder1.Next()));

            PacketBuidler builder2(testBuffer2, static_cast<DWORD>(testBuffer2.Size()));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));

            Assert::AreEqual(3, progress);
        }

        TEST_METHOD(DeserializeCancel)
        {
            TCHAR testData1[]{ L"ABCDEFGHIJKLMNOPQRSTUVWXYZ" };
            SimpleNamedPipeBase::Buffer testBuffer1(testData1, sizeof(testData1) - sizeof(WCHAR));
            TCHAR testData2[]{ L"abcdefghijklmnopqrstuvwxyz" };
            SimpleNamedPipeBase::Buffer testBuffer2(testData2, sizeof(testData2) - sizeof(WCHAR));

            int progress = 1;

            SimpleNamedPipeBase::Deserializer deserializer(1024, 1024, [&](auto buf) {
                Assert::AreNotEqual(1, progress);
                if (progress == 2) {
                    Assert::AreEqual(std::wstring(testData2), StrFromBuffer(buf));
                    progress = 3;
                }
            });;

            auto cancelHeader = SimpleNamedPipeBase::Header::CreateCancel();
            
            PacketBuidler builder1(testBuffer1, 10 * sizeof(WCHAR));
            Assert::IsTrue(deserializer.Feed(builder1.Next()));
            Assert::IsFalse(deserializer.Feed(reinterpret_cast<const SimpleNamedPipeBase::Packet*>(&cancelHeader)));

            progress = 2;

            PacketBuidler builder2(testBuffer2, 10 * sizeof(WCHAR));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));
            Assert::IsTrue(deserializer.Feed(builder2.Next()));

            Assert::AreEqual(3, progress);
        }
    };
}
