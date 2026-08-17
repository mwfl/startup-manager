#pragma once

#include "startup_entry.h"

#include <stop_token>
#include <string>
#include <vector>

namespace startup_manager {

struct Diagnostic {
  std::wstring location;
  std::wstring message;
  std::uint32_t native_error{};
};

struct DiscoveryResult {
  std::vector<StartupEntry> entries;
  std::vector<Diagnostic> diagnostics;
};

struct MutationResult {
  bool succeeded{};
  std::wstring recovery_id;
  std::wstring message;
  std::uint32_t native_error{};
};

class IStartupProvider {
 public:
  virtual ~IStartupProvider() = default;
  virtual DiscoveryResult Discover(std::stop_token stop) = 0;
  virtual MutationResult Disable(const StartupEntry& expected) = 0;
  virtual MutationResult Enable(const StartupEntry& expected) = 0;
  virtual MutationResult Delete(const StartupEntry& expected) = 0;
};

}  // namespace startup_manager

