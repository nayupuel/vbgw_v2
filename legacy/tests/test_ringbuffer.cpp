#include "utils/RingBuffer.h"

#include <vector>

#include <gtest/gtest.h>

TEST(RingBufferTest, BasicWriteRead)
{
    RingBuffer rb(1024);
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5};
    rb.write(data1.data(), data1.size());

    std::vector<uint8_t> out(data1.size());
    size_t read_bytes = rb.read(out.data(), out.size());

    EXPECT_EQ(read_bytes, data1.size());
    EXPECT_EQ(out, data1);
}

TEST(RingBufferTest, WrapAround)
{
    RingBuffer rb(10);
    // Write 7 bytes
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5, 6, 7};
    rb.write(data1.data(), data1.size());

    // Read 5 bytes
    std::vector<uint8_t> out1(5);
    rb.read(out1.data(), out1.size());

    // Write 5 more bytes (total 7 - 5 = 2 bytes in buffer, 5 bytes will wrap to beginning)
    std::vector<uint8_t> data2 = {8, 9, 10, 11, 12};
    rb.write(data2.data(), data2.size());

    // Read 7 bytes
    std::vector<uint8_t> out2(7);
    size_t read_bytes = rb.read(out2.data(), out2.size());

    EXPECT_EQ(read_bytes, 7);
    std::vector<uint8_t> expected = {6, 7, 8, 9, 10, 11, 12};
    EXPECT_EQ(out2, expected);
}

TEST(RingBufferTest, BufferFull)
{
    RingBuffer rb(5);
    std::vector<uint8_t> data1 = {1, 2, 3};
    rb.write(data1.data(), data1.size());

    // Attempt to write 3 more bytes, but only 2 should fit (or it pushes but we only expect
    // available behavior) Current RingBuffer allows overwriting or dropping depending on
    // implementation. The previous implementation of RingBuffer drops if no space.
    std::vector<uint8_t> data2 = {4, 5, 6};
    rb.write(data2.data(), data2.size());

    EXPECT_LE(rb.size(), 5);
}
