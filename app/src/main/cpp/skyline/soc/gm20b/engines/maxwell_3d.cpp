// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2018-2020 fincs (https://github.com/devkitPro/deko3d)

#include <boost/preprocessor/repeat.hpp>
#include <gpu/interconnect/command_executor.h>
#include <soc/gm20b/channel.h>
#include <soc.h>
#include "maxwell/types.h"
#include "maxwell_3d.h"

namespace skyline::soc::gm20b::engine::maxwell3d {
    #define REGTYPE(state) gpu::interconnect::maxwell3d::state::EngineRegisters

    static gpu::interconnect::maxwell3d::PipelineState::EngineRegisters MakePipelineStateRegisters(const Maxwell3D::Registers &registers) {
        return {
            .colorRenderTargetsRegisters = util::MergeInto<REGTYPE(ColorRenderTargetState), type::ColorTargetCount>(*registers.colorTargets),
            .depthRenderTargetRegisters = {*registers.ztSize, *registers.ztOffset, *registers.ztFormat, *registers.ztBlockSize, *registers.ztArrayPitch, *registers.ztSelect, *registers.ztLayer},
            .vertexInputRegisters = {*registers.vertexStreams, *registers.vertexStreamInstance, *registers.vertexAttributes},
            .inputAssemblyRegisters = {*registers.primitiveRestartEnable},
            .tessellationRegisters = {*registers.patchSize, *registers.tessellationParameters},
            .rasterizationRegisters = {*registers.rasterEnable, *registers.frontPolygonMode, *registers.backPolygonMode, *registers.oglCullEnable, *registers.oglCullFace, *registers.windowOrigin, *registers.oglFrontFace, *registers.viewportClipControl, *registers.polyOffset, *registers.provokingVertex},
        };
    }

    static gpu::interconnect::maxwell3d::ActiveState::EngineRegisters MakeActiveStateRegisters(const Maxwell3D::Registers &registers) {
        return {
            .pipelineRegisters = MakePipelineStateRegisters(registers),
            .vertexBuffersRegisters = util::MergeInto<REGTYPE(VertexBufferState), type::VertexStreamCount>(*registers.vertexStreams, *registers.vertexStreamLimits),
            .indexBufferRegisters = {*registers.indexBuffer},
            .transformFeedbackBuffersRegisters = util::MergeInto<REGTYPE(TransformFeedbackBufferState), type::StreamOutBufferCount>(*registers.streamOutBuffers, *registers.streamOutEnable),
            .viewportsRegisters = util::MergeInto<REGTYPE(ViewportState), type::ViewportCount>(*registers.viewports, *registers.viewportClips, *registers.windowOrigin, *registers.viewportScaleOffsetEnable),
            .scissorsRegisters = util::MergeInto<REGTYPE(ScissorState), type::ViewportCount>(*registers.scissors),
            .lineWidthRegisters = {*registers.lineWidth, *registers.lineWidthAliased, *registers.aliasedLineWidthEnable},
            .depthBiasRegisters = {*registers.depthBias, *registers.depthBiasClamp, *registers.slopeScaleDepthBias},
            .blendConstantsRegisters = {*registers.blendConsts},
            .depthBoundsRegisters = {*registers.depthBoundsMin, *registers.depthBoundsMin},
            .stencilValuesRegisters = {*registers.stencilValues, *registers.backStencilValues, *registers.twoSidedStencilTestEnable},
        };
    }

    static gpu::interconnect::maxwell3d::Maxwell3D::EngineRegisterBundle MakeEngineRegisters(const Maxwell3D::Registers &registers) {
        return {
            .activeStateRegisters = MakeActiveStateRegisters(registers),
            .clearRegisters = {registers.scissors[0], registers.viewportClips[0], *registers.clearRect, *registers.colorClearValue, *registers.zClearValue, *registers.stencilClearValue, *registers.surfaceClip, *registers.clearSurfaceControl},
            .constantBufferSelectorRegisters = {*registers.constantBufferSelector}
        };
    }
    #undef REGTYPE

    type::DrawTopology Maxwell3D::GetCurrentTopology() {
        return registers.primitiveTopologyControl->override == type::PrimitiveTopologyControl::Override::UseTopologyInBeginMethods ?
                registers.begin->op : type::ConvertPrimitiveTopologyToDrawTopology(*registers.primitiveTopology);
    }

    Maxwell3D::Maxwell3D(const DeviceState &state, ChannelContext &channelCtx, MacroState &macroState, gpu::interconnect::CommandExecutor &executor)
        : MacroEngineBase{macroState},
          syncpoints{state.soc->host1x.syncpoints},
          i2m{channelCtx},
          dirtyManager{registers},
          interconnect{*state.gpu, channelCtx, executor, dirtyManager, MakeEngineRegisters(registers)},
          channelCtx{channelCtx} {
        executor.AddFlushCallback([this]() { FlushEngineState(); });
        InitializeRegisters();
    }

    __attribute__((always_inline)) void Maxwell3D::FlushDeferredDraw() {
        if (deferredDraw.pending) {
            deferredDraw.pending = false;
            interconnect.Draw(deferredDraw.drawTopology, deferredDraw.indexed, deferredDraw.drawCount, deferredDraw.drawFirst, deferredDraw.instanceCount, deferredDraw.drawBaseVertex, deferredDraw.drawBaseInstance);
            deferredDraw.instanceCount = 1;
        }
    }

    __attribute__((always_inline)) void Maxwell3D::HandleMethod(u32 method, u32 argument) {
        if (method == ENGINE_STRUCT_OFFSET(mme, shadowRamControl)) [[unlikely]] {
            shadowRegisters.raw[method] = registers.raw[method] = argument;
            return;
        }

        if (shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodTrack || shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodTrackWithFilter) [[unlikely]]
            shadowRegisters.raw[method] = argument;
        else if (shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodReplay) [[unlikely]]
            argument = shadowRegisters.raw[method];


        bool redundant{registers.raw[method] == argument};
        registers.raw[method] = argument;

        // TODO COMBINE THESE
        if (batchLoadConstantBuffer.Active()) {
            switch (method) {
                // Add to the batch constant buffer update buffer
                // Return early here so that any code below can rely on the fact that any cbuf updates will always be the first of a batch
                #define LOAD_CONSTANT_BUFFER_CALLBACKS(z, index, data_)                \
                ENGINE_STRUCT_ARRAY_CASE(loadConstantBuffer, data, index, { \
                    batchLoadConstantBuffer.buffer.push_back(argument);         \
                    registers.loadConstantBuffer->offset += 4;              \
                    return;                                                   \
                })

                BOOST_PP_REPEAT(16, LOAD_CONSTANT_BUFFER_CALLBACKS, 0)
                #undef LOAD_CONSTANT_BUFFER_CALLBACKS
                default:
                    // When a method other than constant buffer update is called submit our submit the previously built-up update as a batch
                    interconnect.LoadConstantBuffer(batchLoadConstantBuffer.buffer, batchLoadConstantBuffer.Invalidate());
                    batchLoadConstantBuffer.Reset();
                    break; // Continue on here to handle the actual method
            }
        } else if (deferredDraw.pending) { // See DeferredDrawState comment for full details
            switch (method) {
                ENGINE_CASE(begin, {
                    if (begin.instanceId == Registers::Begin::InstanceId::Subsequent) {
                        if (deferredDraw.drawTopology != begin.op &&
                            registers.primitiveTopologyControl->override == type::PrimitiveTopologyControl::Override::UseTopologyInBeginMethods)
                            Logger::Warn("Vertex topology changed partway through instanced draw!");

                        deferredDraw.instanceCount++;
                    } else {
                        FlushDeferredDraw();
                        break; // This instanced draw is finished, continue on to handle the next draw
                    }

                    return;
                })

                // Can be ignored since we handle drawing in draw{Vertex,Index}Count
                ENGINE_CASE(end, { return; })

                // Draws here can be ignored since they're just repeats of the original instanced draw
                ENGINE_CASE(drawVertexArray, {
                    if (!redundant)
                        Logger::Warn("Vertex count changed partway through instanced draw!");
                    return;
                })
                ENGINE_CASE(drawIndexBuffer, {
                    if (!redundant)
                        Logger::Warn("Index count changed partway through instanced draw!");
                    return;
                })

                // Once we stop calling draw methods flush the current draw since drawing is dependent on the register state not changing
                default:
                    FlushDeferredDraw();
                    break;
            }
        }


        if (!redundant) {
            dirtyManager.MarkDirty(method);

            switch (method) {
                #define VERTEX_STREAM_CALLBACKS(z, idx, data)                                     \
                ENGINE_ARRAY_STRUCT_CASE(vertexStreams, idx, format, {                            \
                    interconnect.directState.vertexInput.SetStride(idx, format.stride);           \
                })                                                                                \
                ENGINE_ARRAY_STRUCT_CASE(vertexStreams, idx, frequency, {                         \
                    interconnect.directState.vertexInput.SetDivisor(idx, frequency);              \
                })                                                                                \
                ENGINE_ARRAY_CASE(vertexStreamInstance, idx, {                                    \
                    interconnect.directState.vertexInput.SetInputRate(idx, vertexStreamInstance); \
                })

                BOOST_PP_REPEAT(16, VERTEX_STREAM_CALLBACKS, 0)
                static_assert(type::VertexStreamCount == 16 && type::VertexStreamCount < BOOST_PP_LIMIT_REPEAT);
                #undef VERTEX_STREAM_CALLBACKS


                #define VERTEX_ATTRIBUTE_CALLBACKS(z, idx, data)                              \
                ENGINE_ARRAY_CASE(vertexAttributes, idx, {                                    \
                    interconnect.directState.vertexInput.SetAttribute(idx, vertexAttributes); \
                })

                BOOST_PP_REPEAT(16, VERTEX_ATTRIBUTE_CALLBACKS, 0)
                static_assert(type::VertexAttributeCount == 32 && type::VertexAttributeCount < BOOST_PP_LIMIT_REPEAT);
                #undef VERTEX_ATTRIBUTE_CALLBACKS


                ENGINE_CASE(primitiveRestartEnable, {
                    interconnect.directState.inputAssembly.SetPrimitiveRestart(primitiveRestartEnable != 0);
                })


                ENGINE_CASE(tessellationParameters, {
                    interconnect.directState.tessellation.SetParameters(tessellationParameters);
                })

                ENGINE_CASE(patchSize, {
                    interconnect.directState.tessellation.SetPatchControlPoints(patchSize);
                })

                default:
                    break;
            }
        }

        switch (method) {
            ENGINE_STRUCT_CASE(mme, instructionRamLoad, {
                if (registers.mme->instructionRamPointer >= macroState.macroCode.size())
                    throw exception("Macro memory is full!");

                macroState.macroCode[registers.mme->instructionRamPointer++] = instructionRamLoad;

                // Wraparound writes
                // This works on HW but will also generate an error interrupt
                registers.mme->instructionRamPointer %= macroState.macroCode.size();
            })

            ENGINE_STRUCT_CASE(mme, startAddressRamLoad, {
                if (registers.mme->startAddressRamPointer >= macroState.macroPositions.size())
                    throw exception("Maximum amount of macros reached!");

                macroState.macroPositions[registers.mme->startAddressRamPointer++] = startAddressRamLoad;
            })

            ENGINE_STRUCT_CASE(i2m, launchDma, {
                i2m.LaunchDma(*registers.i2m);
            })

            ENGINE_STRUCT_CASE(i2m, loadInlineData, {
                i2m.LoadInlineData(*registers.i2m, loadInlineData);
            })

            ENGINE_CASE(syncpointAction, {
                Logger::Debug("Increment syncpoint: {}", static_cast<u16>(syncpointAction.id));
                channelCtx.executor.Submit();
                syncpoints.at(syncpointAction.id).Increment();
            })

            ENGINE_CASE(clearSurface, {
                interconnect.Clear(clearSurface);
            })

            ENGINE_CASE(begin, {
                // If we reach here then we aren't in a deferred draw so theres no need to flush anything
                if (begin.instanceId == Registers::Begin::InstanceId::Subsequent)
                    deferredDraw.instanceCount++;
                else
                    deferredDraw.instanceCount = 1;
            })

            ENGINE_STRUCT_CASE(drawVertexArray, count, {
                // Defer the draw until the first non-draw operation to allow for detecting instanced draws (see DeferredDrawState comment)
                deferredDraw.Set(count, *registers.vertexArrayStart, 0, *registers.globalBaseInstanceIndex, GetCurrentTopology(), false);
            })

            ENGINE_STRUCT_CASE(drawIndexBuffer, count, {
                // Defer the draw until the first non-draw operation to allow for detecting instanced draws (see DeferredDrawState comment)
                deferredDraw.Set(count, registers.indexBuffer->first, *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex, GetCurrentTopology(), true);
            })

            ENGINE_STRUCT_CASE(semaphore, info, {
                if (info.reductionEnable)
                    Logger::Warn("Semaphore reduction is unimplemented!");

                switch (info.op) {
                    case type::SemaphoreInfo::Op::Release:
                        channelCtx.executor.Submit();
                        WriteSemaphoreResult(registers.semaphore->payload);
                        break;

                    case type::SemaphoreInfo::Op::Counter: {
                        switch (info.counterType) {
                            case type::SemaphoreInfo::CounterType::Zero:
                                WriteSemaphoreResult(registers.semaphore->payload);
                                break;

                            default:
                                //Logger::Warn("Unsupported semaphore counter type: 0x{:X}", static_cast<u8>(info.counterType));
                                break;
                        }
                        break;
                    }

                    default:
                        Logger::Warn("Unsupported semaphore operation: 0x{:X}", static_cast<u8>(info.op));
                        break;
                }
            })

            ENGINE_ARRAY_CASE(firmwareCall, 4, {
                registers.raw[0xD00] = 1;
            })

            // Begin a batch constant buffer update, this case will never be reached if a batch update is currently active
            #define LOAD_CONSTANT_BUFFER_CALLBACKS(z, index, data_)                                      \
            ENGINE_STRUCT_ARRAY_CASE(loadConstantBuffer, data, index, {                       \
                batchLoadConstantBuffer.startOffset = registers.loadConstantBuffer->offset; \
                batchLoadConstantBuffer.buffer.push_back(data);                               \
                registers.loadConstantBuffer->offset += 4;                                    \
            })

            BOOST_PP_REPEAT(16, LOAD_CONSTANT_BUFFER_CALLBACKS, 0)
            #undef LOAD_CONSTANT_BUFFER_CALLBACKS

            #define PIPELINE_CALLBACKS(z, idx, data)                                                                                         \
                ENGINE_ARRAY_STRUCT_CASE(bindGroups, idx, constantBuffer, {                                                                  \
                    interconnect.BindConstantBuffer(static_cast<type::PipelineStage>(idx), constantBuffer.shaderSlot, constantBuffer.valid); \
                })

            BOOST_PP_REPEAT(5, PIPELINE_CALLBACKS, 0)
            static_assert(type::PipelineStageCount == 5 && type::PipelineStageCount < BOOST_PP_LIMIT_REPEAT);
            #undef PIPELINE_CALLBACKS
            default:
                break;
        }
    }

    void Maxwell3D::WriteSemaphoreResult(u64 result) {
        u64 address{registers.semaphore->address};

        switch (registers.semaphore->info.structureSize) {
            case type::SemaphoreInfo::StructureSize::OneWord:
                channelCtx.asCtx->gmmu.Write(address, static_cast<u32>(result));
                Logger::Debug("address: 0x{:X} payload: {}", address, result);
                break;

            case type::SemaphoreInfo::StructureSize::FourWords: {
                // Write timestamp first to ensure correct ordering
                u64 timestamp{GetGpuTimeTicks()};
                channelCtx.asCtx->gmmu.Write(address + 8, timestamp);
                channelCtx.asCtx->gmmu.Write(address, result);
                Logger::Debug("address: 0x{:X} payload: {} timestamp: {}", address, result, timestamp);

                break;
            }
        }
    }

    void Maxwell3D::FlushEngineState() {
        FlushDeferredDraw();

        if (batchLoadConstantBuffer.Active()) {
            interconnect.LoadConstantBuffer(batchLoadConstantBuffer.buffer, batchLoadConstantBuffer.Invalidate());
            batchLoadConstantBuffer.Reset();
        }
    }

    __attribute__((always_inline)) void Maxwell3D::CallMethod(u32 method, u32 argument) {
        Logger::Verbose("Called method in Maxwell 3D: 0x{:X} args: 0x{:X}", method, argument);

        HandleMethod(method, argument);
    }

    void Maxwell3D::CallMethodBatchNonInc(u32 method, span<u32> arguments) {
        switch (method) {
            case ENGINE_STRUCT_OFFSET(i2m, loadInlineData):
                i2m.LoadInlineData(*registers.i2m, arguments);
                return;
            default:
                break;
        }

        for (u32 argument : arguments)
            HandleMethod(method, argument);
    }

    void Maxwell3D::CallMethodFromMacro(u32 method, u32 argument) {
        HandleMethod(method, argument);
    }

    u32 Maxwell3D::ReadMethodFromMacro(u32 method) {
        return registers.raw[method];
    }
}
