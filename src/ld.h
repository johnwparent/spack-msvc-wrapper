/**
 * Copyright Spack Project Developers. See COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 */
#pragma once

#include "linker_invocation.h"

#include "toolchain.h"


class RCFileManager {
public:
    explicit RCFileManager(std::string file);
    ~RCFileManager();
    const std::string& getRC();
private:
    std::string rc_file_;
};


/**
 * @brief ClInvocation exposes an interface driving invocations of
 * link.exe and defines the parameters of the call to said executable
 */
class LdInvocation : public ToolChainInvocation {
   public:
    using ToolChainInvocation::ToolChainInvocation;
    virtual DWORD InvokeToolchain();

   protected:
    void LoadToolchainDependentSpackVars(SpackEnvState& spackenv);
    std::string lang = "link";
    ExecuteCommand rpath_executor;
    static std::unique_ptr<RCFileManager> createRC(LinkerInvocation& link_run);
};
