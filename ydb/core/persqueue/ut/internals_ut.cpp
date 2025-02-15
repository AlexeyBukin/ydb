#include "blob.h"
#include <library/cpp/testing/unittest/registar.h>
#include <util/generic/size_literals.h>

namespace NKikimr::NPQ {
namespace {

Y_UNIT_TEST_SUITE(TPQTestInternal) {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TEST CASES
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


Y_UNIT_TEST(TestPartitionedBlobSimpleTest) {
    THead head;
    THead newHead;

    TPartitionedBlob blob(0, 0, "sourceId", 1, 1, 10, head, newHead, false, false, 8_MB);
    TClientBlob clientBlob("sourceId", 1, "valuevalue", TMaybe<TPartData>(), TInstant::MilliSeconds(1), TInstant::MilliSeconds(1), 0, "123", "123");
    UNIT_ASSERT(blob.IsInited());
    TString error;
    UNIT_ASSERT(blob.IsNextPart("sourceId", 1, 0, &error));

    blob.Add(std::move(clientBlob));
    UNIT_ASSERT(blob.IsComplete());
    UNIT_ASSERT(blob.GetFormedBlobs().empty());
    UNIT_ASSERT(blob.GetClientBlobs().size() == 1);
}

void Test(bool headCompacted, ui32 parts, ui32 partSize, ui32 leftInHead)
{
    TVector<TClientBlob> all;

    THead head;
    head.Offset = 100;
    TString value(100_KB, 'a');
    head.Batches.push_back(TBatch(head.Offset, 0, TVector<TClientBlob>()));
    for (ui32 i = 0; i < 50; ++i) {
        head.Batches.back().AddBlob(TClientBlob(
            "sourceId" + TString(1,'a' + rand() % 26), i + 1, value, TMaybe<TPartData>(),
            TInstant::MilliSeconds(i + 1),  TInstant::MilliSeconds(i + 1), 1, "", ""
        ));
        if (!headCompacted)
            all.push_back(head.Batches.back().Blobs.back());
    }
    head.Batches.back().Pack();
    UNIT_ASSERT(head.Batches.back().Header.GetFormat() == NKikimrPQ::TBatchHeader::ECompressed);
    head.Batches.back().Unpack();
    head.Batches.back().Pack();
    TString str = head.Batches.back().Serialize();
    auto header = ExtractHeader(str.c_str(), str.size());
    TBatch batch(header, str.c_str() + header.ByteSize() + sizeof(ui16));
    batch.Unpack();

    head.PackedSize = head.Batches.back().GetPackedSize();
    UNIT_ASSERT(head.Batches.back().GetUnpackedSize() + GetMaxHeaderSize() >= head.Batches.back().GetPackedSize());
    THead newHead;
    newHead.Offset = head.GetNextOffset();
    newHead.Batches.push_back(TBatch(newHead.Offset, 0, TVector<TClientBlob>()));
    for (ui32 i = 0; i < 10; ++i) {
        newHead.Batches.back().AddBlob(TClientBlob(
            "sourceId2", i + 1, value, TMaybe<TPartData>(),
            TInstant::MilliSeconds(i + 1000), TInstant::MilliSeconds(i + 1000), 1, "", ""
        ));
        all.push_back(newHead.Batches.back().Blobs.back()); //newHead always glued
    }
    newHead.PackedSize = newHead.Batches.back().GetUnpackedSize();
    TString value2(partSize, 'b');
    ui32 maxBlobSize = 8 << 20;
    TPartitionedBlob blob(0, newHead.GetNextOffset(), "sourceId3", 1, parts, parts * value2.size(), head, newHead, headCompacted, false, maxBlobSize);

    TVector<std::pair<TKey, TString>> formed;

    TString error;
    for (ui32 i = 0; i < parts; ++i) {
        UNIT_ASSERT(!blob.IsComplete());
        UNIT_ASSERT(blob.IsNextPart("sourceId3", 1, i, &error));
        TMaybe<TPartData> partData = TPartData(i, parts, value2.size());
        TClientBlob clientBlob(
            "soruceId3", 1, value2, std::move(partData),
            TInstant::MilliSeconds(1), TInstant::MilliSeconds(1), 1, "", ""
        );
        all.push_back(clientBlob);
        auto res = blob.Add(std::move(clientBlob));
        if (!res.second.empty())
            formed.push_back(res);
    }
    UNIT_ASSERT(blob.IsComplete());
    UNIT_ASSERT(formed.size() == blob.GetFormedBlobs().size());
    for (ui32 i = 0; i < formed.size(); ++i) {
        UNIT_ASSERT(formed[i].first == blob.GetFormedBlobs()[i].first);
        UNIT_ASSERT(formed[i].second.size() == blob.GetFormedBlobs()[i].second);
        UNIT_ASSERT(formed[i].second.size() <= 8_MB);
        UNIT_ASSERT(formed[i].second.size() > 6_MB);
    }
    TVector<TClientBlob> real;
    ui32 nextOffset = headCompacted ? newHead.Offset : head.Offset;
    for (auto& p : formed) {
        const char* data = p.second.c_str();
        const char* end = data + p.second.size();
        ui64 offset = p.first.GetOffset();
        UNIT_ASSERT(offset == nextOffset);
        while(data < end) {
            auto header = ExtractHeader(data, end - data);
            UNIT_ASSERT(header.GetOffset() == nextOffset);
            nextOffset += header.GetCount();
            data += header.ByteSize() + sizeof(ui16);
            TBatch batch(header, data);
            data += header.GetPayloadSize();
            batch.Unpack();
            for (auto& b: batch.Blobs) {
                real.push_back(b);
            }
        }
    }
    ui32 s = 0;
    ui32 c = 0;

    if (formed.empty()) { //nothing compacted - newHead must be here

        if (!headCompacted) {
            for (auto& p : head.Batches) {
                p.Unpack();
                for (const auto& b : p.Blobs)
                    real.push_back(b);
            }
        }

        for (auto& p : newHead.Batches) {
            p.Unpack();
            for (const auto& b : p.Blobs)
                real.push_back(b);
        }
    }

    for (const auto& p : blob.GetClientBlobs()) {
        real.push_back(p);
        c++;
        s += p.GetBlobSize();
    }

    UNIT_ASSERT(c == leftInHead);
    UNIT_ASSERT(s + GetMaxHeaderSize() <= maxBlobSize);
    UNIT_ASSERT(real.size() == all.size());
    for (ui32 i = 0; i < all.size(); ++i) {
        UNIT_ASSERT(all[i].SourceId == real[i].SourceId);
        UNIT_ASSERT(all[i].SeqNo == real[i].SeqNo);
        UNIT_ASSERT(all[i].Data == real[i].Data);
        UNIT_ASSERT(all[i].PartData.Defined() == real[i].PartData.Defined());
        if (all[i].PartData.Defined()) {
            UNIT_ASSERT(all[i].PartData->PartNo == real[i].PartData->PartNo);
            UNIT_ASSERT(all[i].PartData->TotalParts == real[i].PartData->TotalParts);
            UNIT_ASSERT(all[i].PartData->TotalSize == real[i].PartData->TotalSize);
        }

    }
}

Y_UNIT_TEST(TestPartitionedBigTest) {

    Test(true, 100, 400_KB, 3);
    Test(false, 100, 512_KB - 9 - sizeof(ui64) - sizeof(ui16) - 100, 16); //serialized size of client blob is 512_KB - 100
    Test(false, 101, 512_KB - 9 - sizeof(ui64) - sizeof(ui16) - 100, 1); //serialized size of client blob is 512_KB - 100
    Test(false, 1, 512_KB - 9 - sizeof(ui64) - sizeof(ui16) - 100, 1); //serialized size of client blob is 512_KB - 100
    Test(true, 1, 512_KB - 9 - sizeof(ui64) - sizeof(ui16) - 100, 1); //serialized size of client blob is 512_KB - 100
    Test(true, 101, 512_KB - 9 - sizeof(ui64) - sizeof(ui16) - 100, 7); //serialized size of client blob is 512_KB - 100
}

Y_UNIT_TEST(TestBatchPacking) {
    TString value(10, 'a');
    TBatch batch;
    for (ui32 i = 0; i < 100; ++i) {
        batch.AddBlob(TClientBlob(
            "sourceId1", i + 1, value, TMaybe<TPartData>(),
            TInstant::MilliSeconds(1), TInstant::MilliSeconds(1), 0, "", ""
        ));
    }
    batch.Pack();
    TString s = batch.PackedData;
    UNIT_ASSERT(batch.Header.GetFormat() == NKikimrPQ::TBatchHeader::ECompressed);
    batch.Unpack();
    batch.Pack();
    UNIT_ASSERT(batch.PackedData == s);
    TString str = batch.Serialize();
    auto header = ExtractHeader(str.c_str(), str.size());
    TBatch batch2(header, str.c_str() + header.ByteSize() + sizeof(ui16));
    batch2.Unpack();
    Y_VERIFY(batch2.Blobs.size() == 100);

    TBatch batch3;
    batch3.AddBlob(TClientBlob(
        "sourceId", 999'999'999'999'999ll, "abacaba", TPartData{33, 66, 4'000'000'000u},
        TInstant::MilliSeconds(999'999'999'999ll), TInstant::MilliSeconds(1000), 0, "", ""
    ));
    batch3.Pack();
    UNIT_ASSERT(batch3.Header.GetFormat() == NKikimrPQ::TBatchHeader::EUncompressed);
    batch3.Unpack();
    Y_VERIFY(batch3.Blobs.size() == 1);
}


} //Y_UNIT_TEST_SUITE


} // TInternalsTest
} // namespace NKikimr::NPQ
