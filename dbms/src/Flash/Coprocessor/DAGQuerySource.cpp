// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Flash/Coprocessor/DAGContext.h>
#include <Flash/Coprocessor/DAGQuerySource.h>
#include <Flash/Coprocessor/InterpreterDAG.h>
#include <Flash/Coprocessor/collectOutputFieldTypes.h>
#include <Interpreters/Context.h>
#include <Parsers/makeDummyQuery.h>
#include <fmt/core.h>

namespace DB
{
namespace
{
void fillOrderForListBasedExecutors(DAGContext & dag_context, const DAGQueryBlock & query_block)
{
    assert(query_block.source);
    auto & list_based_executors_order = dag_context.list_based_executors_order;
    list_based_executors_order.push_back(query_block.source_name);
    if (query_block.selection)
        list_based_executors_order.push_back(query_block.selection_name);
    if (query_block.aggregation)
        list_based_executors_order.push_back(query_block.aggregation_name);
    if (query_block.having)
        list_based_executors_order.push_back(query_block.having_name);
    if (query_block.limit_or_topn)
        list_based_executors_order.push_back(query_block.limit_or_topn_name);
    if (query_block.exchange_sender)
        dag_context.list_based_executors_order.push_back(query_block.exchange_sender_name);
}
} // namespace

DAGQuerySource::DAGQuerySource(Context & context_)
    : context(context_)
{
    const tipb::DAGRequest & dag_request = *getDAGContext().dag_request;
    if (dag_request.has_root_executor())
    {
        QueryBlockIDGenerator id_generator;
        root_query_block = std::make_shared<DAGQueryBlock>(dag_request.root_executor(), id_generator);
    }
    else
    {
        root_query_block = std::make_shared<DAGQueryBlock>(1, dag_request.executors());
        auto & dag_context = getDAGContext();
        if (!dag_context.return_executor_id)
            fillOrderForListBasedExecutors(dag_context, *root_query_block);
    }
}

std::tuple<std::string, ASTPtr> DAGQuerySource::parse(size_t)
{
    // this is a WAR to avoid NPE when the MergeTreeDataSelectExecutor trying
    // to extract key range of the query.
    // todo find a way to enable key range extraction for dag query
    return {getDAGContext().dummy_query_string, getDAGContext().dummy_ast};
}

String DAGQuerySource::str(size_t)
{
    return getDAGContext().dummy_query_string;
}

std::unique_ptr<IInterpreter> DAGQuerySource::interpreter(Context &, QueryProcessingStage::Enum)
{
    return std::make_unique<InterpreterDAG>(context, *this);
}

DAGContext & DAGQuerySource::getDAGContext() const
{
    return *context.getDAGContext();
}

} // namespace DB
