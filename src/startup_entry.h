#pragma once

#include <cstdint>
#include <string>

namespace startup_manager {

enum class StartupSource { registry_run, startup_folder, scheduled_task };
enum class StartupScope { current_user, all_users };
enum class StartupState { enabled, disabled, unavailable };

struct StartupEntry {
  std::wstring id;
  std::wstring fingerprint;
  std::wstring name;
  std::wstring command;
  std::wstring executable;
  std::wstring arguments;
  std::wstring location;
  std::wstring publisher;
  StartupSource source{};
  StartupScope scope{};
  StartupState state{};
  bool requires_elevation{};
};

}  // namespace startup_manager

