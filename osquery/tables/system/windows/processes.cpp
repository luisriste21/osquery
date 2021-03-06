/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed in accordance with the terms specified in
 *  the LICENSE file found in the root directory of this source tree.
 */

#include <map>
#include <string>

#define _WIN32_DCOM

// clang-format off
#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
// clang-format on
#include <iomanip>
#include <psapi.h>
#include <stdlib.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <osquery/core.h>
#include <osquery/filesystem/filesystem.h>
#include <osquery/logger.h>
#include <osquery/tables.h>

#include <osquery/core/windows/wmi.h>
#include <osquery/filesystem/fileops.h>
#include <osquery/sql/dynamic_table_row.h>
#include <osquery/utils/conversions/join.h>
#include <osquery/utils/conversions/tryto.h>
#include <osquery/utils/conversions/windows/strings.h>
#include <osquery/utils/scope_guard.h>

namespace osquery {
int getUidFromSid(PSID sid);
int getGidFromSid(PSID sid);
namespace tables {

const std::map<unsigned long, std::string> kMemoryConstants = {
    {PAGE_EXECUTE, "PAGE_EXECUTE"},
    {PAGE_EXECUTE_READ, "PAGE_EXECUTE_READ"},
    {PAGE_EXECUTE_READWRITE, "PAGE_EXECUTE_READWRITE"},
    {PAGE_EXECUTE_WRITECOPY, "PAGE_EXECUTE_WRITECOPY"},
    {PAGE_NOACCESS, "PAGE_NOACCESS"},
    {PAGE_READONLY, "PAGE_READONLY"},
    {PAGE_READWRITE, "PAGE_READWRITE"},
    {PAGE_WRITECOPY, "PAGE_WRITECOPY"},
    {PAGE_GUARD, "PAGE_GUARD"},
    {PAGE_NOCACHE, "PAGE_NOCACHE"},
    {PAGE_WRITECOMBINE, "PAGE_WRITECOMBINE"},
};

const unsigned long kMaxPathSize = 0x1000;

/**
 * All of these structs are needed to interface with
 * the NtQueryInformationProcess function, which is leveraged
 * to get the CommandLine data for a process invocation.
 */
typedef NTSTATUS(NTAPI* NtQueryInformationProcessPtr)(
    IN HANDLE ProcessHandle,
    IN unsigned long ProcessInformationClass,
    OUT PVOID ProcessInformation,
    IN ULONG ProcessInformationLength,
    OUT PULONG ReturnLength OPTIONAL);

typedef ULONG(NTAPI* RtlNtStatusToDosErrorPtr)(NTSTATUS Status);

NtQueryInformationProcessPtr kNtQueryInformationProcess = nullptr;

RtlNtStatusToDosErrorPtr kRtlNtStatusToDosError = nullptr;

typedef struct {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
  BYTE Reserved1[16];
  PVOID Reserved2[10];
  UNICODE_STRING ImagePathName;
  UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

typedef struct _PEB {
  BYTE Reserved1[2];
  BYTE BeingDebugged;
  BYTE Reserved2[1];
  PVOID Reserved3[2];
  PPEB_LDR_DATA Ldr;
  PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
  PVOID Reserved4[3];
  PVOID AtlThunkSListPtr;
  PVOID Reserved5;
  ULONG Reserved6;
  PVOID Reserved7;
  ULONG Reserved8;
  ULONG AtlThunkSListPtr32;
  PVOID Reserved9[45];
  BYTE Reserved10[96];
  PPS_POST_PROCESS_INIT_ROUTINE PostProcessInitRoutine;
  BYTE Reserved11[128];
  PVOID Reserved12[1];
  ULONG SessionId;
} PEB, *PPEB;

typedef struct _PROCESS_BASIC_INFORMATION {
  PVOID Reserved1;
  PPEB PebBaseAddress;
  PVOID Reserved2[2];
  ULONG_PTR UniqueProcessId;
  PVOID Reserved3;
} PROCESS_BASIC_INFORMATION;

/// Given a pid, enumerates all loaded modules and memory pages for that process
Status genMemoryMap(unsigned long pid, QueryData& results) {
  auto proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (proc == nullptr) {
    Row r;
    r["pid"] = INTEGER(pid);
    r["start"] = INTEGER(-1);
    r["end"] = INTEGER(-1);
    r["permissions"] = "";
    r["offset"] = INTEGER(-1);
    r["device"] = INTEGER(-1);
    r["inode"] = INTEGER(-1);
    r["path"] = "";
    r["pseudo"] = INTEGER(-1);
    results.push_back(r);
    return Status::failure("Failed to open handle to process " +
                           std::to_string(pid));
  }
  auto modSnap =
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (modSnap == INVALID_HANDLE_VALUE) {
    CloseHandle(proc);
    return Status::failure("Failed to enumerate modules for " +
                           std::to_string(pid));
  }

  auto formatMemPerms = [](unsigned long perm) {
    std::vector<std::string> perms;
    for (const auto& kv : kMemoryConstants) {
      if (kv.first & perm) {
        perms.push_back(kv.second);
      }
    }
    return osquery::join(perms, " | ");
  };

  MODULEENTRY32 me;
  MEMORY_BASIC_INFORMATION mInfo;
  me.dwSize = sizeof(MODULEENTRY32);
  auto ret = Module32First(modSnap, &me);
  while (ret != FALSE) {
    for (auto p = me.modBaseAddr;
         VirtualQueryEx(proc, p, &mInfo, sizeof(mInfo)) == sizeof(mInfo) &&
         p < (me.modBaseAddr + me.modBaseSize);
         p += mInfo.RegionSize) {
      Row r;
      r["pid"] = INTEGER(pid);
      std::stringstream ssStart;
      ssStart << std::hex << mInfo.BaseAddress;
      r["start"] = "0x" + ssStart.str();
      std::stringstream ssEnd;
      ssEnd << std::hex << std::setfill('0') << std::setw(16)
            << reinterpret_cast<unsigned long long>(mInfo.BaseAddress) +
                   mInfo.RegionSize;
      r["end"] = "0x" + ssEnd.str();
      r["permissions"] = formatMemPerms(mInfo.Protect);
      r["offset"] =
          BIGINT(reinterpret_cast<unsigned long long>(mInfo.AllocationBase));
      r["device"] = "-1";
      r["inode"] = INTEGER(-1);
      r["path"] = me.szExePath;
      r["pseudo"] = INTEGER(-1);
      results.push_back(r);
    }
    ret = Module32Next(modSnap, &me);
  }
  CloseHandle(proc);
  CloseHandle(modSnap);
  return Status::success();
}

/// Helper function for enumerating all active processes on the system
Status getProcList(std::set<long>& pids) {
  auto procSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (procSnap == INVALID_HANDLE_VALUE) {
    return Status::failure("Failed to open process snapshot");
  }

  PROCESSENTRY32 procEntry;
  procEntry.dwSize = sizeof(PROCESSENTRY32);
  auto ret = Process32First(procSnap, &procEntry);

  if (ret == FALSE) {
    CloseHandle(procSnap);
    return Status::failure("Failed to open first process");
  }

  while (ret != FALSE) {
    pids.insert(procEntry.th32ProcessID);
    ret = Process32Next(procSnap, &procEntry);
  }

  CloseHandle(procSnap);
  return Status::success();
}

/// For legacy systems, we retrieve the commandline from the PEB
Status getProcessCommandLineLegacy(HANDLE proc,
                                   std::string& out,
                                   const unsigned long pid) {
  PROCESS_BASIC_INFORMATION pbi;
  unsigned long len{0};
  NTSTATUS status = NtQueryInformationProcess(
      proc, ProcessBasicInformation, &pbi, sizeof(pbi), &len);

  SetLastError(RtlNtStatusToDosError(status));
  if (NT_ERROR(status) || !pbi.PebBaseAddress) {
    return Status::failure("NtQueryInformationProcess failed for " +
                           std::to_string(pid) + " with " +
                           std::to_string(status));
  }

  size_t bytes_read = 0;
  PEB peb;
  if (!ReadProcessMemory(
          proc, pbi.PebBaseAddress, &peb, sizeof(peb), &bytes_read)) {
    return Status::failure("Reading PEB failed for " + std::to_string(pid) +
                           " with " + std::to_string(status));
  }

  RTL_USER_PROCESS_PARAMETERS upp;
  if (!ReadProcessMemory(
          proc, peb.ProcessParameters, &upp, sizeof(upp), &bytes_read)) {
    return Status::failure("Reading USER_PROCESS_PARAMETERS failed for " +
                           std::to_string(pid));
  }

  std::vector<wchar_t> command_line(kMaxPathSize, 0x0);
  SecureZeroMemory(command_line.data(), kMaxPathSize);
  if (!ReadProcessMemory(proc,
                         upp.CommandLine.Buffer,
                         command_line.data(),
                         upp.CommandLine.Length,
                         &bytes_read)) {
    return Status::failure("Failed to read command line for " +
                           std::to_string(pid));
  }
  out = wstringToString(command_line.data());
  return Status::success();
}

// On windows 8.1 and later, there's an enum for commandline
Status getProcessCommandLine(HANDLE& proc,
                             std::string& out,
                             const unsigned long pid) {
  if (kNtQueryInformationProcess == nullptr) {
    return Status::failure("Failed to resolve NtQueryInformationProcess with " +
                           std::to_string(GetLastError()));
  }

  unsigned long size_out = 0;
  PROCESS_BASIC_INFORMATION proc_info;
  SecureZeroMemory(&proc_info, sizeof(PROCESS_BASIC_INFORMATION));

  // See the Process Hacker implementation for more details on the hard coded 60
  // https://github.com/processhacker/processhacker/blob/master/phnt/include/ntpsapi.h#L160
  auto ret = kNtQueryInformationProcess(proc, 60, NULL, 0, &size_out);

  if (ret != STATUS_BUFFER_OVERFLOW && ret != STATUS_BUFFER_TOO_SMALL &&
      ret != STATUS_INFO_LENGTH_MISMATCH) {
    return Status::failure("NtQueryInformationProcess failed for " +
                           std::to_string(pid) + " with " +
                           std::to_string(ret));
  }

  std::vector<char> cmdline(size_out, 0x0);
  ret =
      kNtQueryInformationProcess(proc, 60, cmdline.data(), size_out, &size_out);

  if (!NT_SUCCESS(ret)) {
    return Status::failure("NtQueryInformationProcess failed for " +
                           std::to_string(pid) + " with " +
                           std::to_string(ret));
  }
  auto ustr = reinterpret_cast<PUNICODE_STRING>(cmdline.data());
  out = wstringToString(ustr->Buffer);
  return Status::success();
}

void getProcessPathInfo(HANDLE& proc,
                        const unsigned long pid,
                        DynamicTableRowHolder& r) {
  auto out = kMaxPathSize;
  std::vector<char> path(kMaxPathSize, 0x0);
  SecureZeroMemory(path.data(), kMaxPathSize);
  auto ret = QueryFullProcessImageName(proc, 0, path.data(), &out);
  if (ret != TRUE) {
    LOG(ERROR) << "Failed to lookup path information for process " << pid;
  } else {
    r["path"] = TEXT(path.data());
  }

  auto s = osquery::pathExists(path.data());
  r["on_disk"] = INTEGER(s.getMessage());

  path.clear();
  path.resize(kMaxPathSize, 0x0);
  if (pid == GetCurrentProcessId()) {
    ret = GetModuleFileName(nullptr, path.data(), kMaxPathSize);
  } else {
    ret = GetModuleFileNameEx(proc, nullptr, path.data(), kMaxPathSize);
  }

  if (ret == FALSE) {
    LOG(ERROR) << "Failed to get cwd for " << pid << " with " << GetLastError();
  } else {
    r["cwd"] = TEXT(path.data());
  }
  r["root"] = r["cwd"];
}

void genProcessUserTokenInfo(HANDLE& proc, DynamicTableRowHolder& r) {
  /// Get the process UID and GID from its SID
  HANDLE tok = nullptr;
  std::vector<char> tok_user(sizeof(TOKEN_USER), 0x0);

  auto ret = OpenProcessToken(proc, TOKEN_READ, &tok);
  long elevated = -1;
  if (ret != 0 && tok != nullptr) {
    unsigned long tokOwnerBuffLen;
    ret = GetTokenInformation(tok, TokenUser, nullptr, 0, &tokOwnerBuffLen);
    if (ret == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      tok_user.resize(tokOwnerBuffLen);
      ret = GetTokenInformation(
          tok, TokenUser, tok_user.data(), tokOwnerBuffLen, &tokOwnerBuffLen);
    }

    /// Check if the process is using an elevated token
    TOKEN_ELEVATION elevation;
    DWORD cb_size = sizeof(TOKEN_ELEVATION);
    if (GetTokenInformation(tok,
                            TokenElevation,
                            &elevation,
                            sizeof(TOKEN_ELEVATION),
                            &cb_size)) {
      elevated = elevation.TokenIsElevated;
    }
  }
  r["is_elevated_token"] = INTEGER(elevated);
  if (ret != 0 && !tok_user.empty()) {
    auto sid = PTOKEN_OWNER(tok_user.data())->Owner;

    r["uid"] = BIGINT(getUidFromSid(sid));
    r["gid"] = BIGINT(getGidFromSid(sid));
  }
  if (tok != nullptr) {
    CloseHandle(tok);
  }
}

void genProcessTimeInfo(HANDLE& proc, DynamicTableRowHolder& r) {
  FILETIME create_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;
  auto ret =
      GetProcessTimes(proc, &create_time, &exit_time, &kernel_time, &user_time);
  if (ret == FALSE) {
    LOG(ERROR) << "Failed to lookup time data for process "
               << GetProcessId(proc);
  } else {
    // Windows stores proc times in 100 nanosecond ticks
    ULARGE_INTEGER utime;
    utime.HighPart = user_time.dwHighDateTime;
    utime.LowPart = user_time.dwLowDateTime;
    auto user_time_total = utime.QuadPart;
    r["user_time"] = BIGINT(user_time_total / 10000);
    utime.HighPart = kernel_time.dwHighDateTime;
    utime.LowPart = kernel_time.dwLowDateTime;
    r["system_time"] = BIGINT(utime.QuadPart / 10000);
    r["percent_processor_time"] = BIGINT(user_time_total + utime.QuadPart);

    auto proc_create_time = osquery::filetimeToUnixtime(create_time);
    r["start_time"] = BIGINT(proc_create_time);

    FILETIME curr_ft_time;
    SYSTEMTIME curr_sys_time;
    GetSystemTime(&curr_sys_time);
    SystemTimeToFileTime(&curr_sys_time, &curr_ft_time);
    r["elapsed_time"] =
        BIGINT(osquery::filetimeToUnixtime(curr_ft_time) - proc_create_time);
  }
}

void genProcRssInfo(HANDLE& proc, DynamicTableRowHolder& r) {
  PROCESS_MEMORY_COUNTERS_EX mem_ctr;
  auto ret =
      GetProcessMemoryInfo(proc,
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem_ctr),
                           sizeof(PROCESS_MEMORY_COUNTERS_EX));

  if (ret != TRUE) {
    LOG(ERROR) << "Failed to lookup RSS stats for process "
               << GetProcessId(proc);
  }
  r["wired_size"] =
      ret == TRUE ? BIGINT(mem_ctr.QuotaNonPagedPoolUsage) : BIGINT(-1);
  r["resident_size"] =
      ret == TRUE ? BIGINT(mem_ctr.WorkingSetSize) : BIGINT(-1);
  r["total_size"] = ret == TRUE ? BIGINT(mem_ctr.PrivateUsage) : BIGINT(-1);
}

TableRows genProcesses(QueryContext& context) {
  TableRows results;

  auto proc_snap = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
  auto const proc_snap_manager =
      scope_guard::create([&proc_snap]() { CloseHandle(proc_snap); });
  if (proc_snap == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "Failed to create snapshot of processes with "
               << std::to_string(GetLastError());
    return {};
  }

  PROCESSENTRY32 proc;
  proc.dwSize = sizeof(PROCESSENTRY32);

  auto ret = Process32First(proc_snap, &proc);
  if (ret == FALSE) {
    LOG(ERROR) << "Failed to acquire first process information with "
               << std::to_string(GetLastError());
    return {};
  }

  while (ret != FALSE) {
    auto r = make_table_row();
    auto pid = proc.th32ProcessID;
    r["pid"] = BIGINT(pid);
    r["parent"] = BIGINT(proc.th32ParentProcessID);
    r["name"] = TEXT(proc.szExeFile);
    r["threads"] = INTEGER(proc.cntThreads);

    // Set default values for columns, in the event opening the process fails
    r["pgroup"] = BIGINT(-1);
    r["euid"] = BIGINT(-1);
    r["suid"] = BIGINT(-1);
    r["egid"] = BIGINT(-1);
    r["sgid"] = BIGINT(-1);
    r["uid"] = BIGINT(-1);
    r["gid"] = BIGINT(-1);

    r["user_time"] = BIGINT(-1);
    r["system_time"] = BIGINT(-1);
    r["start_time"] = BIGINT(-1);
    r["elapsed_time"] = BIGINT(-1);
    r["percent_processor_time"] = BIGINT(-1);

    r["nice"] = BIGINT(-1);
    r["wired_size"] = BIGINT(-1);
    r["resident_size"] = BIGINT(-1);
    r["total_size"] = BIGINT(-1);
    r["disk_bytes_read"] = BIGINT(-1);
    r["disk_bytes_written"] = BIGINT(-1);
    r["handle_count"] = BIGINT(-1);

    r["on_disk"] = BIGINT(-1);

    auto proc_handle =
        OpenProcess(PROCESS_ALL_ACCESS, FALSE, proc.th32ProcessID);
    auto const proc_handle_manager =
        scope_guard::create([&proc_handle]() { CloseHandle(proc_handle); });

    // If we fail to get all privs, open with less permissions
    if (proc_handle == NULL) {
      proc_handle = OpenProcess(
          PROCESS_QUERY_LIMITED_INFORMATION, FALSE, proc.th32ProcessID);
    }

    if (proc_handle == NULL) {
      VLOG(1) << "Failed to open handle to process " << proc.th32ProcessID
              << " with " << GetLastError();
      results.push_back(r);
      ret = Process32Next(proc_snap, &proc);
      continue;
    }

    auto nice = GetPriorityClass(proc_handle);
    r["nice"] = nice != FALSE ? INTEGER(nice) : "-1";

    if (context.isAnyColumnUsed({"cwd", "root", "path", "on_disk"})) {
      getProcessPathInfo(proc_handle, pid, r);
    }

    if (context.isColumnUsed("cmdline")) {
      if (kNtQueryInformationProcess == nullptr) {
        // GetModuleHandle doesn't increment a reference, no need to track
        // handle
        kNtQueryInformationProcess =
            reinterpret_cast<NtQueryInformationProcessPtr>(GetProcAddress(
                GetModuleHandle("ntdll.dll"), "NtQueryInformationProcess"));

        kRtlNtStatusToDosError =
            reinterpret_cast<RtlNtStatusToDosErrorPtr>(GetProcAddress(
                GetModuleHandle("ntdll.dll"), "RtlNtStatusToDosError"));
      }

      std::string cmd{""};
      auto s = getProcessCommandLine(
          proc_handle, cmd, static_cast<unsigned long>(proc.th32ProcessID));
      if (!s.ok()) {
        s = getProcessCommandLineLegacy(
            proc_handle, cmd, static_cast<unsigned long>(proc.th32ProcessID));
      }
      r["cmdline"] = cmd;
    }

    if (context.isAnyColumnUsed({"uid", "gid", "is_elevated_token"})) {
      genProcessUserTokenInfo(proc_handle, r);
    }

    if (context.isAnyColumnUsed({"user_time",
                                 "system_time",
                                 "start_time",
                                 "elapsed_time",
                                 "percent_processor_time"})) {
      genProcessTimeInfo(proc_handle, r);
    }

    if (context.isAnyColumnUsed(
            {"wired_size", "resident_size", "total_size"})) {
      genProcRssInfo(proc_handle, r);
    }

    if (context.isAnyColumnUsed({"disk_bytes_read", "disk_bytes_written"})) {
      IO_COUNTERS io_ctrs;
      ret = GetProcessIoCounters(proc_handle, &io_ctrs);
      r["disk_bytes_read"] =
          ret == TRUE ? BIGINT(io_ctrs.ReadTransferCount) : BIGINT(-1);
      r["disk_bytes_written"] =
          ret == TRUE ? BIGINT(io_ctrs.WriteTransferCount) : BIGINT(-1);
    }

    if (context.isColumnUsed("handle_count")) {
      unsigned long handle_count;
      ret = GetProcessHandleCount(proc_handle, &handle_count);
      r["handle_count"] = ret == TRUE ? INTEGER(handle_count) : "-1";
    }

    /*
     * Note: On windows the concept of the process state isn't as clear as on
     * posix. The state value from WMI isn't currently returning anything, and
     * the most common way to get the state is as follows.
     */
    if (context.isColumnUsed("state")) {
      unsigned long exit_code = 0;
      GetExitCodeProcess(proc_handle, &exit_code);
      r["state"] = exit_code == STILL_ACTIVE ? "STILL_ACTIVE" : "EXITED";
    }

    results.push_back(r);
    ret = Process32Next(proc_snap, &proc);
  }

  return results;
}

QueryData genProcessMemoryMap(QueryContext& context) {
  QueryData results;

  std::set<long> pidlist;
  if (context.constraints.count("pid") > 0 &&
      context.constraints.at("pid").exists(EQUALS)) {
    for (const auto& pid : context.constraints.at("pid").getAll<int>(EQUALS)) {
      if (pid > 0) {
        pidlist.insert(pid);
      }
    }
  }
  if (pidlist.empty()) {
    getProcList(pidlist);
  }

  for (const auto& pid : pidlist) {
    auto s = genMemoryMap(pid, results);
    if (!s.ok()) {
      VLOG(1) << s.getMessage();
    }
  }

  return results;
}

} // namespace tables
} // namespace osquery
