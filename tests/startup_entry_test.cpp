#include "startup_entry.h"

#include <cassert>

int main() {
  using namespace startup_manager;
  StartupEntry entry{.id = L"registry:hkcu:64:Example",
                     .fingerprint = L"fixture",
                     .name = L"Example",
                     .source = StartupSource::registry_run,
                     .scope = StartupScope::current_user,
                     .state = StartupState::enabled};
  assert(!entry.id.empty());
  assert(entry.scope == StartupScope::current_user);
  assert(!entry.requires_elevation);
}

