#pragma once

#include <cstdint>
#include <string>

namespace startup_manager {

enum class StartupSource { registry_run, startup_folder, scheduled_task };
enum class StartupScope { current_user, all_users };
enum class StartupState { enabled, disabled };

struct StartupEntry {
  std::wstring id;
  std::wstring name;
  std::wstring command;
  std::wstring location;
  StartupSource source{};
  StartupScope scope{};
  StartupState state{};
  bool writable{};
  bool target_exists{};
  std::wstring target_version;
  std::wstring signature_status;
};

struct OperationResult {
  bool succeeded{};
  std::wstring message;
  std::uint32_t native_error{};
};

std::wstring SourceName(StartupSource source);
std::wstring ScopeName(StartupScope scope);
std::wstring StateName(StartupState state);
std::wstring ExtractExecutable(std::wstring_view command);

}  // namespace startup_manager
