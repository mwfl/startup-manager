#include <algorithm>
#include <memory>
#include <mwfl/mwfl.h>
#include <shellapi.h>

#include "resource.h"
#include "startup_manager.h"
#include <mwfl/app_support/update_checker.h>

using mwfl::operator""_dip;
namespace {
mwfl::app_support::UpdateChecker g_update_checker;
constexpr mwfl::ControlId kRefresh{100}, kAdd{101}, kDisable{102}, kEnable{103},
    kDelete{104}, kList{105}, kFilter{106}, kAdmin{107};

class EntryListModel final : public mwfl::VirtualListModel {
 public:
  void Set(std::shared_ptr<const startup_manager::DiscoveryResult> result,
           std::wstring filter) {
    result_ = std::move(result);
    rows_.clear();
    std::ranges::transform(filter, filter.begin(), ::towlower);
    if (!result_) return;
    rows_.reserve(result_->entries.size());
    for (std::size_t i = 0; i < result_->entries.size(); ++i) {
      auto haystack = result_->entries[i].name + L" " + result_->entries[i].command + L" " +
                      result_->entries[i].location;
      std::ranges::transform(haystack, haystack.begin(), ::towlower);
      if (filter.empty() || haystack.find(filter) != std::wstring::npos) rows_.push_back(i);
    }
  }
  std::size_t GetRowCount() const noexcept override { return rows_.size(); }
  mwfl::ListItemId GetRowId(std::size_t row) const noexcept override {
    return row < rows_.size() ? mwfl::ListItemId{rows_[row] + 1} : mwfl::ListItemId{};
  }
  std::wstring GetCellText(std::size_t row, int column) const override {
    if (!result_ || row >= rows_.size()) return {};
    const auto& entry = result_->entries[rows_[row]];
    switch (column) {
      case 0: return entry.name;
      case 1: return startup_manager::StateName(entry.state);
      case 2: return startup_manager::SourceName(entry.source);
      case 3: return startup_manager::ScopeName(entry.scope);
      case 4: return entry.target_exists ? L"Available" : L"Target missing";
      case 5: return entry.command;
      default: return {};
    }
  }
  const startup_manager::StartupEntry* Get(mwfl::ListItemId id) const noexcept {
    if (!result_ || id.value == 0 || id.value > result_->entries.size()) return nullptr;
    return &result_->entries[id.value - 1];
  }
  std::size_t VisibleCount() const noexcept { return rows_.size(); }

 private:
  std::shared_ptr<const startup_manager::DiscoveryResult> result_;
  std::vector<std::size_t> rows_;
};

class MainWindow final : public mwfl::WindowBase {
 public:
  void BuildUI() override {
    SetTitle(L"MWFL Startup Manager");
    mwfl::ControlHost ui{*this};
    ui.Add(refresh_, kRefresh, L"Refresh");
    ui.Add(add_, kAdd, L"Add application…");
    ui.Add(disable_, kDisable, L"Disable");
    ui.Add(enable_, kEnable, L"Enable");
    ui.Add(delete_, kDelete, L"Delete…");
    ui.Add(admin_, kAdmin, L"Restart as administrator");
    ui.Add(filter_, kFilter, L"");
    ui.Add(summary_, L"Inspecting startup entries…");
    ui.Add(list_, kList, mwfl::ListViewOptions{.virtual_data = true});
    mwfl::TextBoxOptions details_options;
    details_options.style |= ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY;
    ui.Add(details_, L"Select an entry to inspect its exact command and location.",
           details_options);
    filter_.SetCueBanner(L"Filter by name, command, or location");
    list_.SetExtendedListStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES |
                               LVS_EX_HEADERDRAGDROP);
    mwfl::Must(mwfl::AddColumns(list_, {{L"Name", 230}, {L"Status", 90}, {L"Source", 135},
                                        {L"Scope", 105}, {L"Target", 105}, {L"Command", 620}}),
               "add columns");
    model_ = std::make_shared<EntryListModel>();
    mwfl::Must(list_.SetVirtualModel(model_), "attach model");
    mwfl::Must(mwfl::SetAccessibleName(list_.GetHwnd(), L"Startup entries"),
               "set accessible name");
    SetLayout(mwfl::Column()
                  .Margin(10.0_dip)
                  .Gap(8.0_dip)
                  .Add(mwfl::Row()
                           .Gap(6.0_dip)
                           .Add(refresh_, mwfl::Auto())
                           .Add(add_, mwfl::Auto())
                           .Add(disable_, mwfl::Auto())
                           .Add(enable_, mwfl::Auto())
                           .Add(delete_, mwfl::Auto())
                           .Add(admin_, mwfl::Auto())
                           .Add(filter_, mwfl::Stretch()),
                       mwfl::Auto())
                  .Add(summary_, mwfl::Auto())
                  .Add(list_, mwfl::Stretch())
                  .Add(details_, mwfl::Fixed(150.0_dip)));
    SetAppearance({});
    Refresh();
    g_update_checker.Attach(
        GetHwnd(), {L"MWFL Startup Manager", L"startup-manager", MWFL_APP_VERSION,
                    L"Software\\mwfl\\Examples\\StartupManager\\Updates"});
  }

  mwfl::EventResult OnCommand(const mwfl::CommandEvent& event) override {
    if (event.IsClicked(refresh_)) { Refresh(); return mwfl::EventResult::Handled(); }
    if (event.Is(filter_, EN_CHANGE)) { ApplyFilter(); return mwfl::EventResult::Handled(); }
    if (event.IsClicked(add_)) { Add(); return mwfl::EventResult::Handled(); }
    if (event.IsClicked(disable_)) { Mutate(0); return mwfl::EventResult::Handled(); }
    if (event.IsClicked(enable_)) { Mutate(1); return mwfl::EventResult::Handled(); }
    if (event.IsClicked(delete_)) { Mutate(2); return mwfl::EventResult::Handled(); }
    if (event.IsClicked(admin_)) {
      wchar_t executable[MAX_PATH]{};
      if (::GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(
            GetHwnd(), L"runas", executable, nullptr, nullptr, SW_SHOWNORMAL));
        if (result > 32) ::PostMessageW(GetHwnd(), WM_CLOSE, 0, 0);
      }
      return mwfl::EventResult::Handled();
    }
    return mwfl::EventResult::Propagate();
  }

  mwfl::EventResult OnNotify(const mwfl::NotifyEvent& event) override {
    if (!event.IsFrom(list_)) return mwfl::EventResult::Propagate();
    LRESULT native = 0;
    if (list_.HandleNotification(event.header, native))
      return mwfl::EventResult::Handled(native);
    const auto decoded = list_.DecodeNotification(event.header);
    if (decoded && decoded->kind == mwfl::ListViewNotificationKind::selection_changed)
      ShowSelected();
    return mwfl::EventResult::Propagate();
  }

 private:
  const startup_manager::StartupEntry* Selected() const {
    const auto ids = list_.GetSelectedItemIds();
    return ids.empty() ? nullptr : model_->Get(ids.front());
  }
  void Refresh() {
    auto discovered = std::make_shared<startup_manager::DiscoveryResult>(startup_manager::Discover());
    result_ = std::move(discovered);
    admin_.SetEnabled(!startup_manager::IsProcessElevated());
    ApplyFilter();
    details_.SetText(result_->diagnostics.empty()
                         ? L"Select an entry to inspect its exact command and location."
                         : L"Some locations could not be read. Entries that were available are shown.");
  }
  void ApplyFilter() {
    model_->Set(result_, filter_.GetText());
    mwfl::Must(list_.SetVirtualModel(model_), "refresh model");
    list_.RefreshVirtualModel();
    const auto total = result_ ? result_->entries.size() : 0;
    const auto disabled = result_ ? std::ranges::count_if(result_->entries, [](const auto& e) {
      return e.state == startup_manager::StartupState::disabled;
    }) : 0;
    summary_.SetText(std::to_wstring(total) + L" startup entries · " +
                     std::to_wstring(disabled) + L" disabled · " +
                     std::to_wstring(model_->VisibleCount()) + L" shown");
  }
  void ShowSelected() {
    const auto* entry = Selected();
    if (!entry) return;
    details_.SetText(L"Name: " + entry->name + L"\r\nStatus: " +
                     startup_manager::StateName(entry->state) + L"\r\nSource: " +
                     startup_manager::SourceName(entry->source) + L" · " +
                     startup_manager::ScopeName(entry->scope) + L"\r\nLocation: " +
                     entry->location + L"\r\nCommand: " + entry->command +
                     (entry->target_version.empty() ? L"" : L"\r\nVersion: " + entry->target_version) +
                     (entry->signature_status.empty() ? L"" : L"\r\nSignature: " + entry->signature_status) +
                     (entry->target_exists ? L"" : L"\r\nWarning: target could not be found."));
    disable_.SetEnabled(entry->writable &&
                        entry->state == startup_manager::StartupState::enabled);
    enable_.SetEnabled(entry->writable &&
                       entry->state == startup_manager::StartupState::disabled);
    delete_.SetEnabled(entry->writable);
  }
  void Add() {
    const auto chosen = mwfl::ShowOpenFileDialog(
        {.owner = GetHwnd(), .title = L"Choose an application to run at sign-in",
         .filters = {{L"Applications", L"*.exe"}, {L"All files", L"*.*"}}});
    if (!chosen.accepted) return;
    const auto outcome = startup_manager::AddCurrentUserRun(chosen.path.stem().wstring(), chosen.path);
    ::MessageBoxW(GetHwnd(), outcome.message.c_str(), L"Startup Manager",
                  MB_OK | (outcome.succeeded ? MB_ICONINFORMATION : MB_ICONERROR));
    Refresh();
  }
  void Mutate(int operation) {
    const auto* entry = Selected();
    if (!entry) { ::MessageBoxW(GetHwnd(), L"Select a startup entry first.", L"Startup Manager",
                                MB_OK | MB_ICONINFORMATION); return; }
    if (!entry->writable) {
      ::MessageBoxW(GetHwnd(), L"Restart as administrator to change this machine-wide entry.",
                    L"Startup Manager", MB_OK | MB_ICONINFORMATION);
      return;
    }
    if (operation == 2) {
      const auto prompt = L"Permanently delete this startup entry?\r\n\r\n" + entry->name +
                          L"\r\n" + entry->location;
      if (::MessageBoxW(GetHwnd(), prompt.c_str(), L"Confirm deletion",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
    }
    const auto outcome = operation == 0 ? startup_manager::Disable(*entry)
                         : operation == 1 ? startup_manager::Enable(*entry)
                                          : startup_manager::Delete(*entry);
    ::MessageBoxW(GetHwnd(), outcome.message.c_str(), L"Startup Manager",
                  MB_OK | (outcome.succeeded ? MB_ICONINFORMATION : MB_ICONERROR));
    Refresh();
  }

  std::shared_ptr<startup_manager::DiscoveryResult> result_;
  std::shared_ptr<EntryListModel> model_;
  mwfl::Button refresh_, add_, disable_, enable_, delete_, admin_;
  mwfl::TextBox filter_, details_;
  mwfl::Label summary_;
  mwfl::ListView list_;
};
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  const HICON icon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_STARTUP_MANAGER));
  return mwfl::RunApplication<MainWindow>(
      instance, show,
      {.title = L"MWFL Startup Manager",
       .initial_bounds = {{60.0_dip, 60.0_dip}, {1400.0_dip, 850.0_dip}},
       .use_default_bounds = false, .icon = icon, .small_icon = icon});
}
