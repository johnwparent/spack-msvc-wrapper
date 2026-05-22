/**
 * Copyright Spack Project Developers. See COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 */
#include "execute.h"
#include <corecrt_wstring.h>
#include <errhandlingapi.h>
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <namedpipeapi.h>
#include <processenv.h>
#include <processthreadsapi.h>
#include <winbase.h>
#include <windows.h>  // NOLINT
#include <winnt.h>

#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include "utils.h"

enum : std::uint16_t { InvalidExitCode = 999 };

ExecuteCommand::ExecuteCommand(std::string command)
    : base_command(std::move(command)) {
    this->CreateChildPipes();
    this->SetupExecute();
}

ExecuteCommand::ExecuteCommand(std::string arg, const StrList& args)
    : base_command(std::move(arg)) {
    for (const auto& argp : args) {
        this->command_args.push_back(argp);
    }
    this->CreateChildPipes();
    this->SetupExecute();
}

ExecuteCommand& ExecuteCommand::operator=(
    ExecuteCommand&& execute_command) noexcept {
    this->ChildStdOut_Rd = std::move(execute_command.ChildStdOut_Rd);
    this->ChildStdOut_Wd = std::move(execute_command.ChildStdOut_Wd);
    this->ChildStdErr_Rd = std::move(execute_command.ChildStdErr_Rd);
    this->ChildStdErr_Wd = std::move(execute_command.ChildStdErr_Wd);
    this->procInfo = std::move(execute_command.procInfo);
    this->startInfo = std::move(execute_command.startInfo);
    this->saAttr = std::move(execute_command.saAttr);
    this->fileout = std::move(execute_command.fileout);
    this->write_to_file = std::move(execute_command.write_to_file);
    this->base_command = std::move(execute_command.base_command);
    this->command_args = std::move(execute_command.command_args);
    this->child_out_future = std::move(execute_command.child_out_future);
    this->child_err_future = std::move(execute_command.child_err_future);
    this->exit_code_future = std::move(execute_command.exit_code_future);
    this->saAttrErr = std::move(execute_command.saAttrErr);
    return *this;
}

ExecuteCommand::~ExecuteCommand() {
    // UniqueHandle members (pipe handles, fileout) clean up automatically.
    // procInfo handles are raw — ReportExitCode already closes hProcess,
    // so SafeHandleCleanup is a no-op for it after a successful Join.
    SafeHandleCleanup(this->procInfo.hProcess);
    SafeHandleCleanup(this->procInfo.hThread);
}

void ExecuteCommand::SetupExecute() {
    PROCESS_INFORMATION pi_proc_info;
    STARTUPINFOW si_start_info;
    ZeroMemory(&pi_proc_info, sizeof(PROCESS_INFORMATION));

    // Set up members of the STARTUPINFO structure.
    // This structure specifies the STDIN and STDOUT handles for redirection.
    ZeroMemory(&si_start_info, sizeof(STARTUPINFOW));
    si_start_info.cb = sizeof(STARTUPINFOW);
    // Do not attach the pipe handles here. We'll create inheritable
    // duplicates of the write ends immediately before CreateProcess to
    // minimize the inheritance window.
    si_start_info.hStdError = INVALID_HANDLE_VALUE;
    si_start_info.hStdOutput = INVALID_HANDLE_VALUE;
    si_start_info.dwFlags = 0;
    this->procInfo = pi_proc_info;
    this->startInfo = si_start_info;
}

/*
 * Create pipes and handles to communicate with
 * child process
 */
int ExecuteCommand::CreateChildPipes() {
    // Create stdout pipes
    SECURITY_ATTRIBUTES sa_attr;
    // Create non-inheritable pipes. We'll duplicate the write ends to
    // inheritable handles immediately before CreateProcess to reduce the
    // window where other processes could inherit them.
    sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr.bInheritHandle = FALSE;
    sa_attr.lpSecurityDescriptor = nullptr;
    this->saAttr = sa_attr;

    HANDLE rd = nullptr;
    HANDLE wd = nullptr;
    if (!CreatePipe(&rd, &wd, &sa_attr, 0))
        return 0;
    this->ChildStdOut_Rd.reset(rd);
    this->ChildStdOut_Wd.reset(wd);

    // create stderr pipes
    SECURITY_ATTRIBUTES sa_attr_err;
    sa_attr_err.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr_err.bInheritHandle = FALSE;
    sa_attr_err.lpSecurityDescriptor = nullptr;
    this->saAttrErr = sa_attr_err;

    HANDLE err_rd = nullptr;
    HANDLE err_wd = nullptr;
    if (!CreatePipe(&err_rd, &err_wd, &sa_attr_err, 0)) {
        this->ChildStdOut_Rd.reset();
        this->ChildStdOut_Wd.reset();
        return 0;
    }
    this->ChildStdErr_Rd.reset(err_rd);
    this->ChildStdErr_Wd.reset(err_wd);

    return 1;
}

/*
 * Kick off subprocess executing a given toolchain, returns a value indicating
 * whether the subprocess was created successfully
 */
bool ExecuteCommand::ExecuteToolChainChild() {
    LPVOID lp_msg_buf;
    std::wstring c_command_line;
    debug("Executing Command: " + this->ComposeCLI());
    try {
        c_command_line = ConvertASCIIToWide(this->ComposeCLI());
    } catch (const std::overflow_error& e) {
        std::cerr << e.what() << "\n";
        return false;
    }
    // Duplicate command line into writable buffer expected by CreateProcessW
    std::vector<wchar_t> cmdbuf(c_command_line.begin(), c_command_line.end());
    cmdbuf.push_back(L'\0');
    wchar_t* nc_command_line = cmdbuf.data();
    // Duplicate our non-inheritable write handles into inheritable handles
    // immediately before CreateProcess to minimize the inheritance window.
    HANDLE inheritableOut = INVALID_HANDLE_VALUE;
    HANDLE inheritableErr = INVALID_HANDLE_VALUE;
    HANDLE const self = GetCurrentProcess();
    if (this->ChildStdOut_Wd) {
        if (!DuplicateHandle(self, this->ChildStdOut_Wd.get(), self,
                             &inheritableOut, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            return false;
        }
        this->startInfo.hStdOutput = inheritableOut;
        this->startInfo.dwFlags |= STARTF_USESTDHANDLES;
    }
    if (this->ChildStdErr_Wd) {
        if (!DuplicateHandle(self, this->ChildStdErr_Wd.get(), self,
                             &inheritableErr, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            // Clean up any previously duplicated handle
            if (inheritableOut && inheritableOut != INVALID_HANDLE_VALUE)
                CloseHandle(inheritableOut);
            return false;
        }
        this->startInfo.hStdError = inheritableErr;
        this->startInfo.dwFlags |= STARTF_USESTDHANDLES;
    }

    if (!CreateProcessW(nullptr, nc_command_line, nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &this->startInfo, &this->procInfo)) {
        // Handle errors coming from creation of child proc
        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPTSTR>(&lp_msg_buf), 0, nullptr);

        try {
            std::cerr << "Failed to initiate child process from: "
                      << ConvertWideToASCII(nc_command_line) << " ";
        } catch (const std::overflow_error& e) {
            std::cerr << "While handling the exception below another exception "
                         "occured\n";
            std::cerr << e.what() << "\n";
            std::cerr << "Previous exception was:\n";
        }

        std::cerr << "With error: ";
        if (lp_msg_buf) {
            std::cerr << static_cast<char*>(lp_msg_buf) << "\n";
            LocalFree(lp_msg_buf);
        } else {
            std::cerr << reportLastError() << "\n";
        }
        return false;
    }
    // We've suceeded in kicking off the toolchain run
    // Parent no longer needs the inheritable duplicates; close them in the
    // parent. The child has inherited its own copies.
    if (this->startInfo.hStdOutput && this->startInfo.hStdOutput != INVALID_HANDLE_VALUE) {
        CloseHandle(this->startInfo.hStdOutput);
        this->startInfo.hStdOutput = INVALID_HANDLE_VALUE;
    }
    if (this->startInfo.hStdError && this->startInfo.hStdError != INVALID_HANDLE_VALUE) {
        CloseHandle(this->startInfo.hStdError);
        this->startInfo.hStdError = INVALID_HANDLE_VALUE;
    }
    // Close write ends in parent — child has inherited its own copies via the
    // duplicated inheritable handles above.
    this->ChildStdOut_Wd.reset();
    this->ChildStdErr_Wd.reset();
    return true;
}

/*
 * Reads for the member variable holding a pipe to the wrapped processes'
 * STD_HANDLE (stdour or stderr) and writes either to this processes'
 * STD_HANDLE or a file, depending on how the process wrapper is configured
 */
int ExecuteCommand::PipeChildToStdStream(DWORD STD_HANDLE,
                                         HANDLE reader_handle) {
    DWORD dw_read;
    DWORD dw_written;
    std::vector<char> ch_buf(BUFSIZE);
    BOOL b_success = TRUE;
    HANDLE h_parent_out;
    if (this->write_to_file && this->fileout) {
        h_parent_out = this->fileout.get();
    } else {
        h_parent_out = GetStdHandle(STD_HANDLE);
    }

    for (;;) {
        b_success = ReadFile(reader_handle, ch_buf.data(), BUFSIZE, &dw_read, nullptr);
        if (!b_success || (dw_read == 0 && this->terminated))
            break;
        if (dw_read != 0) {
            b_success =
                WriteFile(h_parent_out, ch_buf.data(), dw_read, &dw_written, nullptr);
            if (dw_written < dw_read && b_success) {
                // incomplete write but not a failure
                // since bSuccess is true
                // So lets write until bSuccess is false or
                // until all bytes are written
                DWORD current_pos = dw_written;
                DWORD remaining = dw_read - dw_written;
                while (remaining > 0) {
                    DWORD to_write = remaining;
                    b_success = WriteFile(h_parent_out, ch_buf.data() + current_pos,
                                          to_write, &dw_written, nullptr);
                    if (!b_success)
                        break;
                    current_pos += dw_written;
                    remaining -= dw_written;
                }
            }
            if (!b_success) {
                break;
            }
        }
        if (!b_success)
            break;
    }
    // Close the reader handle now that we're done reading from the pipe to
    // release underlying kernel resources. This prevents leaking handles
    // across many invocations which can cause pipes to never reach EOF and
    // eventually exhaust kernel resources.
    // Ownership was transferred here via .release() in Execute(), so we close
    // the raw handle directly.
    if (reader_handle && reader_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(reader_handle);
    }
    return static_cast<int>(static_cast<int>(b_success) == 0);
}

/**
 * Reports the exit code of a given process, used as a callback to report
 * on the status of wrapped process which is performed asynchronously
 */
DWORD ExecuteCommand::ReportExitCode() {
    WaitForSingleObject(this->procInfo.hProcess, INFINITE);
    DWORD exit_code = InvalidExitCode;
    GetExitCodeProcess(this->procInfo.hProcess, &exit_code);
    this->terminated = true;
    // Close the process handle and mark it invalid so the destructor doesn't
    // attempt to close it again.
    SafeHandleCleanup(this->procInfo.hProcess);
    return exit_code;
}

std::string ExecuteCommand::ComposeCLI() {
    std::string cli;
    cli += this->base_command + " ";
    for (const auto& arg : this->command_args) {
        cli += arg + " ";
    }
    return cli;
}

/*
 * Execute the wrapped command in subprocess, creating the process, and
 * allowing it, and the piping of its stdout to this process to run asychronously
 * and storing the futures of each async operation to be waited on at a later point
 *
 * If this instance has been configured to write to a file instead of stdout, opens that file
 * and prepares it for writing
 *
 * Returns a value indicating whether or not the subprocess has been created sucessfully
*/
bool ExecuteCommand::Execute(const std::string& filename) {
    if (!filename.empty()) {
        this->write_to_file = true;
        try {
            HANDLE const h_fileout = CreateFileW(
                ConvertASCIIToWide(filename).c_str(), FILE_APPEND_DATA,
                FILE_SHARE_WRITE | FILE_SHARE_READ, &this->saAttr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h_fileout != INVALID_HANDLE_VALUE) {
                this->fileout.reset(h_fileout);
                // Ensure file handle is not inheritable by child processes
                SetHandleInformation(this->fileout.get(), HANDLE_FLAG_INHERIT, 0);
            }
        } catch (const std::overflow_error& e) {
            std::cerr << e.what() << "\n";
            return false;
        }
    }
    // Start reader threads before spawning the child so they are ready to
    // drain the pipe as soon as the child writes. This reduces a small race
    // window where a very fast child can fill the pipe buffer before the
    // parent starts reading.
    // Transfer ownership of reader handles to the async threads via .release().
    this->child_out_future = std::async(
        std::launch::async, &ExecuteCommand::PipeChildToStdStream, this,
        STD_OUTPUT_HANDLE, this->ChildStdOut_Rd.release());
    this->child_err_future = std::async(
        std::launch::async, &ExecuteCommand::PipeChildToStdStream, this,
        STD_ERROR_HANDLE, this->ChildStdErr_Rd.release());

    bool const ret_code = this->ExecuteToolChainChild();
    if (ret_code) {
        // Start exit code watcher only after the process is created
        this->exit_code_future = std::async(
            std::launch::async, &ExecuteCommand::ReportExitCode, this);
    } else {
        // If we failed to spawn, ensure reader futures are cancelled/observed
        // by setting terminated so readers can exit promptly.
        this->terminated = true;
    }
    return ret_code;
}

/*
 * Blocks until the command initiated by execute terminates
 * and reports exit code of the process
 */
DWORD ExecuteCommand::Join() {
    // Join primary thread first
    // This process sets the termianted flag
    // without which the reader threads will not
    // terminate, so the primary thread must be
    // joined first so we have a guaruntee that the
    // reader processes can exit
    const DWORD command_error = this->exit_code_future.get();
    if (!this->child_out_future.get())
        return InvalidExitCode;
    if (!this->child_err_future.get())
        return InvalidExitCode;
    return command_error;
}
