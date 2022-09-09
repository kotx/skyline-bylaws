// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 yuzu Team and Contributors (https://github.com/yuzu-emu/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <optional>
#include <range/v3/algorithm/for_each.hpp>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include <gpu/interconnect/command_executor.h>
#include <gpu/texture/format.h>
#include <gpu.h>
#include <vulkan/vulkan_core.h>
#include "pipeline_state.h"
#include "shader_state.h"
#include "soc/gm20b/engines/maxwell/types.h"

namespace skyline::gpu::interconnect::maxwell3d {
    /* Packed State */
    void PackedPipelineState::SetColorRenderTargetFormat(size_t index, engine::ColorTarget::Format format) {
        colorRenderTargetFormats[index] = static_cast<u8>(format);
    }

    void PackedPipelineState::SetDepthRenderTargetFormat(engine::ZtFormat format) {
        depthRenderTargetFormat = static_cast<u8>(format) - static_cast<u8>(engine::ZtFormat::ZF32);
    }

    void PackedPipelineState::SetVertexBinding(u32 index, engine::VertexStream stream, engine::VertexStreamInstance instance) {
        vertexBindings[index].stride = stream.format.stride;
        vertexBindings[index].inputRate = instance.isInstanced ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex;
        vertexBindings[index].enable = stream.format.enable;
        vertexBindings[index].divisor = stream.frequency;
    }

    void PackedPipelineState::SetTessellationParameters(engine::TessellationParameters parameters) {
        domainType = parameters.domainType;
        spacing = parameters.spacing;
        outputPrimitives = parameters.outputPrimitives;
    }

    void PackedPipelineState::SetPolygonMode(engine::PolygonMode mode) {
        switch (mode) {
            case engine::PolygonMode::Fill:
                polygonMode = vk::PolygonMode::eFill;
            case engine::PolygonMode::Line:
                polygonMode = vk::PolygonMode::eLine;
            case engine::PolygonMode::Point:
                polygonMode = vk::PolygonMode::ePoint;
            default:
                throw exception("Invalid polygon mode: 0x{:X}", static_cast<u32>(mode));
        }
    }

    void PackedPipelineState::SetCullMode(bool enable, engine::CullFace mode) {
        if (!enable) {
            cullMode = {};
            return;
        }

        switch (mode) {
            case engine::CullFace::Front:
                cullMode = VK_CULL_MODE_FRONT_BIT;
            case engine::CullFace::Back:
                cullMode = VK_CULL_MODE_BACK_BIT;
            case engine::CullFace::FrontAndBack:
                cullMode = VK_CULL_MODE_FRONT_BIT | VK_CULL_MODE_BACK_BIT;
            default:
                throw exception("Invalid cull mode: 0x{:X}", static_cast<u32>(mode));
        }
    }


    static vk::CompareOp ConvertCompareFunc(engine::CompareFunc func) {
        if (func < engine::CompareFunc::D3DNever || func > engine::CompareFunc::OglAlways || (func > engine::CompareFunc::D3DAlways && func < engine::CompareFunc::OglNever))
            throw exception("Invalid comparision function: 0x{:X}", static_cast<u32>(func));

        u32 val{static_cast<u32>(func)};

        // VK CompareOp values match 1:1 with Maxwell with some small maths
        return static_cast<vk::CompareOp>(func >= engine::CompareFunc::OglNever ? val - 0x200 : val - 1);
    }

    void PackedPipelineState::SetDepthFunc(engine::CompareFunc func) {
        depthFunc = ConvertCompareFunc(func);
    }

    void PackedPipelineState::SetLogicOp(engine::LogicOp::Func op) {
        if (op < engine::LogicOp::Func::Clear || op > engine::LogicOp::Func::Set)
            throw exception("Invalid logical operation: 0x{:X}", val);

        // VK LogicOp values match 1:1 with Maxwell
        logicOp = static_cast<vk::LogicOp>(static_cast<u32>(op) - static_cast<u32>(engine::LogicOp::Func::Clear));
    }

    static vk::StencilOp ConvertStencilOp(engine::StencilOps::Op op) {
        switch (op) {
            case engine::StencilOps::Op::OglZero:
            case engine::StencilOps::Op::D3DZero:
                return vk::StencilOp::eZero;
            case engine::StencilOps::Op::D3DKeep:
            case engine::StencilOps::Op::OglKeep:
                return vk::StencilOp::eKeep;
            case engine::StencilOps::Op::D3DReplace:
            case engine::StencilOps::Op::OglReplace:
                return vk::StencilOp::eReplace;
            case engine::StencilOps::Op::D3DIncrSat:
            case engine::StencilOps::Op::OglIncrSat:
                return vk::StencilOp::eIncrementAndClamp;
            case engine::StencilOps::Op::D3DDecrSat:
            case engine::StencilOps::Op::OglDecrSat:
                return vk::StencilOp::eDecrementAndClamp;
            case engine::StencilOps::Op::D3DInvert:
            case engine::StencilOps::Op::OglInvert:
                return vk::StencilOp::eInvert;
            case engine::StencilOps::Op::D3DIncr:
            case engine::StencilOps::Op::OglIncr:
                return vk::StencilOp::eIncrementAndWrap;
            case engine::StencilOps::Op::D3DDecr:
            case engine::StencilOps::Op::OglDecr:
                return vk::StencilOp::eDecrementAndWrap;
            default:
                throw exception("Invalid stencil operation: 0x{:X}", static_cast<u32>(op));
        }
    }

    static PackedPipelineState::StencilOps PackStencilOps(engine::StencilOps ops) {
        return {
            .zPass = ConvertStencilOp(ops.zPass),
            .fail = ConvertStencilOp(ops.fail),
            .zFail = ConvertStencilOp(ops.zFail),
            .func = ConvertCompareFunc(ops.func),
        };
    }

    void PackedPipelineState::SetStencilOps(engine::StencilOps front, engine::StencilOps back) {
        stencilFront = PackStencilOps(front);
        stencilBack = PackStencilOps(back);
    }

    static VkColorComponentFlags ConvertColorWriteMask(engine::CtWrite write) {
        return (write.rEnable ? VK_COLOR_COMPONENT_R_BIT : 0) |
               (write.gEnable ? VK_COLOR_COMPONENT_G_BIT : 0) |
               (write.bEnable ? VK_COLOR_COMPONENT_B_BIT : 0) |
               (write.aEnable ? VK_COLOR_COMPONENT_A_BIT : 0);
    };

    static vk::BlendOp ConvertBlendOp(engine::BlendOp op) {
        switch (op) {
            case engine::BlendOp::D3DAdd:
            case engine::BlendOp::OglFuncAdd:
                return vk::BlendOp::eAdd;
            case engine::BlendOp::D3DSubtract:
            case engine::BlendOp::OglFuncSubtract:
                return vk::BlendOp::eSubtract;
            case engine::BlendOp::D3DRevSubtract:
            case engine::BlendOp::OglFuncReverseSubtract:
                return vk::BlendOp::eReverseSubtract;
            case engine::BlendOp::D3DMin:
            case engine::BlendOp::OglMin:
                return vk::BlendOp::eMin;
            case engine::BlendOp::D3DMax:
            case engine::BlendOp::OglMax:
                return vk::BlendOp::eMax;
            default:
                throw exception("Invalid blend operation: 0x{:X}", static_cast<u32>(op));
        }
    }

    static vk::BlendFactor ConvertBlendFactor(engine::BlendCoeff coeff) {
        switch (coeff) {
            case engine::BlendCoeff::OglZero:
            case engine::BlendCoeff::D3DZero:
                return vk::BlendFactor::eZero;
            case engine::BlendCoeff::OglOne:
            case engine::BlendCoeff::D3DOne:
                return vk::BlendFactor::eOne;
            case engine::BlendCoeff::OglSrcColor:
            case engine::BlendCoeff::D3DSrcColor:
                return vk::BlendFactor::eSrcColor;
            case engine::BlendCoeff::OglOneMinusSrcColor:
            case engine::BlendCoeff::D3DInvSrcColor:
                return vk::BlendFactor::eOneMinusSrcColor;
            case engine::BlendCoeff::OglSrcAlpha:
            case engine::BlendCoeff::D3DSrcAlpha:
                return vk::BlendFactor::eSrcAlpha;
            case engine::BlendCoeff::OglOneMinusSrcAlpha:
            case engine::BlendCoeff::D3DInvSrcAlpha:
                return vk::BlendFactor::eOneMinusSrcAlpha;
            case engine::BlendCoeff::OglDstAlpha:
            case engine::BlendCoeff::D3DDstAlpha:
                return vk::BlendFactor::eDstAlpha;
            case engine::BlendCoeff::OglOneMinusDstAlpha:
            case engine::BlendCoeff::D3DInvDstAlpha:
                return vk::BlendFactor::eOneMinusDstAlpha;
            case engine::BlendCoeff::OglDstColor:
            case engine::BlendCoeff::D3DDstColor:
                return vk::BlendFactor::eDstColor;
            case engine::BlendCoeff::OglOneMinusDstColor:
            case engine::BlendCoeff::D3DInvDstColor:
                return vk::BlendFactor::eOneMinusDstColor;
            case engine::BlendCoeff::OglSrcAlphaSaturate:
            case engine::BlendCoeff::D3DSrcAlphaSaturate:
                return vk::BlendFactor::eSrcAlphaSaturate;
            case engine::BlendCoeff::OglConstantColor:
            case engine::BlendCoeff::D3DBlendCoeff:
                return vk::BlendFactor::eConstantColor;
            case engine::BlendCoeff::OglOneMinusConstantColor:
            case engine::BlendCoeff::D3DInvBlendCoeff:
                return vk::BlendFactor::eOneMinusConstantColor;
            case engine::BlendCoeff::OglConstantAlpha:
                return vk::BlendFactor::eConstantAlpha;
            case engine::BlendCoeff::OglOneMinusConstantAlpha:
                return vk::BlendFactor::eOneMinusConstantAlpha;
            case engine::BlendCoeff::OglSrc1Color:
            case engine::BlendCoeff::D3DSrc1Color:
                return vk::BlendFactor::eSrc1Color;
            case engine::BlendCoeff::OglInvSrc1Color:
            case engine::BlendCoeff::D3DInvSrc1Color:
                return vk::BlendFactor::eOneMinusSrc1Color;
            case engine::BlendCoeff::OglSrc1Alpha:
            case engine::BlendCoeff::D3DSrc1Alpha:
                return vk::BlendFactor::eSrc1Alpha;
            case engine::BlendCoeff::OglInvSrc1Alpha:
            case engine::BlendCoeff::D3DInvSrc1Alpha:
                return vk::BlendFactor::eOneMinusSrc1Alpha;
            default:
                throw exception("Invalid blend coefficient type: 0x{:X}", static_cast<u32>(coeff));
        }
    }

    static PackedPipelineState::AttachmentBlendState PackAttachmentBlendState(bool enable, engine::CtWrite writeMask, auto blend) {
        return {
            .colorWriteMask = ConvertColorWriteMask(writeMask),
            .colorBlendOp = ConvertBlendOp(blend.colorOp),
            .srcColorBlendFactor = ConvertBlendFactor(blend.colorSourceCoeff),
            .dstColorBlendFactor = ConvertBlendFactor(blend.colorDestCoeff),
            .alphaBlendOp = ConvertBlendOp(blend.alphaOp),
            .srcAlphaBlendFactor = ConvertBlendFactor(blend.alphaSourceCoeff),
            .dstAlphaBlendFactor = ConvertBlendFactor(blend.alphaDestCoeff),
            .blendEnable = enable
        };
    }

    void PackedPipelineState::SetAttachmentBlendState(u32 index, bool enable, engine::CtWrite writeMask, engine::Blend blend) {
        attachmentBlendStates[index] = PackAttachmentBlendState(enable, writeMask, blend);
    }

    void PackedPipelineState::SetAttachmentBlendState(u32 index, bool enable, engine::CtWrite writeMask, engine::BlendPerTarget blend) {
        attachmentBlendStates[index] = PackAttachmentBlendState(enable, writeMask, blend);
    }

    /* Colour Render Target */
    void ColorRenderTargetState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, colorTarget);
    }

    ColorRenderTargetState::ColorRenderTargetState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine, size_t index) : engine{manager, dirtyHandle, engine}, index{index} {}

    static texture::Format ConvertColorRenderTargetFormat(engine::ColorTarget::Format format) {
        #define FORMAT_CASE_BASE(engineFormat, skFormat, warn) \
                case engine::ColorTarget::Format::engineFormat:                     \
                    if constexpr (warn)                                             \
                        Logger::Warn("Partially supported RT format: " #engineFormat " used!"); \
                    return skyline::gpu::format::skFormat

        #define FORMAT_CASE(engineFormat, skFormat) FORMAT_CASE_BASE(engineFormat, skFormat, false)
        #define FORMAT_CASE_WARN(engineFormat, skFormat) FORMAT_CASE_BASE(engineFormat, skFormat, true)

        switch (format) {
            FORMAT_CASE(RF32_GF32_BF32_AF32, R32G32B32A32Float);
            FORMAT_CASE(RS32_GS32_BS32_AS32, R32G32B32A32Sint);
            FORMAT_CASE(RU32_GU32_BU32_AU32, R32G32B32A32Uint);
            FORMAT_CASE_WARN(RF32_GF32_BF32_X32, R32G32B32A32Float); // TODO: ignore X32 component with blend
            FORMAT_CASE_WARN(RS32_GS32_BS32_X32, R32G32B32A32Sint); // TODO: ^
            FORMAT_CASE_WARN(RU32_GU32_BU32_X32, R32G32B32A32Uint); // TODO: ^
            FORMAT_CASE(R16_G16_B16_A16, R16G16B16A16Unorm);
            FORMAT_CASE(RN16_GN16_BN16_AN16, R16G16B16A16Snorm);
            FORMAT_CASE(RS16_GS16_BS16_AS16, R16G16B16A16Sint);
            FORMAT_CASE(RU16_GU16_BU16_AU16, R16G16B16A16Uint);
            FORMAT_CASE(RF16_GF16_BF16_AF16, R16G16B16A16Float);
            FORMAT_CASE(RF32_GF32, R32G32Float);
            FORMAT_CASE(RS32_GS32, R32G32Sint);
            FORMAT_CASE(RU32_GU32, R32G32Uint);
            FORMAT_CASE_WARN(RF16_GF16_BF16_X16, R16G16B16A16Float); // TODO: ^^
            FORMAT_CASE(A8R8G8B8, B8G8R8A8Unorm);
            FORMAT_CASE(A8RL8GL8BL8, B8G8R8A8Srgb);
            FORMAT_CASE(A2B10G10R10, A2B10G10R10Unorm);
            FORMAT_CASE(AU2BU10GU10RU10, A2B10G10R10Uint);
            FORMAT_CASE(A8B8G8R8, R8G8B8A8Unorm);
            FORMAT_CASE(A8BL8GL8RL8, R8G8B8A8Srgb);
            FORMAT_CASE(AN8BN8GN8RN8, R8G8B8A8Snorm);
            FORMAT_CASE(AS8BS8GS8RS8, R8G8B8A8Sint);
            FORMAT_CASE(R16_G16, R16G16Unorm);
            FORMAT_CASE(RN16_GN16, R16G16Snorm);
            FORMAT_CASE(RS16_GS16, R16G16Sint);
            FORMAT_CASE(RU16_GU16, R16G16Uint);
            FORMAT_CASE(RF16_GF16, R16G16Float);
            FORMAT_CASE(A2R10G10B10, A2B10G10R10Unorm);
            FORMAT_CASE(BF10GF11RF11, B10G11R11Float);
            FORMAT_CASE(RS32, R32Sint);
            FORMAT_CASE(RU32, R32Uint);
            FORMAT_CASE(RF32, R32Float);
            FORMAT_CASE_WARN(X8R8G8B8, B8G8R8A8Unorm); // TODO: ^^
            FORMAT_CASE_WARN(X8RL8GL8BL8, B8G8R8A8Srgb); // TODO: ^^
            FORMAT_CASE(R5G6B5, R5G6B5Unorm);
            FORMAT_CASE(A1R5G5B5, A1R5G5B5Unorm);
            FORMAT_CASE(G8R8, R8G8Unorm);
            FORMAT_CASE(GN8RN8, R8G8Snorm);
            FORMAT_CASE(GS8RS8, R8G8Sint);
            FORMAT_CASE(GU8RU8, R8G8Uint);
            FORMAT_CASE(R16, R16Unorm);
            FORMAT_CASE(RN16, R16Snorm);
            FORMAT_CASE(RS16, R16Sint);
            FORMAT_CASE(RU16, R16Uint);
            FORMAT_CASE(RF16, R16Float);
            FORMAT_CASE(R8, R8Unorm);
            FORMAT_CASE(RN8, R8Snorm);
            FORMAT_CASE(RS8, R8Sint);
            FORMAT_CASE(RU8, R8Uint);
            // FORMAT_CASE(A8, A8Unorm);
            FORMAT_CASE_WARN(X1R5G5B5, A1R5G5B5Unorm); // TODO: ^^
            FORMAT_CASE_WARN(X8B8G8R8, R8G8B8A8Unorm); // TODO: ^^
            FORMAT_CASE_WARN(X8BL8GL8RL8, R8G8B8A8Srgb); // TODO: ^^
            FORMAT_CASE_WARN(Z1R5G5B5, A1R5G5B5Unorm); // TODO: ^^ but with zero blend
            FORMAT_CASE_WARN(O1R5G5B5, A1R5G5B5Unorm); // TODO: ^^ but with one blend
            FORMAT_CASE_WARN(Z8R8G8B8, B8G8R8A8Unorm); // TODO: ^^ but with zero blend
            FORMAT_CASE_WARN(O8R8G8B8, B8G8R8A8Unorm); // TODO: ^^ but with one blend
            // FORMAT_CASE(R32, R32Unorm);
            // FORMAT_CASE(A16, A16Unorm);
            // FORMAT_CASE(AF16, A16Float);
            // FORMAT_CASE(AF32, A32Float);
            // FORMAT_CASE(A8R8, R8A8Unorm);
            // FORMAT_CASE(R16_A16, R16A16Unorm);
            // FORMAT_CASE(RF16_AF16, R16A16Float);
            // FORMAT_CASE(RF32_AF32, R32A32Float);
            // FORMAT_CASE(B8G8R8A8, A8R8G8B8Unorm)
            default:
                throw exception("Unsupported colour rendertarget format: 0x{:X}", static_cast<u32>(format));
        }

        #undef FORMAT_CASE
        #undef FORMAT_CASE_WARN
        #undef FORMAT_CASE_BASE
    }

    void ColorRenderTargetState::Flush(InterconnectContext &ctx, PackedPipelineState &packedState) {
        auto &target{engine->colorTarget};
        packedState.SetColorRenderTargetFormat(index, target.format);

        if (target.format == engine::ColorTarget::Format::Disabled) {
            view = {};
            return;
        }

        GuestTexture guest{};
        guest.format = ConvertColorRenderTargetFormat(target.format);
        guest.aspect = vk::ImageAspectFlagBits::eColor;
        guest.baseArrayLayer = target.layerOffset;

        bool thirdDimensionDefinesArraySize{target.memory.thirdDimensionControl == engine::TargetMemory::ThirdDimensionControl::ThirdDimensionDefinesArraySize};
        guest.layerCount = thirdDimensionDefinesArraySize ? target.thirdDimension : 1;
        guest.viewType = target.thirdDimension > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;

        u32 depth{thirdDimensionDefinesArraySize ? 1U : target.thirdDimension};
        if (target.memory.layout == engine::TargetMemory::Layout::Pitch) {
            guest.dimensions = texture::Dimensions{target.width / guest.format->bpb, target.height, depth};
            guest.tileConfig = texture::TileConfig{
                .mode = gpu::texture::TileMode::Linear,
            };
        } else {
            guest.dimensions = gpu::texture::Dimensions{target.width, target.height, depth};
            guest.tileConfig = gpu::texture::TileConfig{
                .mode = gpu::texture::TileMode::Block,
                .blockHeight = target.memory.BlockHeight(),
                .blockDepth = target.memory.BlockDepth(),
            };
        }

        guest.layerStride = (guest.baseArrayLayer > 1 || guest.layerCount > 1) ? target.ArrayPitch() : 0;

        auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(target.offset, guest.GetSize())};
        guest.mappings.assign(mappings.begin(), mappings.end());

        view = ctx.executor.AcquireTextureManager().FindOrCreate(guest, ctx.executor.tag);
    }

    /* Depth Render Target */
    void DepthRenderTargetState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, ztSize, ztOffset, ztFormat, ztBlockSize, ztArrayPitch, ztSelect, ztLayer);
    }

    DepthRenderTargetState::DepthRenderTargetState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    static texture::Format ConvertDepthRenderTargetFormat(engine::ZtFormat format) {
        #define FORMAT_CASE(engineFormat, skFormat) \
            case engine::ZtFormat::engineFormat: \
                return skyline::gpu::format::skFormat

        switch (format) {
            FORMAT_CASE(Z16, D16Unorm);
            FORMAT_CASE(Z24S8, S8UintD24Unorm);
            FORMAT_CASE(X8Z24, D24UnormX8Uint);
            FORMAT_CASE(S8Z24, D24UnormS8Uint);
            FORMAT_CASE(S8, S8Uint);
            FORMAT_CASE(ZF32, D32Float);
            FORMAT_CASE(ZF32_X24S8, D32FloatS8Uint);
            default:
                throw exception("Unsupported depth rendertarget format: 0x{:X}", static_cast<u32>(format));
        }

        #undef FORMAT_CASE
    }

    void DepthRenderTargetState::Flush(InterconnectContext &ctx, PackedPipelineState &packedState) {
        packedState.SetDepthRenderTargetFormat(engine->ztFormat);

        if (!engine->ztSelect.targetCount) {
            view = {};
            return;
        }

        GuestTexture guest{};
        guest.format = ConvertDepthRenderTargetFormat(engine->ztFormat);
        guest.aspect = guest.format->vkAspect;
        guest.baseArrayLayer = engine->ztLayer.offset;

        bool thirdDimensionDefinesArraySize{engine->ztSize.control == engine::ZtSize::Control::ThirdDimensionDefinesArraySize};
        if (engine->ztSize.control == engine::ZtSize::Control::ThirdDimensionDefinesArraySize) {
            guest.layerCount = engine->ztSize.thirdDimension;
            guest.viewType = vk::ImageViewType::e2DArray;
        } else if (engine->ztSize.control == engine::ZtSize::Control::ArraySizeIsOne) {
            guest.layerCount = 1;
            guest.viewType = vk::ImageViewType::e2D;
        }

        guest.dimensions = gpu::texture::Dimensions{engine->ztSize.width, engine->ztSize.height, 1};
        guest.tileConfig = gpu::texture::TileConfig{
            .mode = gpu::texture::TileMode::Block,
            .blockHeight = engine->ztBlockSize.BlockHeight(),
            .blockDepth = engine->ztBlockSize.BlockDepth(),
        };

        guest.layerStride = (guest.baseArrayLayer > 1 || guest.layerCount > 1) ? engine->ztArrayPitch : 0;

        auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(engine->ztOffset, guest.GetSize())};
        guest.mappings.assign(mappings.begin(), mappings.end());

        view = ctx.executor.AcquireTextureManager().FindOrCreate(guest, ctx.executor.tag);
    }

    /* Vertex Input State */
    // TODO: check if better individually
    void VertexInputState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        ranges::for_each(vertexStreams, [&](const auto &regs) { manager.Bind(handle, regs.format, regs.frequency); });

        auto bindFull{[&](const auto &regs) { manager.Bind(handle, regs); }};
        ranges::for_each(vertexStreamInstance, bindFull);
        ranges::for_each(vertexAttributes, bindFull);
    }

    VertexInputState::VertexInputState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void VertexInputState::Flush(PackedPipelineState &packedState) {
        for (u32 i{}; i < engine::VertexStreamCount; i++)
            packedState.SetVertexBinding(i, engine->vertexStreams[i], engine->vertexStreamInstance[i]);

        for (u32 i{}; i < engine::VertexAttributeCount; i++)
            packedState.vertexAttributes[i] = engine->vertexAttributes[i];
    }

    static vk::Format ConvertVertexInputAttributeFormat(engine::VertexAttribute::ComponentBitWidths componentBitWidths, engine::VertexAttribute::NumericalType numericalType) {
        #define FORMAT_CASE(bitWidths, type, vkType, vkFormat, ...) \
            case engine::VertexAttribute::ComponentBitWidths::bitWidths | engine::VertexAttribute::NumericalType::type: \
                return vk::Format::vkFormat ## vkType ##__VA_ARGS__

        #define FORMAT_INT_CASE(size, vkFormat, ...) \
            FORMAT_CASE(size, Uint, Uint, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Sint, Sint, vkFormat, ##__VA_ARGS__);

        #define FORMAT_INT_FLOAT_CASE(size, vkFormat, ...) \
            FORMAT_INT_CASE(size, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Float, Sfloat, vkFormat, ##__VA_ARGS__);

        #define FORMAT_NORM_INT_SCALED_CASE(size, vkFormat, ...) \
            FORMAT_INT_CASE(size, vkFormat, ##__VA_ARGS__);               \
            FORMAT_CASE(size, Unorm, Unorm, vkFormat, ##__VA_ARGS__);     \
            FORMAT_CASE(size, Snorm, Unorm, vkFormat, ##__VA_ARGS__);     \
            FORMAT_CASE(size, Uscaled, Uscaled, vkFormat, ##__VA_ARGS__); \
            FORMAT_CASE(size, Sscaled, Sscaled, vkFormat, ##__VA_ARGS__)

        #define FORMAT_NORM_INT_SCALED_FLOAT_CASE(size, vkFormat) \
            FORMAT_NORM_INT_SCALED_CASE(size, vkFormat); \
            FORMAT_CASE(size, Float, Sfloat, vkFormat)

        switch (componentBitWidths | numericalType) {
            /* 8-bit components */
            FORMAT_NORM_INT_SCALED_CASE(R8, eR8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8, eR8G8);
            FORMAT_NORM_INT_SCALED_CASE(G8R8, eR8G8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8_B8, eR8G8B8);
            FORMAT_NORM_INT_SCALED_CASE(R8_G8_B8_A8, eR8G8B8A8);
            FORMAT_NORM_INT_SCALED_CASE(A8B8G8R8, eR8G8B8A8);
            FORMAT_NORM_INT_SCALED_CASE(X8B8G8R8, eR8G8B8A8);

            /* 16-bit components */
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16, eR16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16, eR16G16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16_B16, eR16G16B16);
            FORMAT_NORM_INT_SCALED_FLOAT_CASE(R16_G16_B16_A16, eR16G16B16A16);

            /* 32-bit components */
            FORMAT_INT_FLOAT_CASE(R32, eR32);
            FORMAT_INT_FLOAT_CASE(R32_G32, eR32G32);
            FORMAT_INT_FLOAT_CASE(R32_G32_B32, eR32G32B32);
            FORMAT_INT_FLOAT_CASE(R32_G32_B32_A32, eR32G32B32A32);

            /* 10-bit RGB, 2-bit A */
            FORMAT_NORM_INT_SCALED_CASE(A2B10G10R10, eA2B10G10R10, Pack32);

            /* 11-bit G and R, 10-bit B */
            FORMAT_CASE(B10G11R11, Float, Ufloat, eB10G11R11, Pack32);

            default:
                Logger::Warn("Unimplemented Maxwell3D Vertex Buffer Format: {} | {}", static_cast<u8>(componentBitWidths), static_cast<u8>(numericalType));
                return vk::Format::eR8G8B8A8Unorm;
        }

        #undef FORMAT_CASE
        #undef FORMAT_INT_CASE
        #undef FORMAT_INT_FLOAT_CASE
        #undef FORMAT_NORM_INT_SCALED_CASE
        #undef FORMAT_NORM_INT_SCALED_FLOAT_CASE
    }

    static Shader::AttributeType ConvertShaderGenericInputType(engine::VertexAttribute::NumericalType numericalType) {
        using MaxwellType = engine::VertexAttribute::NumericalType;
        switch (numericalType) {
            case MaxwellType::Snorm:
            case MaxwellType::Unorm:
            case MaxwellType::Uscaled:
            case MaxwellType::Sscaled:
            case MaxwellType::Float:
                return Shader::AttributeType::Float;
            case MaxwellType::Sint:
                return Shader::AttributeType::SignedInt;
            case MaxwellType::Uint:
                return Shader::AttributeType::UnsignedInt;
            default:
                Logger::Warn("Unimplemented attribute type: {}", static_cast<u8>(numericalType));
                return Shader::AttributeType::Disabled;
        }
    }
    
    /* Input Assembly State */
    void InputAssemblyState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, primitiveRestartEnable);
    }

    InputAssemblyState::InputAssemblyState(const EngineRegisters &engine) : engine{engine} {}

    void InputAssemblyState::Update(PackedPipelineState &packedState) {
        packedState.topology = currentEngineTopology;
        packedState.primitiveRestartEnabled = engine.primitiveRestartEnable & 1;
    }
    
    static std::pair<vk::PrimitiveTopology, Shader::InputTopology> ConvertPrimitiveTopology(engine::DrawTopology topology) {
        switch (topology) {
            case engine::DrawTopology::Points:
                return {vk::PrimitiveTopology::ePointList, Shader::InputTopology::Points};
            case engine::DrawTopology::Lines:
                return {vk::PrimitiveTopology::eLineList, Shader::InputTopology::Lines};
            case engine::DrawTopology::LineStrip:
                return {vk::PrimitiveTopology::eLineStrip, Shader::InputTopology::Lines};
            case engine::DrawTopology::Triangles:
                return {vk::PrimitiveTopology::eTriangleList, Shader::InputTopology::Triangles};
            case engine::DrawTopology::TriangleStrip:
                return {vk::PrimitiveTopology::eTriangleStrip, Shader::InputTopology::Triangles};
            case engine::DrawTopology::TriangleFan:
                return {vk::PrimitiveTopology::eTriangleFan, Shader::InputTopology::Triangles};
            case engine::DrawTopology::Quads:
                return {vk::PrimitiveTopology::eTriangleList, Shader::InputTopology::Triangles}; // Will use quad conversion
            case engine::DrawTopology::LineListAdjcy:
                return {vk::PrimitiveTopology::eLineListWithAdjacency, Shader::InputTopology::Lines};
            case engine::DrawTopology::LineStripAdjcy:
                return {vk::PrimitiveTopology::eLineStripWithAdjacency, Shader::InputTopology::Lines};
            case engine::DrawTopology::TriangleListAdjcy:
                return {vk::PrimitiveTopology::eTriangleListWithAdjacency, Shader::InputTopology::Triangles};
            case engine::DrawTopology::TriangleStripAdjcy:
                return {vk::PrimitiveTopology::eTriangleStripWithAdjacency, Shader::InputTopology::Triangles};
            case engine::DrawTopology::Patch:
                return {vk::PrimitiveTopology::ePatchList, Shader::InputTopology::Triangles};
            default:
                Logger::Warn("Unimplemented input assembly topology: {}", static_cast<u8>(topology));
                return {vk::PrimitiveTopology::eTriangleList, Shader::InputTopology::Triangles};
        }
    }

    void InputAssemblyState::SetPrimitiveTopology(engine::DrawTopology topology) {
        currentEngineTopology = topology;

        /*
            if (shaderTopology == ShaderCompiler::InputTopology::Points)
                UpdateRuntimeInformation(runtimeInfo.fixed_state_point_size, std::make_optional(pointSpriteSize), maxwell3d::PipelineStage::Vertex, maxwell3d::PipelineStage::Geometry);
            else if (runtimeInfo.input_topology == ShaderCompiler::InputTopology::Points)
                UpdateRuntimeInformation(runtimeInfo.fixed_state_point_size, std::optional<float>{}, maxwell3d::PipelineStage::Vertex, maxwell3d::PipelineStage::Geometry);

            UpdateRuntimeInformation(runtimeInfo.input_topology, shaderTopology, maxwell3d::PipelineStage::Geometry);
         */
    }

    engine::DrawTopology InputAssemblyState::GetPrimitiveTopology() const {
        return currentEngineTopology;
    }

    bool InputAssemblyState::NeedsQuadConversion() const {
        return currentEngineTopology == engine::DrawTopology::Quads;
    }


    /* Tessellation State */
    void TessellationState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, patchSize, tessellationParameters);
    }

    TessellationState::TessellationState(const EngineRegisters &engine) : engine{engine} {}

    void TessellationState::Update(PackedPipelineState &packedState) {
        packedState.patchSize = engine.patchSize;
        packedState.SetTessellationParameters(engine.tessellationParameters);
    }

    Shader::TessPrimitive ConvertShaderTessPrimitive(engine::TessellationParameters::DomainType domainType) {
        switch (domainType) {
            case engine::TessellationParameters::DomainType::Isoline:
                return Shader::TessPrimitive::Isolines;
            case engine::TessellationParameters::DomainType::Triangle:
                return Shader::TessPrimitive::Triangles;
            case engine::TessellationParameters::DomainType::Quad:
                return Shader::TessPrimitive::Quads;
        }
    }

    Shader::TessSpacing ConvertShaderTessSpacing(engine::TessellationParameters::Spacing spacing) {
        switch (spacing) {
            case engine::TessellationParameters::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case engine::TessellationParameters::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            case engine::TessellationParameters::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
        }
    }

 //   void TessellationState::SetParameters(engine::TessellationParameters params) {
        // UpdateRuntimeInformation(runtimeInfo.tess_primitive, ConvertShaderTessPrimitive(params.domainType), maxwell3d::PipelineStage::TessellationEvaluation);
        // UpdateRuntimeInformation(runtimeInfo.tess_spacing, ConvertShaderTessSpacing(params.spacing), maxwell3d::PipelineStage::TessellationEvaluation);
        // UpdateRuntimeInformation(runtimeInfo.tess_clockwise, params.outputPrimitive == engine::TessellationParameters::OutputPrimitives::TrianglesCW,
        //                          maxwell3d::PipelineStage::TessellationEvaluation);
 //   }

    /* Rasterizer State */
    void RasterizationState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, rasterEnable, frontPolygonMode, backPolygonMode, viewportClipControl, oglCullEnable, oglFrontFace, oglCullFace, windowOrigin, provokingVertex, polyOffset);
    }

    RasterizationState::RasterizationState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    bool ConvertDepthBiasEnable(engine::PolyOffset polyOffset, engine::PolygonMode polygonMode) {
        switch (polygonMode) {
            case engine::PolygonMode::Point:
                return polyOffset.pointEnable;
            case engine::PolygonMode::Line:
                return polyOffset.lineEnable;
            case engine::PolygonMode::Fill:
                return polyOffset.fillEnable;
            default:
                throw exception("Invalid polygon mode: 0x{:X}", static_cast<u32>(polygonMode));
        }
    }

    static vk::ProvokingVertexModeEXT ConvertProvokingVertex(engine::ProvokingVertex::Value provokingVertex) {
        switch (provokingVertex) {
            case engine::ProvokingVertex::Value::First:
                return vk::ProvokingVertexModeEXT::eFirstVertex;
            case engine::ProvokingVertex::Value::Last:
                return vk::ProvokingVertexModeEXT::eLastVertex;
        }
    }

    void RasterizationState::Flush(PackedPipelineState &packedState) {
        packedState.rasterizerDiscardEnable = !engine->rasterEnable;
        packedState.SetPolygonMode(engine->frontPolygonMode);
        if (engine->backPolygonMode != engine->frontPolygonMode)
            Logger::Warn("Non-matching polygon modes!");

        packedState.SetCullMode(engine->oglCullEnable, engine->oglCullFace);

        //                UpdateRuntimeInformation(runtimeInfo.y_negate, enabled, maxwell3d::PipelineStage::Vertex, maxwell3d::PipelineStage::Fragment);

        packedState.flipYEnable = engine->windowOrigin.flipY;

        bool origFrontFaceClockwise{engine->oglFrontFace == engine::FrontFace::CW};
        packedState.frontFaceClockwise = (packedState.flipYEnable != origFrontFaceClockwise);
        packedState.depthBiasEnable = ConvertDepthBiasEnable(engine->polyOffset, engine->frontPolygonMode);
        packedState.provokingVertex = engine->provokingVertex.value;
    }

    /* Depth Stencil State */
    void DepthStencilState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, depthTestEnable, depthWriteEnable, depthFunc, depthBoundsTestEnable, stencilTestEnable, twoSidedStencilTestEnable, stencilOps, stencilBack);
    }

    DepthStencilState::DepthStencilState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    static vk::StencilOpState ConvertStencilOpsState(engine::StencilOps ops) {
        return {
            .passOp = ConvertStencilOp(ops.zPass),
            .depthFailOp = ConvertStencilOp(ops.zFail),
            .failOp = ConvertStencilOp(ops.fail),
            .compareOp = ConvertCompareFunc(ops.func),
        };
    }

    void DepthStencilState::Flush(PackedPipelineState &packedState) {
        packedState.depthTestEnable = engine->depthTestEnable;
        packedState.depthWriteEnable = engine->depthWriteEnable;
        packedState.SetDepthFunc(engine->depthFunc);
        packedState.depthBoundsTestEnable = engine->depthBoundsTestEnable;
        packedState.stencilTestEnable = engine->stencilTestEnable;

        auto stencilBack{engine->twoSidedStencilTestEnable ? engine->stencilBack : engine->stencilOps};
        packedState.SetStencilOps(engine->stencilOps, engine->stencilOps);
    };

    /* Color Blend State */
    void ColorBlendState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, logicOp, singleCtWriteControl, ctWrites, blendStatePerTargetEnable, blendPerTargets, blend);
    }

    ColorBlendState::ColorBlendState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void ColorBlendState::Flush(PackedPipelineState &packedState) {
        packedState.logicOpEnable = engine->logicOp.enable;
        packedState.SetLogicOp(engine->logicOp.func);

        for (u32 i{}; i < engine::ColorTargetCount; i++) {
            auto ctWrite{engine->singleCtWriteControl ? engine->ctWrites[0] : engine->ctWrites[i]};
            bool enable{engine->blend.enable[i] != 0};

            if (engine->blendStatePerTargetEnable)
                packedState.SetAttachmentBlendState(i, enable, ctWrite, engine->blendPerTargets[i]);
            else
                packedState.SetAttachmentBlendState(i, enable, ctWrite, engine->blend);
        }
    }

    /* Global Shader Config State */
    void GlobalShaderConfigState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, postVtgShaderAttributeSkipMask, bindlessTexture);
    }

    GlobalShaderConfigState::GlobalShaderConfigState(const EngineRegisters &engine) : engine{engine} {}

    void GlobalShaderConfigState::Update(PackedPipelineState &packedState) {
        packedState.postVtgShaderAttributeSkipMask = engine.postVtgShaderAttributeSkipMask;
        packedState.bindlessTextureConstantBufferSlotSelect = engine.bindlessTexture.constantBufferSlotSelect;
    }

    /* Pipeline State */
    void PipelineState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        auto bindFunc{[&](auto &regs) { regs.DirtyBind(manager, handle); }};

        ranges::for_each(colorRenderTargetsRegisters, bindFunc);
        bindFunc(depthRenderTargetRegisters);
        bindFunc(vertexInputRegisters);
    }

    PipelineState::PipelineState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine)
        : engine{manager, dirtyHandle, engine},
          shaders{util::MergeInto<dirty::ManualDirtyState<IndividualShaderState>, engine::PipelineCount>(manager, engine.shadersRegisters, util::IncrementingT<u8>{})},
          colorRenderTargets{util::MergeInto<dirty::ManualDirtyState<ColorRenderTargetState>, engine::ColorTargetCount>(manager, engine.colorRenderTargetsRegisters, util::IncrementingT<size_t>{})},
          depthRenderTarget{manager, engine.depthRenderTargetRegisters},
          vertexInput{manager, engine.vertexInputRegisters},
          tessellation{engine.tessellationRegisters},
          rasterization{manager, engine.rasterizationRegisters},
          depthStencil{manager, engine.depthStencilRegisters},
          colorBlend{manager, engine.colorBlendRegisters},
          directState{engine.inputAssemblyRegisters},
          globalShaderConfig{engine.globalShaderConfigRegisters} {}

    void PipelineState::Flush(InterconnectContext &ctx, StateUpdateBuilder &builder) {
        boost::container::static_vector<TextureView *, engine::ColorTargetCount> colorAttachments;
        for (auto &colorRenderTarget : colorRenderTargets)
            if (auto view{colorRenderTarget.UpdateGet(ctx, packedState).view}; view)
                colorAttachments.push_back(view.get());

        TextureView *depthAttachment{depthRenderTarget.UpdateGet(ctx, packedState).view.get()};

        vertexInput.Update(packedState);
        directState.inputAssembly.Update(packedState);
        tessellation.Update(packedState);
        rasterization.Update(packedState);
        /* vk::PipelineMultisampleStateCreateInfo multisampleState{
            .rasterizationSamples = vk::SampleCountFlagBits::e1
        }; */
        depthStencil.Update(packedState);
        colorBlend.Update(packedState);
        globalShaderConfig.Update(packedState);

        constexpr std::array<vk::DynamicState, 9> dynamicStates{
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
            vk::DynamicState::eLineWidth,
            vk::DynamicState::eDepthBias,
            vk::DynamicState::eBlendConstants,
            vk::DynamicState::eDepthBounds,
            vk::DynamicState::eStencilCompareMask,
            vk::DynamicState::eStencilWriteMask,
            vk::DynamicState::eStencilReference
        };

        vk::PipelineDynamicStateCreateInfo dynamicState{
            .dynamicStateCount = static_cast<u32>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data()
        };
    }

    std::shared_ptr<TextureView> PipelineState::GetColorRenderTargetForClear(InterconnectContext &ctx, size_t index) {
        return colorRenderTargets[index].UpdateGet(ctx, packedState).view;
    }

    std::shared_ptr<TextureView> PipelineState::GetDepthRenderTargetForClear(InterconnectContext &ctx) {
        return depthRenderTarget.UpdateGet(ctx, packedState).view;
    }
}