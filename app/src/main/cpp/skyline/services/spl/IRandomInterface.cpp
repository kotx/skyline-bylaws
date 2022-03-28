// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IRandomInterface.h"

#include <common.h>

namespace skyline::service::spl {
    std::mt19937 rng;

    IRandomInterface::IRandomInterface(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IRandomInterface::GetRandomBytes(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto &outBuf{request.outputBuf.at(0)};

        std::uniform_int_distribution<u16> distribution(0, std::numeric_limits<u8>::max());
        std::vector<u8> data(outBuf.size());
        std::generate(data.begin(), data.end(), [&] { return static_cast<u8>(distribution(rng)); });

        outBuf.copy_from(data);

        return {};
    }
}
