#include "startup_manager.h"

#include <mwfl/deployment.h>
#include <mwfl/security.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <windows.h>
#include <knownfolders.h>
#include <memory>
#include <shlobj.h>
#include <taskschd.h>
#include <unordered_map>
#include <wrl/client.h>

namespace startup_manager {
namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kDisabledKey[] = L"Software\\MWFL\\StartupManager\\DisabledRun";

struct RegKeyCloser { void operator()(HKEY value) const noexcept { if (value) ::RegCloseKey(value); } };
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;
using Microsoft::WRL::ComPtr;

struct BstrCloser { void operator()(wchar_t* value) const noexcept { ::SysFreeString(value); } };
using UniqueBstr = std::unique_ptr<wchar_t, BstrCloser>;

std::wstring TakeBstr(BSTR raw) {
  UniqueBstr value(raw);
  return raw ? std::wstring(raw, ::SysStringLen(raw)) : std::wstring{};
}

std::wstring ErrorText(DWORD error) {
  wchar_t* raw = nullptr;
  ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
  std::wstring text = raw ? raw : L"Windows error";
  if (raw) ::LocalFree(raw);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
  return text;
}

OperationResult Failure(std::wstring action, DWORD error) {
  return {false, std::move(action) + L": " + ErrorText(error), error};
}

std::wstring ErrorCodeText(const std::error_code& error) {
  const auto message = error.message();
  return std::to_wstring(error.value()) + L" (" +
         std::wstring(message.begin(), message.end()) + L")";
}

std::wstring RegistryId(StartupScope scope, REGSAM view, std::wstring_view name,
                        StartupState state) {
  return (scope == StartupScope::current_user ? L"hkcu:" : L"hklm:") +
         std::to_wstring(view == KEY_WOW64_32KEY ? 32 : 64) + L":" +
         (state == StartupState::disabled ? L"disabled:" : L"run:") + std::wstring(name);
}

bool ParseRegistryId(const StartupEntry& entry, HKEY& root, REGSAM& view,
                     std::wstring& name) {
  root = entry.scope == StartupScope::current_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
  view = entry.id.find(L":32:") != std::wstring::npos ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
  const auto marker = entry.state == StartupState::disabled ? L":disabled:" : L":run:";
  const auto pos = entry.id.find(marker);
  if (pos == std::wstring::npos) return false;
  name = entry.id.substr(pos + std::char_traits<wchar_t>::length(marker));
  return !name.empty();
}

void DiscoverRegistryView(DiscoveryResult& result, HKEY root, StartupScope scope,
                          REGSAM view, const wchar_t* key_path, StartupState state) {
  HKEY raw = nullptr;
  const auto opened = ::RegOpenKeyExW(root, key_path, 0, KEY_READ | view, &raw);
  if (opened == ERROR_FILE_NOT_FOUND) return;
  if (opened != ERROR_SUCCESS) {
    result.diagnostics.push_back(std::wstring(key_path) + L": " + ErrorText(opened));
    return;
  }
  UniqueRegKey key(raw);
  DWORD index = 0;
  for (;;) {
    std::array<wchar_t, 16384> name{};
    std::array<BYTE, 65536> data{};
    DWORD name_size = static_cast<DWORD>(name.size());
    DWORD data_size = static_cast<DWORD>(data.size());
    DWORD type = 0;
    const auto status = ::RegEnumValueW(key.get(), index++, name.data(), &name_size, nullptr,
                                        &type, data.data(), &data_size);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) {
      result.diagnostics.push_back(L"Registry enumeration: " + ErrorText(status));
      break;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
    std::wstring command(reinterpret_cast<wchar_t*>(data.data()), data_size / sizeof(wchar_t));
    while (!command.empty() && command.back() == L'\0') command.pop_back();
    if (type == REG_EXPAND_SZ) {
      const DWORD needed = ::ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
      if (needed) {
        std::wstring expanded(needed, L'\0');
        ::ExpandEnvironmentStringsW(command.c_str(), expanded.data(), needed);
        while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
        command = std::move(expanded);
      }
    }
    const auto executable = ExtractExecutable(command);
    const bool exists = executable.empty() || std::filesystem::exists(executable);
    result.entries.push_back({RegistryId(scope, view, {name.data(), name_size}, state),
                              {name.data(), name_size}, command,
                              (scope == StartupScope::current_user ? L"HKCU\\" : L"HKLM\\") +
                                  std::wstring(key_path) +
                                  (view == KEY_WOW64_32KEY ? L" (32-bit)" : L" (64-bit)"),
                              StartupSource::registry_run, scope, state,
                              scope == StartupScope::current_user, exists});
  }
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
  std::filesystem::path value(raw);
  ::CoTaskMemFree(raw);
  return value;
}

void DiscoverFolder(DiscoveryResult& result, const std::filesystem::path& folder,
                    StartupScope scope, StartupState state) {
  std::error_code error;
  if (folder.empty() || !std::filesystem::exists(folder, error)) return;
  for (const auto& item : std::filesystem::directory_iterator(folder, error)) {
    if (error) break;
    if (!item.is_regular_file(error)) continue;
    if (_wcsicmp(item.path().filename().c_str(), L"desktop.ini") == 0) continue;
    result.entries.push_back({L"folder:" + std::to_wstring(static_cast<int>(scope)) + L":" +
                                  std::to_wstring(static_cast<int>(state)) + L":" +
                                  item.path().wstring(),
                              item.path().stem().wstring(), item.path().wstring(),
                              item.path().wstring(), StartupSource::startup_folder, scope, state,
                              scope == StartupScope::current_user, true});
  }
  if (error) result.diagnostics.push_back(folder.wstring() + L": " + ErrorCodeText(error));
}

std::filesystem::path DisabledFolder(StartupScope scope) {
  auto base = KnownFolder(scope == StartupScope::current_user ? FOLDERID_LocalAppData
                                                               : FOLDERID_ProgramData);
  return base / L"MWFL" / L"StartupManager" / L"DisabledStartup";
}

OperationResult MoveRegistry(const StartupEntry& entry, bool enabling) {
  HKEY root = nullptr;
  REGSAM view = 0;
  std::wstring name;
  if (!ParseRegistryId(entry, root, view, name)) return {false, L"Invalid registry identity", 0};
  const wchar_t* source_path = enabling ? kDisabledKey : kRunKey;
  const wchar_t* destination_path = enabling ? kRunKey : kDisabledKey;
  HKEY source_raw = nullptr;
  auto status = ::RegOpenKeyExW(root, source_path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | view,
                                &source_raw);
  if (status != ERROR_SUCCESS) return Failure(L"Open source key", status);
  UniqueRegKey source(source_raw);
  DWORD type = 0, size = 0;
  status = ::RegQueryValueExW(source.get(), name.c_str(), nullptr, &type, nullptr, &size);
  if (status != ERROR_SUCCESS) return Failure(L"Read startup value", status);
  std::vector<BYTE> data(size);
  status = ::RegQueryValueExW(source.get(), name.c_str(), nullptr, &type, data.data(), &size);
  if (status != ERROR_SUCCESS) return Failure(L"Read startup value", status);
  HKEY destination_raw = nullptr;
  status = ::RegCreateKeyExW(root, destination_path, 0, nullptr, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr,
                             &destination_raw, nullptr);
  if (status != ERROR_SUCCESS) return Failure(L"Open recovery key", status);
  UniqueRegKey destination(destination_raw);
  DWORD existing = 0;
  if (::RegQueryValueExW(destination.get(), name.c_str(), nullptr, nullptr, nullptr, &existing) ==
      ERROR_SUCCESS)
    return {false, L"Destination already contains an entry with this name", ERROR_ALREADY_EXISTS};
  status = ::RegSetValueExW(destination.get(), name.c_str(), 0, type, data.data(), size);
  if (status != ERROR_SUCCESS) return Failure(L"Write destination value", status);
  status = ::RegDeleteValueW(source.get(), name.c_str());
  if (status != ERROR_SUCCESS) {
    ::RegDeleteValueW(destination.get(), name.c_str());
    return Failure(L"Remove source value", status);
  }
  DWORD verified_size = 0;
  if (::RegQueryValueExW(destination.get(), name.c_str(), nullptr, nullptr, nullptr,
                         &verified_size) != ERROR_SUCCESS ||
      ::RegQueryValueExW(source.get(), name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
          ERROR_SUCCESS)
    return {false, L"Windows did not confirm the requested startup state", ERROR_WRITE_FAULT};
  return {true, enabling ? L"Startup entry enabled" : L"Startup entry disabled", 0};
}

OperationResult MoveFolder(const StartupEntry& entry, bool enabling) {
  const std::filesystem::path source(entry.location);
  auto disabled = DisabledFolder(entry.scope);
  std::error_code error;
  std::filesystem::create_directories(disabled, error);
  if (error) return {false, L"Create recovery folder: " + ErrorCodeText(error),
                     static_cast<std::uint32_t>(error.value())};
  const auto destination = enabling
                               ? KnownFolder(entry.scope == StartupScope::current_user
                                                 ? FOLDERID_Startup : FOLDERID_CommonStartup) /
                                     source.filename()
                               : disabled / source.filename();
  if (std::filesystem::exists(destination, error))
    return {false, L"Destination already exists: " + destination.wstring(), ERROR_ALREADY_EXISTS};
  std::filesystem::rename(source, destination, error);
  if (!error && (!std::filesystem::exists(destination, error) ||
                 std::filesystem::exists(source, error)))
    return {false, L"Windows did not confirm the moved startup entry", ERROR_WRITE_FAULT};
  return error ? OperationResult{false, L"Move startup entry: " + ErrorCodeText(error),
                                 static_cast<std::uint32_t>(error.value())}
               : OperationResult{true, enabling ? L"Startup entry enabled"
                                                  : L"Startup entry disabled", 0};
}

ComPtr<ITaskService> ConnectTaskService() {
  ComPtr<ITaskService> service;
  if (FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&service)))) return {};
  VARIANT empty{};
  ::VariantInit(&empty);
  if (FAILED(service->Connect(empty, empty, empty, empty))) return {};
  return service;
}

void DiscoverTaskFolder(DiscoveryResult& result, ITaskFolder* folder, bool elevated) {
  ComPtr<IRegisteredTaskCollection> tasks;
  if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
    LONG count = 0;
    tasks->get_Count(&count);
    for (LONG index = 1; index <= count; ++index) {
      VARIANT item{};
      item.vt = VT_I4;
      item.lVal = index;
      ComPtr<IRegisteredTask> task;
      if (FAILED(tasks->get_Item(item, &task)) || !task) continue;
      ComPtr<ITaskDefinition> definition;
      ComPtr<ITriggerCollection> triggers;
      if (FAILED(task->get_Definition(&definition)) || !definition ||
          FAILED(definition->get_Triggers(&triggers)) || !triggers) continue;
      LONG trigger_count = 0;
      triggers->get_Count(&trigger_count);
      bool starts_at_logon = false;
      for (LONG trigger_index = 1; trigger_index <= trigger_count; ++trigger_index) {
        ComPtr<ITrigger> trigger;
        if (SUCCEEDED(triggers->get_Item(trigger_index, &trigger)) && trigger) {
          TASK_TRIGGER_TYPE2 type{};
          if (SUCCEEDED(trigger->get_Type(&type)) && type == TASK_TRIGGER_LOGON) {
            starts_at_logon = true;
            break;
          }
        }
      }
      if (!starts_at_logon) continue;
      BSTR raw_name = nullptr, raw_path = nullptr;
      VARIANT_BOOL enabled = VARIANT_FALSE;
      task->get_Name(&raw_name);
      task->get_Path(&raw_path);
      task->get_Enabled(&enabled);
      const auto name = TakeBstr(raw_name);
      const auto path = TakeBstr(raw_path);
      std::wstring command;
      ComPtr<IActionCollection> actions;
      if (SUCCEEDED(definition->get_Actions(&actions)) && actions) {
        LONG action_count = 0;
        actions->get_Count(&action_count);
        if (action_count > 0) {
          ComPtr<IAction> action;
          if (SUCCEEDED(actions->get_Item(1, &action)) && action) {
            ComPtr<IExecAction> execute;
            if (SUCCEEDED(action.As(&execute)) && execute) {
              BSTR raw_executable = nullptr, raw_arguments = nullptr;
              execute->get_Path(&raw_executable);
              execute->get_Arguments(&raw_arguments);
              command = TakeBstr(raw_executable);
              const auto arguments = TakeBstr(raw_arguments);
              if (!arguments.empty()) command += L" " + arguments;
            }
          }
        }
      }
      const auto executable = ExtractExecutable(command);
      result.entries.push_back({L"task:" + path, name, command, path,
                                StartupSource::scheduled_task, StartupScope::all_users,
                                enabled == VARIANT_TRUE ? StartupState::enabled
                                                        : StartupState::disabled,
                                elevated,
                                executable.empty() || std::filesystem::exists(executable)});
    }
  }
  ComPtr<ITaskFolderCollection> folders;
  if (FAILED(folder->GetFolders(0, &folders)) || !folders) return;
  LONG count = 0;
  folders->get_Count(&count);
  for (LONG index = 1; index <= count; ++index) {
    VARIANT item{};
    item.vt = VT_I4;
    item.lVal = index;
    ComPtr<ITaskFolder> child;
    if (SUCCEEDED(folders->get_Item(item, &child)) && child)
      DiscoverTaskFolder(result, child.Get(), elevated);
  }
}

ComPtr<IRegisteredTask> OpenTask(std::wstring_view path,
                                 ComPtr<ITaskFolder>* owner_folder = nullptr,
                                 std::wstring* task_name = nullptr) {
  const auto slash = path.find_last_of(L'\\');
  const std::wstring folder_path = slash == 0 ? L"\\" : std::wstring(path.substr(0, slash));
  const std::wstring name = slash == std::wstring_view::npos
                                ? std::wstring(path)
                                : std::wstring(path.substr(slash + 1));
  const auto service = ConnectTaskService();
  if (!service) return {};
  ComPtr<ITaskFolder> folder;
  const UniqueBstr folder_bstr(::SysAllocString(folder_path.c_str()));
  if (FAILED(service->GetFolder(folder_bstr.get(), &folder)) || !folder) return {};
  ComPtr<IRegisteredTask> task;
  const UniqueBstr name_bstr(::SysAllocString(name.c_str()));
  if (FAILED(folder->GetTask(name_bstr.get(), &task))) return {};
  if (owner_folder) *owner_folder = folder;
  if (task_name) *task_name = name;
  return task;
}

OperationResult SetTaskEnabled(const StartupEntry& entry, bool enabled) {
  const auto task = OpenTask(entry.location);
  if (!task) return {false, L"Scheduled task could not be opened", ERROR_FILE_NOT_FOUND};
  const auto status = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
  if (FAILED(status)) return Failure(L"Update scheduled task", HRESULT_CODE(status));
  VARIANT_BOOL verified = VARIANT_FALSE;
  if (FAILED(task->get_Enabled(&verified)) || (verified == VARIANT_TRUE) != enabled)
    return {false, L"Windows did not confirm the requested scheduled-task state",
            ERROR_WRITE_FAULT};
  return {true, enabled ? L"Scheduled task enabled" : L"Scheduled task disabled", 0};
}
}  // namespace

bool IsProcessElevated() {
  const auto identity = mwfl::QueryCurrentProcessIdentity();
  return identity && identity.Value().elevated;
}

std::wstring SourceName(StartupSource source) {
  switch (source) {
    case StartupSource::registry_run: return L"Registry Run";
    case StartupSource::startup_folder: return L"Startup folder";
    case StartupSource::scheduled_task: return L"Scheduled task";
  }
  return L"Unknown";
}
std::wstring ScopeName(StartupScope scope) {
  return scope == StartupScope::current_user ? L"Current user" : L"All users";
}
std::wstring StateName(StartupState state) {
  return state == StartupState::enabled ? L"Enabled" : L"Disabled";
}

std::wstring ExtractExecutable(std::wstring_view command) {
  while (!command.empty() && std::iswspace(command.front())) command.remove_prefix(1);
  if (command.empty()) return {};
  if (command.front() == L'\"') {
    command.remove_prefix(1);
    const auto end = command.find(L'\"');
    return std::wstring(command.substr(0, end));
  }
  const auto end = command.find_first_of(L" \t");
  return std::wstring(command.substr(0, end));
}

DiscoveryResult Discover() {
  DiscoveryResult result;
  const bool elevated = IsProcessElevated();
  const auto initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  DiscoverRegistryView(result, HKEY_CURRENT_USER, StartupScope::current_user,
                       KEY_WOW64_64KEY, kRunKey, StartupState::enabled);
  DiscoverRegistryView(result, HKEY_CURRENT_USER, StartupScope::current_user,
                       KEY_WOW64_64KEY, kDisabledKey, StartupState::disabled);
  for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
    DiscoverRegistryView(result, HKEY_LOCAL_MACHINE, StartupScope::all_users, view, kRunKey,
                         StartupState::enabled);
    DiscoverRegistryView(result, HKEY_LOCAL_MACHINE, StartupScope::all_users, view,
                         kDisabledKey, StartupState::disabled);
  }
  DiscoverFolder(result, KnownFolder(FOLDERID_Startup), StartupScope::current_user,
                 StartupState::enabled);
  DiscoverFolder(result, KnownFolder(FOLDERID_CommonStartup), StartupScope::all_users,
                 StartupState::enabled);
  DiscoverFolder(result, DisabledFolder(StartupScope::current_user),
                 StartupScope::current_user, StartupState::disabled);
  DiscoverFolder(result, DisabledFolder(StartupScope::all_users), StartupScope::all_users,
                 StartupState::disabled);
  if (const auto service = ConnectTaskService(); service) {
    ComPtr<ITaskFolder> root;
    const UniqueBstr root_path(::SysAllocString(L"\\"));
    if (SUCCEEDED(service->GetFolder(root_path.get(), &root)) && root)
      DiscoverTaskFolder(result, root.Get(), elevated);
  } else {
    result.diagnostics.push_back(L"Task Scheduler could not be read.");
  }
  std::ranges::sort(result.entries, [](const auto& a, const auto& b) {
    if (a.state != b.state) return a.state < b.state;
    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  if (elevated)
    for (auto& entry : result.entries) entry.writable = true;
  struct TargetMetadata {
    std::wstring version;
    std::wstring signature_status;
  };
  std::unordered_map<std::wstring, TargetMetadata> metadata_cache;
  for (auto& entry : result.entries) {
    const std::filesystem::path executable = ExtractExecutable(entry.command);
    std::error_code file_error;
    if (executable.empty() || !std::filesystem::is_regular_file(executable, file_error)) continue;
    auto cache_key = executable.lexically_normal().wstring();
    std::ranges::transform(cache_key, cache_key.begin(), ::towlower);
    if (const auto cached = metadata_cache.find(cache_key); cached != metadata_cache.end()) {
      entry.target_version = cached->second.version;
      entry.signature_status = cached->second.signature_status;
      continue;
    }
    if (const auto version = mwfl::QueryFileVersion(executable); version) {
      entry.target_version = std::format(L"{}.{}.{}.{}", version.Value().major,
                                         version.Value().minor, version.Value().build,
                                         version.Value().revision);
    }
    if (const auto signature = mwfl::VerifyAuthenticode(
            executable, mwfl::RevocationPolicy::Offline); signature) {
      switch (signature.Value().status) {
        case mwfl::SignatureStatus::Valid: entry.signature_status = L"Valid"; break;
        case mwfl::SignatureStatus::Unsigned: entry.signature_status = L"Unsigned"; break;
        case mwfl::SignatureStatus::Untrusted: entry.signature_status = L"Untrusted"; break;
        case mwfl::SignatureStatus::Invalid: entry.signature_status = L"Invalid"; break;
        case mwfl::SignatureStatus::RevocationUnavailable:
          entry.signature_status = L"Revocation unavailable";
          break;
      }
    }
    metadata_cache.emplace(std::move(cache_key),
                           TargetMetadata{entry.target_version, entry.signature_status});
  }
  if (SUCCEEDED(initialized)) ::CoUninitialize();
  return result;
}

OperationResult Disable(const StartupEntry& entry) {
  if (entry.state != StartupState::enabled) return {false, L"Entry is already disabled", 0};
  if (entry.source == StartupSource::scheduled_task) return SetTaskEnabled(entry, false);
  return entry.source == StartupSource::registry_run ? MoveRegistry(entry, false)
                                                     : MoveFolder(entry, false);
}
OperationResult Enable(const StartupEntry& entry) {
  if (entry.state != StartupState::disabled) return {false, L"Entry is already enabled", 0};
  if (entry.source == StartupSource::scheduled_task) return SetTaskEnabled(entry, true);
  return entry.source == StartupSource::registry_run ? MoveRegistry(entry, true)
                                                     : MoveFolder(entry, true);
}
OperationResult Delete(const StartupEntry& entry) {
  if (entry.source == StartupSource::scheduled_task) {
    ComPtr<ITaskFolder> folder;
    std::wstring name;
    if (!OpenTask(entry.location, &folder, &name) || !folder)
      return {false, L"Scheduled task could not be opened", ERROR_FILE_NOT_FOUND};
    const UniqueBstr task_name(::SysAllocString(name.c_str()));
    const auto status = folder->DeleteTask(task_name.get(), 0);
    return SUCCEEDED(status) ? OperationResult{true, L"Scheduled task deleted", 0}
                             : Failure(L"Delete scheduled task", HRESULT_CODE(status));
  }
  if (entry.source == StartupSource::registry_run) {
    HKEY root = nullptr;
    REGSAM view = 0;
    std::wstring name;
    if (!ParseRegistryId(entry, root, view, name)) return {false, L"Invalid registry identity", 0};
    HKEY raw = nullptr;
    const auto path = entry.state == StartupState::enabled ? kRunKey : kDisabledKey;
    auto status = ::RegOpenKeyExW(root, path, 0, KEY_SET_VALUE | view, &raw);
    if (status != ERROR_SUCCESS) return Failure(L"Open startup key", status);
    UniqueRegKey key(raw);
    status = ::RegDeleteValueW(key.get(), name.c_str());
    if (status != ERROR_SUCCESS) return Failure(L"Delete startup value", status);
    if (::RegQueryValueExW(key.get(), name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
        ERROR_SUCCESS)
      return {false, L"Windows did not confirm deletion", ERROR_WRITE_FAULT};
    return {true, L"Startup entry deleted", 0};
  }
  std::error_code error;
  const bool removed = std::filesystem::remove(entry.location, error);
  if (error) return {false, L"Delete startup file: " + ErrorCodeText(error),
                     static_cast<std::uint32_t>(error.value())};
  return {removed, removed ? L"Startup entry deleted" : L"Startup file was not found", 0};
}

OperationResult AddCurrentUserRun(std::wstring name, const std::filesystem::path& executable) {
  if (name.empty() || executable.empty() || !executable.is_absolute() ||
      !std::filesystem::is_regular_file(executable))
    return {false, L"Choose an existing executable with an absolute path", ERROR_INVALID_PARAMETER};
  HKEY raw = nullptr;
  const auto status = ::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                                        nullptr, &raw, nullptr);
  if (status != ERROR_SUCCESS) return Failure(L"Open startup key", status);
  UniqueRegKey key(raw);
  DWORD ignored = 0;
  if (::RegQueryValueExW(key.get(), name.c_str(), nullptr, nullptr, nullptr, &ignored) ==
      ERROR_SUCCESS)
    return {false, L"An entry with this name already exists", ERROR_ALREADY_EXISTS};
  const auto command = L"\"" + executable.wstring() + L"\"";
  const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
  const auto written = ::RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(command.c_str()), bytes);
  if (written != ERROR_SUCCESS) return Failure(L"Add startup entry", written);
  DWORD verified_size = 0;
  if (::RegQueryValueExW(key.get(), name.c_str(), nullptr, nullptr, nullptr, &verified_size) !=
          ERROR_SUCCESS ||
      verified_size != bytes)
    return {false, L"Windows did not confirm the new startup entry", ERROR_WRITE_FAULT};
  return {true, L"Startup entry added", 0};
}

}  // namespace startup_manager
