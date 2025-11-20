/**
 * Copyright Spack Project Developers. See COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 */
#include "ld.h"
#include <minwindef.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include "coff_parser.h"
#include "coff_reader_writer.h"
#include "linker_invocation.h"
#include "spack_env.h"
#include "toolchain.h"
#include "utils.h"

void LdInvocation::LoadToolchainDependentSpackVars(SpackEnvState& spackenv) {
    this->command = spackenv.SpackLD;
}

DWORD LdInvocation::InvokeToolchain() {
    // Run a pass of the linker

    // First parse the linker command line to
    // understand what we'll be doing
    LinkerInvocation link_run(LdInvocation::ComposeCommandLists(
        {this->command_args, this->include_args, this->lib_args,
         this->lib_dir_args, this->obj_args}));
    link_run.Parse();
    // Run resource compiler to create
    // Resource for id'ing binary when relocating its import library
    std::string const rc_file = createRC(link_run.get_out());
    // Add produced RC file to linker CLI to inject ID
    this->lib_args.push_back(rc_file);
    // Run base linker invocation to produce initial
    // dll and import library
    DWORD const ret_code = ToolChainInvocation::InvokeToolchain();
    if (ret_code != 0) {
        return ret_code;
    }

    // Next we want to construct the proper commmand line to
    // recreate the import library from the same set of obj files
    // and libs

    // We're creating a PE, we need to create an appropriate import lib
    std::string const imp_lib_name = link_run.get_implib_name();
    // If there is no implib, we don't need to bother
    // trying to rename
    if (!fileExists(imp_lib_name)) {
        // There are numerous contexts in which a PE file
        // may not export symbols, some are bugs in the
        // upstream project, most are valid, all are not
        // the concern of this wrapper
        return 0;
    }
    std::string pe_name;
    try {
        pe_name = link_run.get_mangled_out();
    } catch (const NameTooLongError& e) {
        std::cerr << "Unable to mangle PE " << link_run.get_out()
                  << " name is too long\n";
        return ExitConditions::NORMALIZE_NAME_FAILURE;
    }
    std::string const abs_out_imp_lib_name = imp_lib_name + ".pe-abs.lib";
    std::string def = "-def ";
    std::string piped_args = link_run.get_lib_link_args();
    // create command line to generate new import lib
    this->rpath_executor =
        ExecuteCommand("lib.exe", LdInvocation::ComposeCommandLists({
                                      {def, piped_args, "-name:" + pe_name,
                                       "-out:" + abs_out_imp_lib_name},
                                      {link_run.get_rsp_file()},
                                      this->obj_args,
                                      this->lib_args,
                                      this->lib_dir_args,
                                  }));
    this->rpath_executor.Execute();
    DWORD const err_code = this->rpath_executor.Join();
    if (err_code != 0) {
        return err_code;
    }
    CoffReaderWriter coff_reader(abs_out_imp_lib_name);
    CoffParser coff(&coff_reader);
    if (!coff.Parse()) {
        debug("Failed to parse COFF file: " + abs_out_imp_lib_name);
        return ExitConditions::COFF_PARSE_FAILURE;
    }
    if (!coff.NormalizeName(pe_name)) {
        debug("Failed to normalize name for COFF file: " +
              abs_out_imp_lib_name);
        return ExitConditions::NORMALIZE_NAME_FAILURE;
    }
    debug("Renaming library from " + abs_out_imp_lib_name + " to " +
          imp_lib_name);
    int const remove_exitcode = std::remove(imp_lib_name.c_str());
    if (remove_exitcode) {
        debug("Failed to remove original import library with exit code: " +
              remove_exitcode);
        return ExitConditions::LIB_REMOVE_FAILURE;
    }
    int const rename_exitcode =
        std::rename(abs_out_imp_lib_name.c_str(), imp_lib_name.c_str());
    if (rename_exitcode) {
        debug("Failed to rename temporary import library with exit code: " +
              rename_exitcode);
        return ExitConditions::FILE_RENAME_FAILURE;
    }
    return ret_code;
}

std::string LdInvocation::createRC(const std::string& pe_stage_name) {
    const std::string template_base =
        "STRINGTABLE\n"
        "BEGIN\n";
    const std::string string_table_id = "    59673 ";
    const std::string template_end = "END\n";
    const std::string pe_name = stripLastExt(basename(pe_stage_name));
    const std::string rc_file_name = pe_name + ".rc";
    std::string res_file_name = pe_name + ".res";

    ExecuteCommand rc_executor("rc",
                               {"/fo " + res_file_name + " " + rc_file_name});
    std::ofstream rc_out(rc_file_name);
    if (!rc_out) {
        std::cerr << "Error: could not open rc file for creation: "
                  << rc_file_name << "\n";
        return std::string();
    }
    rc_out << template_base << string_table_id << '"' << pe_name << '"' << "\n"
           << template_end;
    rc_out.close();
    rc_executor.Execute();
    DWORD const err_code = rc_executor.Join();
    if (err_code != 0) {
        return std::string();
    }
    return res_file_name;
}
