#pragma once

#include "startup_entry.h"

#include <filesystem>
#include <string>
#include <vector>

namespace startup_manager {

struct DiscoveryResult {
  std::vector<StartupEntry> entries;
  std::vector<std::wstring> diagnostics;
};

DiscoveryResult Discover();
OperationResult Disable(const StartupEntry& entry);
OperationResult Enable(const StartupEntry& entry);
OperationResult Delete(const StartupEntry& entry);
OperationResult AddCurrentUserRun(std::wstring name, const std::filesystem::path& executable);

}  // namespace startup_manager
