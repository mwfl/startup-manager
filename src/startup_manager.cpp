#include "startup_manager.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <windows.h>
#include <knownfolders.h>
#include <memory>
#include <shlobj.h>

namespace startup_manager {
namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kDisabledKey[] = L"Software\\MWFL\\StartupManager\\DisabledRun";

struct RegKeyCloser { void operator()(HKEY value) const noexcept { if (value) ::RegCloseKey(value); } };
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

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
  return error ? OperationResult{false, L"Move startup entry: " + ErrorCodeText(error),
                                 static_cast<std::uint32_t>(error.value())}
               : OperationResult{true, enabling ? L"Startup entry enabled"
                                                  : L"Startup entry disabled", 0};
}
}  // namespace

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
  for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
    DiscoverRegistryView(result, HKEY_CURRENT_USER, StartupScope::current_user, view, kRunKey,
                         StartupState::enabled);
    DiscoverRegistryView(result, HKEY_CURRENT_USER, StartupScope::current_user, view,
                         kDisabledKey, StartupState::disabled);
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
  std::ranges::sort(result.entries, [](const auto& a, const auto& b) {
    if (a.state != b.state) return a.state < b.state;
    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return result;
}

OperationResult Disable(const StartupEntry& entry) {
  if (entry.state != StartupState::enabled) return {false, L"Entry is already disabled", 0};
  return entry.source == StartupSource::registry_run ? MoveRegistry(entry, false)
                                                     : MoveFolder(entry, false);
}
OperationResult Enable(const StartupEntry& entry) {
  if (entry.state != StartupState::disabled) return {false, L"Entry is already enabled", 0};
  return entry.source == StartupSource::registry_run ? MoveRegistry(entry, true)
                                                     : MoveFolder(entry, true);
}
OperationResult Delete(const StartupEntry& entry) {
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
    return status == ERROR_SUCCESS ? OperationResult{true, L"Startup entry deleted", 0}
                                   : Failure(L"Delete startup value", status);
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
  return written == ERROR_SUCCESS ? OperationResult{true, L"Startup entry added", 0}
                                  : Failure(L"Add startup entry", written);
}

}  // namespace startup_manager
