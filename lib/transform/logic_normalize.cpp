#include "transform/logic_normalize.hpp"

#include "core/grh.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        void replaceUsers(wolvrix::lib::grh::Graph &graph,
                          wolvrix::lib::grh::ValueId from,
                          wolvrix::lib::grh::ValueId to,
                          const std::function<void(std::string)> &onError)
        {
            try
            {
                graph.replaceAllUses(from, to);
            }
            catch (const std::exception &ex)
            {
                onError(std::string("Failed to replace operands: ") + ex.what());
                return;
            }

            std::vector<std::string> outputPortsToUpdate;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == from)
                {
                    outputPortsToUpdate.push_back(port.name);
                }
            }
            for (const auto &portName : outputPortsToUpdate)
            {
                try
                {
                    graph.bindOutputPort(portName, to);
                }
                catch (const std::exception &ex)
                {
                    onError(std::string("Failed to rebind output port: ") + ex.what());
                }
            }
        }
    } // namespace

    LogicNormalizePass::LogicNormalizePass()
        : Pass("logic-normalize", "logic-normalize",
               "Rewrite kLogicAnd/kLogicOr/kLogicNot into bitwise ops with explicit truth reduction")
    {
    }

    PassResult LogicNormalizePass::run()
    {
        PassResult result;
        std::size_t rewritten = 0;
        std::size_t truthOpsCreated = 0;

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            wolvrix::lib::grh::Graph &graph = *entry.second;

            // Truthiness helper t(x): a 1-bit value is already boolean; wider
            // operands go through a fresh kReduceOr (result is always 1-bit).
            // Equivalent kReduceOr ops are intentionally not deduplicated here;
            // RedundantElim CSEs them later.
            auto truthify = [&](wolvrix::lib::grh::ValueId value) -> wolvrix::lib::grh::ValueId {
                if (graph.getValue(value).width() == 1)
                {
                    return value;
                }
                const wolvrix::lib::grh::ValueId truthValue =
                    graph.createValue(1, false, wolvrix::lib::grh::ValueType::Logic);
                const wolvrix::lib::grh::OperationId truthOp =
                    graph.createOperation(wolvrix::lib::grh::OperationKind::kReduceOr);
                graph.addOperand(truthOp, value);
                graph.addResult(truthOp, truthValue);
                const wolvrix::lib::grh::SrcLoc genLoc =
                    makeTransformSrcLoc("logic-normalize", "truthify");
                graph.setValueSrcLoc(truthValue, genLoc);
                graph.setOpSrcLoc(truthOp, genLoc);
                ++truthOpsCreated;
                return truthValue;
            };

            // Snapshot the op list: ops created below are appended to the graph
            // and must not be visited by this rewrite loop.
            const std::vector<wolvrix::lib::grh::OperationId> ops(graph.operations().begin(),
                                                                  graph.operations().end());
            for (const auto opId : ops)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                const wolvrix::lib::grh::OperationKind kind = op.kind();
                if (kind != wolvrix::lib::grh::OperationKind::kLogicAnd &&
                    kind != wolvrix::lib::grh::OperationKind::kLogicOr &&
                    kind != wolvrix::lib::grh::OperationKind::kLogicNot)
                {
                    continue;
                }
                const auto operands = op.operands();
                const auto results = op.results();
                const std::size_t expectedOperands =
                    kind == wolvrix::lib::grh::OperationKind::kLogicNot ? 1 : 2;
                if (results.size() != 1 || operands.size() != expectedOperands)
                {
                    continue;
                }
                const wolvrix::lib::grh::ValueId resultId = results.front();
                if (!resultId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Value resultValue = graph.getValue(resultId);
                // Logic ops produce a 1-bit result; skip malformed ops instead
                // of changing the result type.
                if (resultValue.width() != 1)
                {
                    continue;
                }

                wolvrix::lib::grh::OperationKind newKind;
                std::vector<wolvrix::lib::grh::ValueId> newOperands;
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kLogicAnd:
                    newKind = wolvrix::lib::grh::OperationKind::kAnd;
                    newOperands = {truthify(operands[0]), truthify(operands[1])};
                    break;
                case wolvrix::lib::grh::OperationKind::kLogicOr:
                    newKind = wolvrix::lib::grh::OperationKind::kOr;
                    newOperands = {truthify(operands[0]), truthify(operands[1])};
                    break;
                case wolvrix::lib::grh::OperationKind::kLogicNot:
                default:
                    newKind = graph.getValue(operands[0]).width() == 1
                                  ? wolvrix::lib::grh::OperationKind::kNot
                                  : wolvrix::lib::grh::OperationKind::kReduceNor;
                    newOperands = {operands[0]};
                    break;
                }

                const wolvrix::lib::grh::ValueId newResult = graph.createValue(
                    resultValue.width(), resultValue.isSigned(), resultValue.type());
                const wolvrix::lib::grh::OperationId newOp = graph.createOperation(newKind);
                for (const auto operand : newOperands)
                {
                    graph.addOperand(newOp, operand);
                }
                graph.addResult(newOp, newResult);
                const wolvrix::lib::grh::SrcLoc genLoc =
                    makeTransformSrcLoc("logic-normalize", "rewrite");
                graph.setValueSrcLoc(newResult, genLoc);
                graph.setOpSrcLoc(newOp, genLoc);

                bool replaceFailed = false;
                replaceUsers(graph, resultId, newResult, [&](const std::string &msg) {
                    this->error(graph, op, msg);
                    replaceFailed = true;
                });
                if (replaceFailed)
                {
                    result.failed = true;
                    continue;
                }
                // The old op is now dead and is left for dead-code elimination.
                ++rewritten;
                result.changed = true;
            }
        }

        logInfo("logic-normalize: rewritten=" + std::to_string(rewritten) +
                " truth_ops_created=" + std::to_string(truthOpsCreated));
        return result;
    }

} // namespace wolvrix::lib::transform
