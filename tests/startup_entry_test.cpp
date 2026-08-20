#include "startup_manager.h"

#include <cassert>
#include <stdexcept>
#include <unordered_set>

int main() {
  using namespace startup_manager;
  StartupEntry entry{.id = L"hkcu:64:run:Example", .name = L"Example",
                     .source = StartupSource::registry_run,
                     .scope = StartupScope::current_user, .state = StartupState::enabled};
  assert(!entry.id.empty());
  assert(entry.scope == StartupScope::current_user);
  assert(ExtractExecutable(L"\"C:\\Program Files\\Example\\app.exe\" --quiet") ==
         L"C:\\Program Files\\Example\\app.exe");
  assert(ExtractExecutable(L"C:\\Tools\\app.exe --quiet") == L"C:\\Tools\\app.exe");
  assert(ExtractExecutable(L"  C:\\Tools\\app.exe\t--quiet") == L"C:\\Tools\\app.exe");
  assert(ExtractExecutable(L"\"C:\\Program Files\\Example\\app.exe\"") ==
         L"C:\\Program Files\\Example\\app.exe");
  assert(ExtractExecutable(L"   ").empty());
  const auto discovered = Discover();
  std::unordered_set<std::wstring> identities;
  for (const auto& item : discovered.entries) {
    if (item.id.empty() || item.name.empty())
      throw std::runtime_error("discovered entry has no stable identity or name");
    if (!identities.insert(item.id).second)
      throw std::runtime_error("discovered entry identity is duplicated");
    if (_wcsicmp(item.name.c_str(), L"desktop") == 0 &&
        item.source == StartupSource::startup_folder)
      throw std::runtime_error("desktop.ini must not be exposed as a startup entry");
  }
}
