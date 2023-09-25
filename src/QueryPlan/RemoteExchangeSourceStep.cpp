/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <string>

#include <Interpreters/Context_fwd.h>
#include <Interpreters/DistributedStages/AddressInfo.h>
#include <Interpreters/DistributedStages/ExchangeMode.h>
#include <Interpreters/DistributedStages/PlanSegment.h>
#include <Processors/Exchange/DataTrans/BroadcastSenderProxy.h>
#include <Processors/Exchange/DataTrans/BroadcastSenderProxyRegistry.h>
#include <Processors/Exchange/DataTrans/Brpc/BrpcRemoteBroadcastReceiver.h>
#include <Processors/Exchange/DataTrans/DataTrans_fwd.h>
#include <Processors/Exchange/DataTrans/IBroadcastReceiver.h>
#include <Processors/Exchange/DataTrans/Local/LocalBroadcastChannel.h>
#include <Processors/Exchange/DataTrans/Local/LocalChannelOptions.h>
#include <Processors/Exchange/DataTrans/MultiPathBoundedQueue.h>
#include <Processors/Exchange/DataTrans/MultiPathReceiver.h>
#include <Processors/Exchange/DeserializeBufTransform.h>
#include <Processors/Exchange/ExchangeDataKey.h>
#include <Processors/Exchange/ExchangeOptions.h>
#include <Processors/Exchange/ExchangeSource.h>
#include <Processors/Exchange/ExchangeUtils.h>
#include <Processors/QueryPipeline.h>
#include <QueryPlan/BuildQueryPipelineSettings.h>
#include <QueryPlan/ISourceStep.h>
#include <QueryPlan/RemoteExchangeSourceStep.h>
#include <brpc/controller.h>
#include <butil/endpoint.h>
#include <common/types.h>
#include <common/getFQDNOrHostName.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

RemoteExchangeSourceStep::RemoteExchangeSourceStep(PlanSegmentInputs inputs_, DataStream input_stream_)
    : ISourceStep(DataStream{.header = inputs_[0]->getHeader()}), inputs(std::move(inputs_))
{
    input_streams.emplace_back(std::move(input_stream_));
    logger = &Poco::Logger::get("RemoteExchangeSourceStep");
}

void RemoteExchangeSourceStep::toProto(Protos::RemoteExchangeSourceStep & proto, bool) const
{
    // NOTE: this step is ISourceStep but not using serde of ISourceStep
    // maybe a bug, but here just follow the original serde anyway
    input_streams[0].toProto(*proto.mutable_input_stream());
    proto.set_step_description(step_description);
    for (auto & element : inputs)
    {
        if (!element)
            throw Exception("PlanSegmentInput cannot be nullptr", ErrorCodes::LOGICAL_ERROR);
        element->toProto(*proto.add_inputs());
    }
}

std::shared_ptr<RemoteExchangeSourceStep>
RemoteExchangeSourceStep::fromProto(const Protos::RemoteExchangeSourceStep & proto, ContextPtr context)
{
    DataStream input_stream;
    input_stream.fillFromProto(proto.input_stream());
    auto step_description = proto.step_description();

    PlanSegmentInputs inputs;
    for (auto & proto_element : proto.inputs())
    {
        auto element = std::make_shared<PlanSegmentInput>();
        element->fillFromProto(proto_element, context);
        inputs.emplace_back(std::move(element));
    }

    auto step = std::make_shared<RemoteExchangeSourceStep>(inputs, input_stream);
    step->setStepDescription(step_description);

    return step;
}

std::shared_ptr<IQueryPlanStep> RemoteExchangeSourceStep::copy(ContextPtr) const
{
    return std::make_shared<RemoteExchangeSourceStep>(inputs, input_streams[0]);
}

void RemoteExchangeSourceStep::setPlanSegment(PlanSegment * plan_segment_)
{
    plan_segment = plan_segment_;
    plan_segment_id = plan_segment->getPlanSegmentId();
    query_id = plan_segment->getQueryId();
    coordinator_address = extractExchangeStatusHostPort(plan_segment->getCoordinatorAddress());
    read_address_info = plan_segment->getCurrentAddress();
    context = plan_segment->getContext();
    if (!context)
        throw Exception("Plan segment not set context", ErrorCodes::BAD_ARGUMENTS);
    options = ExchangeUtils::getExchangeOptions(context);
}

void RemoteExchangeSourceStep::initializePipeline(QueryPipeline & pipeline, const BuildQueryPipelineSettings & settings)
{
    auto current_tx_id = context->getCurrentTransactionID().toUInt64();
    if (!plan_segment)
        throw Exception("Should setPlanSegment before initializePipeline!", ErrorCodes::LOGICAL_ERROR);

    Pipe pipe;

    size_t source_num = 0;
    bool keep_order = context->getSettingsRef().exchange_enable_force_keep_order || context->getSettingsRef().enable_shuffle_with_order;
    if (!keep_order)
    {
        for (const auto & input : inputs)
        {
            if (input->needKeepOrder())
            {
                keep_order = input->needKeepOrder();
                break;
            }
        }
    }

    const Block & exchange_header = getOutputStream().header;
    Block source_header;
    if (keep_order)
        source_header = exchange_header;

    if (context->getSettingsRef().exchange_enable_multipath_reciever)
    {
        for (const auto & input : inputs)
        {
            size_t write_plan_segment_id = input->getPlanSegmentId();
            size_t exchange_parallel_size = input->getExchangeParallelSize();
            UInt32 exchange_id = input->getExchangeId();

            //TODO: hack logic for BROADCAST, we should remove this logic
            if (input->getExchangeMode() == ExchangeMode::BROADCAST)
                exchange_parallel_size = 1;

            size_t partition_id_start = (input->getParallelIndex() - 1) * exchange_parallel_size + 1;
            LocalChannelOptions local_options{
                .queue_size = context->getSettingsRef().exchange_local_receiver_queue_size, .max_timeout_ms = options.exhcange_timeout_ms};
            if (input->getSourceAddress().empty() && !settings.distributed_settings.is_explian)
                throw Exception("No source address!", ErrorCodes::LOGICAL_ERROR);
            bool enable_block_compress = context->getSettingsRef().exchange_enable_block_compress;
            BroadcastReceiverPtrs receivers;
            MultiPathQueuePtr collector
                = std::make_shared<MultiPathBoundedQueue>(context->getSettingsRef().exchange_multi_path_receiver_queue_size);
            bool is_final_plan_segment = plan_segment_id == 0;
            for (const auto & source_address : input->getSourceAddress())
            {
                auto write_address_info = extractExchangeHostPort(source_address);
                for (size_t i = 0; i < exchange_parallel_size; ++i)
                {
                    UInt32 partition_id = partition_id_start + i;
                    ExchangeDataKeyPtr data_key = std::make_shared<ExchangeDataKey>(current_tx_id, exchange_id, partition_id);
                    BroadcastReceiverPtr receiver;
                    if (ExchangeUtils::isLocalExchange(read_address_info, source_address))
                    {
                        if (!options.force_remote_mode)
                        {
                            LOG_TRACE(
                                logger,
                                "Create local exchange source : {}@{} for plansegment {}->{}",
                                *data_key,
                                write_address_info,
                                write_plan_segment_id,
                                plan_segment_id);
                            String name = LocalBroadcastChannel::generateName(
                                exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                            auto local_channel = std::make_shared<LocalBroadcastChannel>(data_key, local_options, name, collector, context);
                            receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(local_channel);
                        }
                        else
                        {
                            String localhost_address = context->getHostWithPorts().getExchangeAddress();
                            LOG_TRACE(
                                logger,
                                "Force local exchange use remote source : {}@{} for plansegment {}->{}",
                                *data_key,
                                localhost_address,
                                write_plan_segment_id,
                                plan_segment_id);
                            String name = BrpcRemoteBroadcastReceiver::generateName(
                                exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                            auto brpc_receiver = std::make_shared<BrpcRemoteBroadcastReceiver>(
                                std::move(data_key), localhost_address, context, exchange_header, collector, keep_order, name);
                            receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(brpc_receiver);
                        }
                    }
                    else
                    {
                        LOG_TRACE(
                            logger,
                            "Create remote exchange source : {}@{} for plansegment {}->{}",
                            *data_key,
                            write_address_info,
                            write_plan_segment_id,
                            plan_segment_id);
                        String name = BrpcRemoteBroadcastReceiver::generateName(
                            exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                        auto brpc_receiver = std::make_shared<BrpcRemoteBroadcastReceiver>(
                            std::move(data_key), write_address_info, context, exchange_header, collector, keep_order, name);
                        receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(brpc_receiver);
                    }
                    receivers.emplace_back(std::move(receiver));

                }
            }
            if (settings.distributed_settings.is_explian)
            {
                ExchangeDataKeyPtr data_key = std::make_shared<ExchangeDataKey>(current_tx_id, exchange_id, partition_id_start);
                String name = BrpcRemoteBroadcastReceiver::generateName(
                            exchange_id, write_plan_segment_id, plan_segment_id, partition_id_start, coordinator_address);
                auto brpc_receiver = std::make_shared<BrpcRemoteBroadcastReceiver>(std::move(data_key), "", context, exchange_header, keep_order, name);
                BroadcastReceiverPtr receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(brpc_receiver);
                receivers.emplace_back(std::move(receiver));
                source_num++;
            }
            String receiver_name = MultiPathReceiver::generateName(
                exchange_id, write_plan_segment_id, plan_segment_id, coordinator_address);
            auto multi_path_receiver = std::make_shared<MultiPathReceiver>(collector, std::move(receivers), exchange_header, receiver_name, enable_block_compress);
            LOG_DEBUG(logger, "Create {}", multi_path_receiver->getName());
            auto source = std::make_shared<ExchangeSource>(source_header, std::move(multi_path_receiver), options, is_final_plan_segment);
            pipe.addSource(std::move(source));
            source_num++;
        }
    }
    else
    {
        for (const auto & input : inputs)
        {
            size_t write_plan_segment_id = input->getPlanSegmentId();
            size_t exchange_parallel_size = input->getExchangeParallelSize();
            size_t exchange_id = input->getExchangeId();

            //TODO: hack logic for BROADCAST, we should remove this logic
            if (input->getExchangeMode() == ExchangeMode::BROADCAST)
                exchange_parallel_size = 1;

            size_t partition_id_start = (input->getParallelIndex() - 1) * exchange_parallel_size + 1;
            LocalChannelOptions local_options{
                .queue_size = context->getSettingsRef().exchange_local_receiver_queue_size, .max_timeout_ms = options.exhcange_timeout_ms};
            if (input->getSourceAddress().empty())
                throw Exception("No source address!", ErrorCodes::LOGICAL_ERROR);
            bool is_final_plan_segment = plan_segment_id == 0;
            for (const auto & source_address : input->getSourceAddress())
            {
                auto write_address_info = extractExchangeHostPort(source_address);
                for (size_t i = 0; i < exchange_parallel_size; ++i)
                {
                    size_t partition_id = partition_id_start + i;
                    ExchangeDataKeyPtr data_key = std::make_shared<ExchangeDataKey>(current_tx_id, exchange_id, partition_id);
                    BroadcastReceiverPtr receiver;
                    if (ExchangeUtils::isLocalExchange(read_address_info, source_address))
                    {
                        if (!options.force_remote_mode)
                        {
                            LOG_TRACE(
                                logger,
                                "Create local exchange source : {}@{} for plansegment {}->{}",
                                *data_key,
                                write_address_info,
                                write_plan_segment_id,
                                plan_segment_id);
                            String name = LocalBroadcastChannel::generateName(
                                exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                            auto local_channel = std::make_shared<LocalBroadcastChannel>(data_key, local_options, name, context);
                            receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(local_channel);
                        }
                        else
                        {
                            String localhost_address = context->getHostWithPorts().getExchangeAddress();
                            LOG_TRACE(
                                logger,
                                "Force local exchange use remote source : {}@{} for plansegment {}->{}",
                                *data_key,
                                localhost_address,
                                write_plan_segment_id,
                                plan_segment_id);
                            String name = BrpcRemoteBroadcastReceiver::generateName(
                                exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                            auto brpc_receiver = std::make_shared<BrpcRemoteBroadcastReceiver>(
                                std::move(data_key), localhost_address, context, exchange_header, keep_order, name);
                            receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(brpc_receiver);
                        }
                    }
                    else
                    {
                        LOG_TRACE(
                            logger,
                            "Create remote exchange source : {}@{} for plansegment {}->{}",
                            *data_key,
                            write_address_info,
                            write_plan_segment_id,
                            plan_segment_id);
                        String name = BrpcRemoteBroadcastReceiver::generateName(
                            exchange_id, write_plan_segment_id, plan_segment_id, partition_id, coordinator_address);
                        auto brpc_receiver = std::make_shared<BrpcRemoteBroadcastReceiver>(
                            std::move(data_key), write_address_info, context, exchange_header, keep_order, name);
                        receiver = std::dynamic_pointer_cast<IBroadcastReceiver>(brpc_receiver);
                    }
                    auto source = std::make_shared<ExchangeSource>(source_header, std::move(receiver), options, is_final_plan_segment);
                    pipe.addSource(std::move(source));
                    source_num++;
                }
            }
        }
    }

    pipeline.init(std::move(pipe));
    if (!keep_order)
    {
        pipeline.resize(context->getSettingsRef().exchange_source_pipeline_threads);
        pipeline.addSimpleTransform([enable_compress = context->getSettingsRef().exchange_enable_block_compress, header = exchange_header](
                                        const Block &) { return std::make_shared<DeserializeBufTransform>(header, enable_compress); });
    }
    LOG_DEBUG(logger, "Total exchange source : {}, keep_order: {}", source_num, keep_order);
    pipeline.setMinThreads(source_num);
    for (const auto & processor : pipeline.getProcessors())
        processors.emplace_back(processor);
}


void RemoteExchangeSourceStep::describePipeline(FormatSettings & settings) const
{
    if (!inputs.empty())
        settings.out << String(settings.offset, settings.indent_char) << "Source segment_id : [ " << std::to_string(inputs.back().get()->getPlanSegmentId()) << " ]\n";
    ISourceStep::describePipeline(settings);
}

}
