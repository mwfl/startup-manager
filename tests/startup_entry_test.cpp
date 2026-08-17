#include "startup_entry.h"

#include <cassert>

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
}
