#include "arrow_helpers.h"
#include "switch_type.h"
#include "one_batch_input_stream.h"
#include "merging_sorted_input_stream.h"

#include <ydb/core/util/yverify_stream.h>
#include <util/system/yassert.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/io/memory.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/compute/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/array/array_primitive.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/array/builder_primitive.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type_traits.h>
#include <library/cpp/containers/stack_vector/stack_vec.h>
#include <library/cpp/actors/core/log.h>
#include <memory>

#define Y_VERIFY_OK(status) Y_VERIFY(status.ok(), "%s", status.ToString().c_str())

namespace NKikimr::NArrow {

namespace {

enum class ECompareResult : i8 {
    LESS = -1,
    BORDER = 0,
    GREATER = 1
};

template <typename T>
inline void UpdateCompare(const T& value, const T& border, ECompareResult& res) {
    if (res == ECompareResult::BORDER) {
        if constexpr (std::is_same_v<T, arrow::util::string_view>) {
            size_t minSize = (value.size() < border.size()) ? value.size() : border.size();
            int cmp = memcmp(value.data(), border.data(), minSize);
            if (cmp < 0) {
                res = ECompareResult::LESS;
            } else if (cmp > 0) {
                res = ECompareResult::GREATER;
            } else {
                UpdateCompare(value.size(), border.size(), res);
            }
        } else {
            if (value < border) {
                res = ECompareResult::LESS;
            } else if (value > border) {
                res = ECompareResult::GREATER;
            }
        }
    }
}

template <typename TArray>
inline auto GetValue(const std::shared_ptr<TArray>& array, int pos) {
    return array->GetView(pos);
}

template <typename TArray, typename T>
bool CompareImpl(const std::shared_ptr<arrow::Array>& column, const T& border,
                 std::vector<NArrow::ECompareResult>& rowsCmp)
{
    bool hasBorder = false;
    ECompareResult* res = &rowsCmp[0];
    auto array = std::static_pointer_cast<TArray>(column);

    for (int i = 0; i < array->length(); ++i, ++res) {
        UpdateCompare(GetValue(array, i), border, *res);
        hasBorder = hasBorder || (*res == ECompareResult::BORDER);
    }
    return !hasBorder;
}

template <typename TArray, typename T>
bool CompareImpl(const std::shared_ptr<arrow::ChunkedArray>& column, const T& border,
                 std::vector<NArrow::ECompareResult>& rowsCmp)
{
    bool hasBorder = false;
    ECompareResult* res = &rowsCmp[0];

    for (auto& chunk : column->chunks()) {
        auto array = std::static_pointer_cast<TArray>(chunk);

        for (int i = 0; i < chunk->length(); ++i, ++res) {
            UpdateCompare(GetValue(array, i), border, *res);
            hasBorder = hasBorder || (*res == ECompareResult::BORDER);
        }
    }
    return !hasBorder;
}

/// @return true in case we have no borders in compare: no need for future keys, allow early exit
template <typename TArray>
bool Compare(const arrow::Datum& column, const std::shared_ptr<arrow::Array>& borderArray,
             std::vector<NArrow::ECompareResult>& rowsCmp)
{
    auto border = GetValue(std::static_pointer_cast<TArray>(borderArray), 0);

    switch (column.kind()) {
        case arrow::Datum::ARRAY:
            return CompareImpl<TArray>(column.make_array(), border, rowsCmp);
        case arrow::Datum::CHUNKED_ARRAY:
            return CompareImpl<TArray>(column.chunked_array(), border, rowsCmp);
        default:
            break;
    }
    Y_VERIFY(false);
    return false;
}

bool SwitchCompare(const arrow::Datum& column, const std::shared_ptr<arrow::Array>& border,
                   std::vector<NArrow::ECompareResult>& rowsCmp) {
    Y_VERIFY(border->length() == 1);

    // first time it's empty
    if (rowsCmp.empty()) {
        rowsCmp.resize(column.length(), ECompareResult::BORDER);
    }

    return SwitchArrayType(column, [&](const auto& type) -> bool {
        using TWrap = std::decay_t<decltype(type)>;
        using TArray = typename arrow::TypeTraits<typename TWrap::T>::ArrayType;
        return Compare<TArray>(column, border, rowsCmp);
    });
}

template <typename T>
void CompositeCompare(std::shared_ptr<T> some, std::shared_ptr<arrow::RecordBatch> borderBatch,
                      std::vector<NArrow::ECompareResult>& rowsCmp) {
    auto key = borderBatch->schema()->fields();
    Y_VERIFY(key.size());

    for (size_t i = 0; i < key.size(); ++i) {
        auto& field = key[i];
        auto typeId = field->type()->id();
        auto column = some->GetColumnByName(field->name());
        std::shared_ptr<arrow::Array> border = borderBatch->GetColumnByName(field->name());
        Y_VERIFY(column);
        Y_VERIFY(border);
        Y_VERIFY(some->schema()->GetFieldByName(field->name())->type()->id() == typeId);

        if (SwitchCompare(column, border, rowsCmp)) {
            break; // early exit in case we have all rows compared: no borders, can omit key tail
        }
    }
}
#if 0
std::shared_ptr<arrow::Array> CastToInt32Array(const std::shared_ptr<arrow::Array>& arr) {
    auto newData = arr->data()->Copy();
    newData->type = arrow::int32();
    return std::make_shared<arrow::Int32Array>(newData);
}

std::shared_ptr<arrow::Array> CastToInt64Array(const std::shared_ptr<arrow::Array>& arr) {
    auto newData = arr->data()->Copy();
    newData->type = arrow::int64();
    return std::make_shared<arrow::Int64Array>(newData);
}

// We need more types than arrow::compute::SortToIndices() support out of the box
std::shared_ptr<arrow::UInt64Array> SortPermutation(const std::shared_ptr<arrow::Array>& arr) {
    switch (arr->type_id()) {
        case arrow::Type::DATE32:
        case arrow::Type::TIME32:
        {
            auto res = arrow::compute::SortToIndices(*CastToInt32Array(arr));
            Y_VERIFY_OK(res.status());
            return std::static_pointer_cast<arrow::UInt64Array>(*res);
        }
        case arrow::Type::DATE64:
        case arrow::Type::TIMESTAMP:
        case arrow::Type::TIME64:
        {
            auto res = arrow::compute::SortToIndices(*CastToInt64Array(arr));
            Y_VERIFY_OK(res.status());
            return std::static_pointer_cast<arrow::UInt64Array>(*res);
        }
        default:
            break;
    }
    auto res = arrow::compute::SortToIndices(*arr);
    Y_VERIFY_OK(res.status());
    return std::static_pointer_cast<arrow::UInt64Array>(*res);
}
#endif
}

template <typename TType>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl() {
    return std::make_shared<TType>();
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::Decimal128Type>() {
    return arrow::decimal(NScheme::DECIMAL_PRECISION, NScheme::DECIMAL_SCALE);
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::TimestampType>() {
    return arrow::timestamp(arrow::TimeUnit::TimeUnit::MICRO);
}

template <>
std::shared_ptr<arrow::DataType> CreateEmptyArrowImpl<arrow::DurationType>() {
    return arrow::duration(arrow::TimeUnit::TimeUnit::MICRO);
}

std::shared_ptr<arrow::DataType> GetArrowType(NScheme::TTypeInfo typeId) {
    std::shared_ptr<arrow::DataType> result;
    bool success = SwitchYqlTypeToArrowType(typeId, [&]<typename TType>(TTypeWrapper<TType> typeHolder) {
        Y_UNUSED(typeHolder);
        result = CreateEmptyArrowImpl<TType>();
        return true;
    });
    if (success) {
        return result;
    }
    return std::make_shared<arrow::NullType>();
}

std::shared_ptr<arrow::DataType> GetCSVArrowType(NScheme::TTypeInfo typeId) {
    std::shared_ptr<arrow::DataType> result;
    switch (typeId.GetTypeId()) {
        case NScheme::NTypeIds::Datetime:
            return std::make_shared<arrow::TimestampType>(arrow::TimeUnit::SECOND);
        case NScheme::NTypeIds::Timestamp:
            return std::make_shared<arrow::TimestampType>(arrow::TimeUnit::MICRO);
        case NScheme::NTypeIds::Date:
            return std::make_shared<arrow::TimestampType>(arrow::TimeUnit::SECOND);
        default:
            return GetArrowType(typeId);
    }
}

std::vector<std::shared_ptr<arrow::Field>> MakeArrowFields(const TVector<std::pair<TString, NScheme::TTypeInfo>>& columns) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(columns.size());
    for (auto& [name, ydbType] : columns) {
        std::string colName(name.data(), name.size());
        fields.emplace_back(std::make_shared<arrow::Field>(colName, GetArrowType(ydbType)));
    }
    return fields;
}

std::shared_ptr<arrow::Schema> MakeArrowSchema(const TVector<std::pair<TString, NScheme::TTypeInfo>>& ydbColumns) {
    return std::make_shared<arrow::Schema>(MakeArrowFields(ydbColumns));
}

TString SerializeSchema(const arrow::Schema& schema) {
    auto buffer = arrow::ipc::SerializeSchema(schema);
    if (!buffer.ok()) {
        return {};
    }
    return TString((const char*)(*buffer)->data(), (*buffer)->size());
}

std::shared_ptr<arrow::Schema> DeserializeSchema(const TString& str) {
    std::shared_ptr<arrow::Buffer> buffer(std::make_shared<TBufferOverString>(str));
    arrow::io::BufferReader reader(buffer);
    arrow::ipc::DictionaryMemo dictMemo;
    auto schema = ReadSchema(&reader, &dictMemo);
    if (!schema.ok()) {
        return {};
    }
    return *schema;
}

namespace {
    class TFixedStringOutputStream final : public arrow::io::OutputStream {
    public:
        TFixedStringOutputStream(TString* out)
            : Out(out)
            , Position(0)
        { }

        arrow::Status Close() override {
            Out = nullptr;
            return arrow::Status::OK();
        }

        bool closed() const override {
            return Out == nullptr;
        }

        arrow::Result<int64_t> Tell() const override {
            return Position;
        }

        arrow::Status Write(const void* data, int64_t nbytes) override {
            if (Y_LIKELY(nbytes > 0)) {
                Y_VERIFY(Out && Out->size() - Position >= ui64(nbytes));
                char* dst = &(*Out)[Position];
                ::memcpy(dst, data, nbytes);
                Position += nbytes;
            }

            return arrow::Status::OK();
        }

        size_t GetPosition() const {
            return Position;
        }

    private:
        TString* Out;
        size_t Position;
    };
}

TString SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch, const arrow::ipc::IpcWriteOptions& options) {
    arrow::ipc::IpcPayload payload;
    auto status = arrow::ipc::GetRecordBatchPayload(*batch, options, &payload);
    Y_VERIFY_OK(status);

    int32_t metadata_length = 0;
    arrow::io::MockOutputStream mock;
    status = arrow::ipc::WriteIpcPayload(payload, options, &mock, &metadata_length);
    Y_VERIFY_OK(status);

    TString str;
    str.resize(mock.GetExtentBytesWritten());

    TFixedStringOutputStream out(&str);
    status = arrow::ipc::WriteIpcPayload(payload, options, &out, &metadata_length);
    Y_VERIFY_OK(status);
    Y_VERIFY(out.GetPosition() == str.size());

    return str;
}

TString SerializeBatchNoCompression(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto writeOptions = arrow::ipc::IpcWriteOptions::Defaults();
    writeOptions.use_threads = false;
    return SerializeBatch(batch, writeOptions);
}

std::shared_ptr<arrow::RecordBatch> DeserializeBatch(const TString& blob, const std::shared_ptr<arrow::Schema>& schema) {
    arrow::ipc::DictionaryMemo dictMemo;
    auto options = arrow::ipc::IpcReadOptions::Defaults();
    options.use_threads = false;

    std::shared_ptr<arrow::Buffer> buffer(std::make_shared<TBufferOverString>(blob));
    arrow::io::BufferReader reader(buffer);
    auto batch = ReadRecordBatch(schema, &dictMemo, options, &reader);
    if (!batch.ok() || !(*batch)->Validate().ok()) {
        return {};
    }
    return *batch;
}

std::shared_ptr<arrow::RecordBatch> MakeEmptyBatch(const std::shared_ptr<arrow::Schema>& schema) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(schema->num_fields());

    for (auto& field : schema->fields()) {
        auto result = arrow::MakeArrayOfNull(field->type(), 0);
        Y_VERIFY_OK(result.status());
        columns.emplace_back(*result);
        Y_VERIFY(columns.back());
    }
    return arrow::RecordBatch::Make(schema, 0, columns);
}

std::shared_ptr<arrow::RecordBatch> ExtractColumns(const std::shared_ptr<arrow::RecordBatch>& srcBatch,
                                                   const std::vector<TString>& columnNames) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(columnNames.size());
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(columnNames.size());

    auto srcSchema = srcBatch->schema();
    for (auto& name : columnNames) {
        int pos = srcSchema->GetFieldIndex(name);
        if (pos < 0) {
            return {};
        }
        fields.push_back(srcSchema->field(pos));
        columns.push_back(srcBatch->column(pos));
    }

    return arrow::RecordBatch::Make(std::make_shared<arrow::Schema>(std::move(fields)), srcBatch->num_rows(), std::move(columns));
}

std::shared_ptr<arrow::RecordBatch> ExtractColumns(const std::shared_ptr<arrow::RecordBatch>& srcBatch,
                                                   const std::shared_ptr<arrow::Schema>& dstSchema,
                                                   bool addNotExisted) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(dstSchema->num_fields());

    for (auto& field : dstSchema->fields()) {
        columns.push_back(srcBatch->GetColumnByName(field->name()));
        if (!columns.back()->type()->Equals(field->type())) {
            columns.back() = {};
        }

        if (!columns.back()) {
            if (addNotExisted) {
                auto result = arrow::MakeArrayOfNull(field->type(), srcBatch->num_rows());
                if (!result.ok()) {
                    return {};
                }
                columns.back() = *result;
            } else {
                return {};
            }
        }
    }

    return arrow::RecordBatch::Make(dstSchema, srcBatch->num_rows(), columns);
}

std::shared_ptr<arrow::RecordBatch> ExtractExistedColumns(const std::shared_ptr<arrow::RecordBatch>& srcBatch,
                                                          const arrow::FieldVector& fieldsToExtract) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(fieldsToExtract.size());
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(fieldsToExtract.size());

    auto srcSchema = srcBatch->schema();
    for (auto& fldToExtract : fieldsToExtract) {
        auto& name = fldToExtract->name();
        auto field = srcSchema->GetFieldByName(name);
        if (field && field->type()->Equals(fldToExtract->type())) {
            fields.push_back(field);
            columns.push_back(srcBatch->GetColumnByName(name));
        }
    }

    return arrow::RecordBatch::Make(std::make_shared<arrow::Schema>(std::move(fields)), srcBatch->num_rows(), std::move(columns));
}

std::shared_ptr<arrow::Table> CombineInTable(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    auto res = arrow::Table::FromRecordBatches(batches);
    if (!res.ok()) {
        return {};
    }

    res = (*res)->CombineChunks();
    if (!res.ok()) {
        return {};
    }

    return *res;
}

std::shared_ptr<arrow::RecordBatch> CombineBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    auto table = CombineInTable(batches);
    return ToBatch(table);
}

std::shared_ptr<arrow::RecordBatch> ToBatch(const std::shared_ptr<arrow::Table>& table) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(table->num_columns());
    for (auto& col : table->columns()) {
        Y_VERIFY(col->num_chunks() == 1);
        columns.push_back(col->chunk(0));
    }
    return arrow::RecordBatch::Make(table->schema(), table->num_rows(), columns);
}

std::shared_ptr<arrow::RecordBatch> CombineSortedBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                         const std::shared_ptr<TSortDescription>& description) {
    TVector<NArrow::IInputStream::TPtr> streams;
    for (auto& batch : batches) {
        streams.push_back(std::make_shared<NArrow::TOneBatchInputStream>(batch));
    }

    auto mergeStream = std::make_shared<NArrow::TMergingSortedInputStream>(streams, description, Max<ui64>());
    std::shared_ptr<arrow::RecordBatch> batch = mergeStream->Read();
    Y_VERIFY(!mergeStream->Read());
    return batch;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> MergeSortedBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                                    const std::shared_ptr<TSortDescription>& description,
                                                                    size_t maxBatchRows) {
    Y_VERIFY(maxBatchRows);
    ui64 numRows = 0;
    std::vector<NArrow::IInputStream::TPtr> streams;
    streams.reserve(batches.size());
    for (auto& batch : batches) {
        if (batch->num_rows()) {
            numRows += batch->num_rows();
            streams.push_back(std::make_shared<NArrow::TOneBatchInputStream>(batch));
        }
    }

    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    out.reserve(numRows / maxBatchRows + 1);

    auto mergeStream = std::make_shared<NArrow::TMergingSortedInputStream>(streams, description, maxBatchRows);
    while (std::shared_ptr<arrow::RecordBatch> batch = mergeStream->Read()) {
        Y_VERIFY(batch->num_rows());
        out.push_back(batch);
    }
    return out;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> SliceSortedBatches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                                                    const std::shared_ptr<TSortDescription>& description,
                                                                    size_t maxBatchRows) {
    Y_VERIFY(!description->Reverse);

    std::vector<NArrow::IInputStream::TPtr> streams;
    streams.reserve(batches.size());
    for (auto& batch : batches) {
        if (batch->num_rows()) {
            streams.push_back(std::make_shared<NArrow::TOneBatchInputStream>(batch));
        }
    }

    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    out.reserve(streams.size());

    auto dedupStream = std::make_shared<NArrow::TMergingSortedInputStream>(streams, description, maxBatchRows, true);
    while (std::shared_ptr<arrow::RecordBatch> batch = dedupStream->Read()) {
        Y_VERIFY(batch->num_rows());
        out.push_back(batch);
    }
    return out;
}

// Check if the permutation doesn't reorder anything
bool IsNoOp(const arrow::UInt64Array& permutation) {
    for (i64 i = 0; i < permutation.length(); ++i) {
        if (permutation.Value(i) != (ui64)i) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<arrow::RecordBatch> Reorder(const std::shared_ptr<arrow::RecordBatch>& batch,
                                            const std::shared_ptr<arrow::UInt64Array>& permutation) {
    Y_VERIFY(permutation->length() == batch->num_rows());

    auto res = IsNoOp(*permutation) ? batch : arrow::compute::Take(batch, permutation);
    Y_VERIFY(res.ok());
    return (*res).record_batch();
}

std::vector<std::shared_ptr<arrow::RecordBatch>> ShardingSplit(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                               const std::vector<ui32>& sharding, ui32 numShards) {
    Y_VERIFY((size_t)batch->num_rows() == sharding.size());

    std::vector<std::vector<ui32>> shardRows(numShards);
    for (size_t row = 0; row < sharding.size(); ++row) {
        ui32 shardNo = sharding[row];
        Y_VERIFY(shardNo < numShards);
        shardRows[shardNo].push_back(row);
    }

    std::shared_ptr<arrow::UInt64Array> permutation;
    {
        arrow::UInt64Builder builder;
        Y_VERIFY_OK(builder.Reserve(sharding.size()));

        for (ui32 shardNo = 0; shardNo < numShards; ++shardNo) {
            for (auto& row : shardRows[shardNo]) {
                Y_VERIFY_OK(builder.Append(row));
            }
        }
        Y_VERIFY_OK(builder.Finish(&permutation));
    }

    auto reorderedBatch = Reorder(batch, permutation);

    std::vector<std::shared_ptr<arrow::RecordBatch>> out(numShards);

    int offset = 0;
    for (ui32 shardNo = 0; shardNo < numShards; ++shardNo) {
        int length = shardRows[shardNo].size();
        if (length) {
            out[shardNo] = reorderedBatch->Slice(offset, length);
            offset += length;
        }
    }

    Y_VERIFY(offset == batch->num_rows());
    return out;
}

void DedupSortedBatch(const std::shared_ptr<arrow::RecordBatch>& batch,
                      const std::shared_ptr<arrow::Schema>& sortingKey,
                      std::vector<std::shared_ptr<arrow::RecordBatch>>& out) {
    if (batch->num_rows() < 2) {
        out.push_back(batch);
        return;
    }

    Y_VERIFY_DEBUG(NArrow::IsSorted(batch, sortingKey));

    auto keyBatch = ExtractColumns(batch, sortingKey);
    auto& keyColumns = keyBatch->columns();

    bool same = false;
    int start = 0;
    for (int i = 1; i < batch->num_rows(); ++i) {
        TRawReplaceKey prev(&keyColumns, i - 1);
        TRawReplaceKey current(&keyColumns, i);
        if (prev == current) {
            if (!same) {
                out.push_back(batch->Slice(start, i - start));
                Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(out.back(), sortingKey));
                same = true;
            }
        } else if (same) {
            same = false;
            start = i;
        }
    }
    if (!start) {
        out.push_back(batch);
    } else if (!same) {
        out.push_back(batch->Slice(start, batch->num_rows() - start));
    }
    Y_VERIFY_DEBUG(NArrow::IsSortedAndUnique(out.back(), sortingKey));
}

template <bool desc, bool uniq>
static bool IsSelfSorted(const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (batch->num_rows() < 2) {
        return true;
    }
    auto& columns = batch->columns();

    for (int i = 1; i < batch->num_rows(); ++i) {
        TRawReplaceKey prev(&columns, i - 1);
        TRawReplaceKey current(&columns, i);
        if constexpr (desc) {
            if (prev < current) {
                return false;
            }
        } else {
            if (current < prev) {
                return false;
            }
        }
        if constexpr (uniq) {
            if (prev == current) {
                return false;
            }
        }
    }
    return true;
}

bool IsSorted(const std::shared_ptr<arrow::RecordBatch>& batch,
              const std::shared_ptr<arrow::Schema>& sortingKey, bool desc) {
    auto keyBatch = ExtractColumns(batch, sortingKey);
    if (desc) {
        return IsSelfSorted<true, false>(keyBatch);
    } else {
        return IsSelfSorted<false, false>(keyBatch);
    }
}

bool IsSortedAndUnique(const std::shared_ptr<arrow::RecordBatch>& batch,
                       const std::shared_ptr<arrow::Schema>& sortingKey, bool desc) {
    auto keyBatch = ExtractColumns(batch, sortingKey);
    if (desc) {
        return IsSelfSorted<true, true>(keyBatch);
    } else {
        return IsSelfSorted<false, true>(keyBatch);
    }
}

bool HasAllColumns(const std::shared_ptr<arrow::RecordBatch>& batch, const std::shared_ptr<arrow::Schema>& schema) {
    for (auto& field : schema->fields()) {
        if (batch->schema()->GetFieldIndex(field->name()) < 0) {
            return false;
        }
    }
    return true;
}

std::vector<std::unique_ptr<arrow::ArrayBuilder>> MakeBuilders(const std::shared_ptr<arrow::Schema>& schema,
                                                               size_t reserve, const std::map<std::string, ui64>& sizeByColumn) {
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(schema->num_fields());

    for (auto& field : schema->fields()) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        auto status = arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder);
        Y_VERIFY_OK(status);
        if (sizeByColumn.size()) {
            auto it = sizeByColumn.find(field->name());
            if (it != sizeByColumn.end()) {
                Y_VERIFY(NArrow::ReserveData(*builder, it->second));
            }
        }

        if (reserve) {
            Y_VERIFY_OK(builder->Reserve(reserve));
        }

        builders.emplace_back(std::move(builder));

    }
    return builders;
}

std::vector<std::shared_ptr<arrow::Array>> Finish(std::vector<std::unique_ptr<arrow::ArrayBuilder>>&& builders) {
    std::vector<std::shared_ptr<arrow::Array>> out;
    for (auto& builder : builders) {
        std::shared_ptr<arrow::Array> array;
        auto status = builder->Finish(&array);
        Y_VERIFY_OK(status);
        out.emplace_back(array);
    }
    return out;
}

TVector<TString> ColumnNames(const std::shared_ptr<arrow::Schema>& schema) {
    TVector<TString> out;
    out.reserve(schema->num_fields());
    for (int i = 0; i < schema->num_fields(); ++i) {
        auto& name = schema->field(i)->name();
        out.emplace_back(TString(name.data(), name.size()));
    }
    return out;
}

i64 LowerBound(const std::shared_ptr<arrow::Array>& array, const arrow::Scalar& border, i64 offset) {
    i64 pos = 0;
    SwitchType(array->type_id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using T = typename TWrap::T;
        using TArray = typename arrow::TypeTraits<T>::ArrayType;
        using TScalar = typename arrow::TypeTraits<T>::ScalarType;

        auto& column = static_cast<const TArray&>(*array);

        if constexpr (arrow::is_number_type<T>() || arrow::is_timestamp_type<T>()) {
            const auto* start = column.raw_values() + offset;
            const auto* end = column.raw_values() + column.length();
            pos = offset;
            pos += std::lower_bound(start, end, static_cast<const TScalar&>(border).value) - start;
        } else if constexpr (arrow::has_string_view<T>()) {
            arrow::util::string_view value(*static_cast<const TScalar&>(border).value);

            // TODO: binary search
            for (pos = offset; pos < column.length(); ++pos) {
                if (!(column.GetView(pos) < value)) {
                    return true;
                }
            }
        } else {
            Y_VERIFY(false); // not implemented
        }

        return true;
    });

    return pos;
}

std::shared_ptr<arrow::UInt64Array> MakeUI64Array(ui64 value, i64 size) {
    auto res = arrow::MakeArrayFromScalar(arrow::UInt64Scalar(value), size);
    Y_VERIFY(res.ok());
    return std::static_pointer_cast<arrow::UInt64Array>(*res);
}

std::pair<int, int> FindMinMaxPosition(const std::shared_ptr<arrow::Array>& array) {
    if (array->length() == 0) {
        return {-1, -1};
    }

    int minPos = 0;
    int maxPos = 0;
    SwitchType(array->type_id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using TArray = typename arrow::TypeTraits<typename TWrap::T>::ArrayType;

        auto& column = static_cast<const TArray&>(*array);

        for (int i = 1; i < column.length(); ++i) {
            const auto& value = column.GetView(i);
            if (value < column.GetView(minPos)) {
                minPos = i;
            }
            if (value > column.GetView(maxPos)) {
                maxPos = i;
            }
        }
        return true;
    });
    return {minPos, maxPos};
}

std::shared_ptr<arrow::Scalar> MinScalar(const std::shared_ptr<arrow::DataType>& type) {
    std::shared_ptr<arrow::Scalar> out;
    SwitchType(type->id(), [&](const auto& t) {
        using TWrap = std::decay_t<decltype(t)>;
        using T = typename TWrap::T;
        using TScalar = typename arrow::TypeTraits<T>::ScalarType;

        if constexpr (std::is_same_v<T, arrow::StringType> ||
                      std::is_same_v<T, arrow::BinaryType> ||
                      std::is_same_v<T, arrow::LargeStringType> ||
                      std::is_same_v<T, arrow::LargeBinaryType>) {
            out = std::make_shared<TScalar>(arrow::Buffer::FromString(""), type);
        } else if constexpr (std::is_same_v<T, arrow::FixedSizeBinaryType>) {
            std::string s(static_cast<arrow::FixedSizeBinaryType&>(*type).byte_width(), '\0');
            out = std::make_shared<TScalar>(arrow::Buffer::FromString(s), type);
        } else if constexpr (std::is_same_v<T, arrow::HalfFloatType>) {
            return false;
        } else if constexpr (arrow::is_temporal_type<T>::value) {
            using TCType = typename arrow::TypeTraits<T>::CType;
            out = std::make_shared<TScalar>(Min<TCType>(), type);
        } else if constexpr (arrow::has_c_type<T>::value) {
            using TCType = typename arrow::TypeTraits<T>::CType;
            out = std::make_shared<TScalar>(Min<TCType>());
        } else {
            return false;
        }
        return true;
    });
    Y_VERIFY(out);
    return out;
}

std::shared_ptr<arrow::Scalar> GetScalar(const std::shared_ptr<arrow::Array>& array, int position) {
    auto res = array->GetScalar(position);
    Y_VERIFY(res.ok());
    return *res;
}

bool IsGoodScalar(const std::shared_ptr<arrow::Scalar>& x) {
    if (!x) {
        return false;
    }

    return SwitchType(x->type->id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using TScalar = typename arrow::TypeTraits<typename TWrap::T>::ScalarType;
        using TValue = std::decay_t<decltype(static_cast<const TScalar&>(*x).value)>;

        if constexpr (arrow::has_string_view<typename TWrap::T>()) {
            const auto& xval = static_cast<const TScalar&>(*x).value;
            return xval && xval->data();
        }
        if constexpr (std::is_arithmetic_v<TValue>) {
            return true;
        }
        return false;
    });
}

bool ScalarLess(const std::shared_ptr<arrow::Scalar>& x, const std::shared_ptr<arrow::Scalar>& y) {
    Y_VERIFY(x);
    Y_VERIFY(y);
    return ScalarLess(*x, *y);
}

bool ScalarLess(const arrow::Scalar& x, const arrow::Scalar& y) {
    Y_VERIFY_S(x.type->Equals(y.type), x.type->ToString() + " vs " + y.type->ToString());

    return SwitchType(x.type->id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using TScalar = typename arrow::TypeTraits<typename TWrap::T>::ScalarType;
        using TValue = std::decay_t<decltype(static_cast<const TScalar&>(x).value)>;

        if constexpr (arrow::has_string_view<typename TWrap::T>()) {
            const auto& xval = static_cast<const TScalar&>(x).value;
            const auto& yval = static_cast<const TScalar&>(y).value;
            Y_VERIFY(xval);
            Y_VERIFY(yval);
            TStringBuf xBuf(reinterpret_cast<const char*>(xval->data()), xval->size());
            TStringBuf yBuf(reinterpret_cast<const char*>(yval->data()), yval->size());
            return xBuf < yBuf;
        }
        if constexpr (std::is_arithmetic_v<TValue>) {
            const auto& xval = static_cast<const TScalar&>(x).value;
            const auto& yval = static_cast<const TScalar&>(y).value;
            return xval < yval;
        }
        Y_VERIFY(false); // TODO: non primitive types
        return false;
    });
}

std::shared_ptr<arrow::UInt64Array> MakePermutation(int size, bool reverse) {
    if (size < 1) {
        return {};
    }

    arrow::UInt64Builder builder;
    if (!builder.Reserve(size).ok()) {
        return {};
    }

    if (reverse) {
        ui64 value = size - 1;
        for (i64 i = 0; i < size; ++i, --value) {
            if (!builder.Append(value).ok()) {
                return {};
            }
        }
    } else {
        for (i64 i = 0; i < size; ++i) {
            if (!builder.Append(i).ok()) {
                return {};
            }
        }
    }

    std::shared_ptr<arrow::UInt64Array> out;
    if (!builder.Finish(&out).ok()) {
        return {};
    }
    return out;
}

std::shared_ptr<arrow::BooleanArray> MakeFilter(const std::vector<bool>& bits) {
    arrow::BooleanBuilder builder;
    auto res = builder.Resize(bits.size());
    Y_VERIFY_OK(res);
    res = builder.AppendValues(bits);
    Y_VERIFY_OK(res);

    std::shared_ptr<arrow::BooleanArray> out;
    res = builder.Finish(&out);
    Y_VERIFY_OK(res);
    return out;
}

std::vector<bool> CombineFilters(std::vector<bool>&& f1, std::vector<bool>&& f2) {
    if (f1.empty()) {
        return f2;
    } else if (f2.empty()) {
        return f1;
    }

    Y_VERIFY(f1.size() == f2.size());
    for (size_t i = 0; i < f1.size(); ++i) {
        f1[i] = f1[i] && f2[i];
    }
    return f1;
}

std::vector<bool> CombineFilters(std::vector<bool>&& f1, std::vector<bool>&& f2, size_t& count) {
    count = 0;
    if (f1.empty() && !f2.empty()) {
        f1.swap(f2);
    }
    if (f1.empty()) {
        return {};
    }

    if (f2.empty()) {
        for (bool bit : f1) {
            count += bit;
        }
        return f1;
    }

    Y_VERIFY(f1.size() == f2.size());
    for (size_t i = 0; i < f1.size(); ++i) {
        f1[i] = f1[i] && f2[i];
        count += f1[i];
    }
    return f1;
}

std::vector<bool> MakePredicateFilter(const arrow::Datum& datum, const arrow::Datum& border,
                                      ECompareType compareType) {
    std::vector<NArrow::ECompareResult> cmps;

    switch (datum.kind()) {
        case arrow::Datum::ARRAY:
            Y_VERIFY(border.kind() == arrow::Datum::ARRAY);
            SwitchCompare(datum, border.make_array(), cmps);
            break;
        case arrow::Datum::CHUNKED_ARRAY:
            Y_VERIFY(border.kind() == arrow::Datum::ARRAY);
            SwitchCompare(datum, border.make_array(), cmps);
            break;
        case arrow::Datum::RECORD_BATCH:
            Y_VERIFY(border.kind() == arrow::Datum::RECORD_BATCH);
            CompositeCompare(datum.record_batch(), border.record_batch(), cmps);
            break;
        case arrow::Datum::TABLE:
            Y_VERIFY(border.kind() == arrow::Datum::RECORD_BATCH);
            CompositeCompare(datum.table(), border.record_batch(), cmps);
            break;
        default:
            Y_VERIFY(false);
            break;
    }

    std::vector<bool> bits(cmps.size());

    switch (compareType) {
        case ECompareType::LESS:
            for (size_t i = 0; i < cmps.size(); ++i) {
                bits[i] = (cmps[i] < ECompareResult::BORDER);
            }
            break;
        case ECompareType::LESS_OR_EQUAL:
            for (size_t i = 0; i < cmps.size(); ++i) {
                bits[i] = (cmps[i] <= ECompareResult::BORDER);
            }
            break;
        case ECompareType::GREATER:
            for (size_t i = 0; i < cmps.size(); ++i) {
                bits[i] = (cmps[i] > ECompareResult::BORDER);
            }
            break;
        case ECompareType::GREATER_OR_EQUAL:
            for (size_t i = 0; i < cmps.size(); ++i) {
                bits[i] = (cmps[i] >= ECompareResult::BORDER);
            }
            break;
    }

    return bits;
}

std::shared_ptr<arrow::UInt64Array> MakeSortPermutation(const std::shared_ptr<arrow::RecordBatch>& batch,
                                                        const std::shared_ptr<arrow::Schema>& sortingKey) {
    auto keyBatch = ExtractColumns(batch, sortingKey);
    auto keyColumns = std::make_shared<TArrayVec>(keyBatch->columns());
    std::vector<TRawReplaceKey> points;
    points.reserve(keyBatch->num_rows());

    for (int i = 0; i < keyBatch->num_rows(); ++i) {
        points.push_back(TRawReplaceKey(keyColumns.get(), i));
    }

    bool haveNulls = false;
    for (auto& column : *keyColumns) {
        if (HasNulls(column)) {
            haveNulls = true;
            break;
        }
    }

    if (haveNulls) {
        std::sort(points.begin(), points.end());
    } else {
        std::sort(points.begin(), points.end(),
            [](const TRawReplaceKey& a, const TRawReplaceKey& b) {
                return a.LessNotNull(b);
            }
        );
    }

    arrow::UInt64Builder builder;
    Y_VERIFY_OK(builder.Reserve(points.size()));

    for (auto& point : points) {
        Y_VERIFY_OK(builder.Append(point.GetPosition()));
    }

    std::shared_ptr<arrow::UInt64Array> out;
    Y_VERIFY_OK(builder.Finish(&out));
    return out;
}

std::shared_ptr<arrow::RecordBatch> SortBatch(const std::shared_ptr<arrow::RecordBatch>& batch,
                                              const std::shared_ptr<arrow::Schema>& sortingKey) {
    auto sortPermutation = MakeSortPermutation(batch, sortingKey);
    return Reorder(batch, sortPermutation);
}

std::shared_ptr<arrow::Array> BoolVecToArray(const std::vector<bool>& vec) {
    std::shared_ptr<arrow::Array> out;
    arrow::BooleanBuilder builder;
    for (const auto val : vec) {
        Y_VERIFY(builder.Append(val).ok());
    }
    Y_VERIFY(builder.Finish(&out).ok());
    return out;
}


bool ArrayScalarsEqual(const std::shared_ptr<arrow::Array>& lhs, const std::shared_ptr<arrow::Array>& rhs) {
    bool res = lhs->length() == rhs->length();
    for (int64_t i = 0; i < lhs->length() && res; ++i) {
        res &= arrow::ScalarEquals(*lhs->GetScalar(i).ValueOrDie(), *rhs->GetScalar(i).ValueOrDie());
    }
    return res;
}

bool ReserveData(arrow::ArrayBuilder& builder, const size_t size) {
    if (builder.type()->id() == arrow::Type::BINARY) {
        arrow::BaseBinaryBuilder<arrow::BinaryType>& bBuilder = static_cast<arrow::BaseBinaryBuilder<arrow::BinaryType>&>(builder);
        return bBuilder.ReserveData(size).ok();
    } else if (builder.type()->id() == arrow::Type::STRING) {
        arrow::BaseBinaryBuilder<arrow::StringType>& bBuilder = static_cast<arrow::BaseBinaryBuilder<arrow::StringType>&>(builder);
        return bBuilder.ReserveData(size).ok();
    }
    return true;
}

}
