#pragma once
#include <ydb/library/accessor/accessor.h>
#include <ydb/library/yql/dq/expr_nodes/dq_expr_nodes.h>
#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>

namespace NKikimr::NKqp {
class TStagePredictor {
private:
    YDB_READONLY_FLAG(HasFinalCombiner, false);
    YDB_READONLY_FLAG(HasStateCombiner, false);
    YDB_READONLY(ui32, GroupByKeysCount, 0);

    YDB_READONLY_FLAG(HasSort, false);
    YDB_READONLY_FLAG(HasMapJoin, false);
    YDB_READONLY_FLAG(HasUdf, false);
    YDB_READONLY_FLAG(HasFilter, false);
    YDB_READONLY_FLAG(HasTop, false);
    YDB_READONLY_FLAG(HasRangeScan, false);
    YDB_READONLY_FLAG(HasLookup, false);

    YDB_READONLY(double, InputDataPrediction, 1);
    YDB_READONLY(double, OutputDataPrediction, 1);
    YDB_OPT(double, LevelDataPrediction);
    YDB_READONLY(ui32, StageLevel, 0);
    std::vector<double> InputDataVolumes;
    void Prepare();
    friend class TRequestPredictor;
public:
    void Scan(const NYql::TExprNode::TPtr& stageNode);
    void AcceptInputStageInfo(const TStagePredictor& info, const NYql::NNodes::TDqConnection& connection);
    void SerializeToKqpSettings(NYql::NDqProto::TProgram::TSettings& kqpProto) const;
    bool DeserializeFromKqpSettings(const NYql::NDqProto::TProgram::TSettings& kqpProto);
    static ui32 GetUsableThreads();

    ui32 CalcTasksOptimalCount(const ui32 availableThreadsCount, const std::optional<ui32> previousStageTasksCount) const;
};

class TRequestPredictor {
private:
    std::deque<TStagePredictor> StagePredictors;
    std::map<ui64, TStagePredictor*> StagesMap;
public:
    double GetLevelDataVolume(const ui32 level) const;
    TStagePredictor& BuildForStage(const NYql::NNodes::TDqPhyStage& stage, NYql::TExprContext& ctx);
};

}
