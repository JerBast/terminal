// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowEmperor.h"

#include "../inc/WindowingBehavior.h"

#include "../../types/inc/utils.hpp"

#include "../WinRTUtils/inc/WtExeUtils.h"

#include "resource.h"
#include "NotificationIcon.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation;
using namespace ::Microsoft::Console;
using namespace std::chrono_literals;
using VirtualKeyModifiers = winrt::Windows::System::VirtualKeyModifiers;

#define TERMINAL_MESSAGE_CLASS_NAME L"TERMINAL_MESSAGE_CLASS"
extern "C" IMAGE_DOS_HEADER __ImageBase;

WindowEmperor::WindowEmperor() noexcept :
    _app{}
{
    _manager.FindTargetWindowRequested([this](const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                              const winrt::Microsoft::Terminal::Remoting::FindTargetWindowArgs& findWindowArgs) {
        {
            const auto targetWindow = _app.Logic().FindTargetWindow(findWindowArgs.Args().Commandline());
            findWindowArgs.ResultTargetWindow(targetWindow.WindowId());
            findWindowArgs.ResultTargetWindowName(targetWindow.WindowName());
        }
    });

    _dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
}

WindowEmperor::~WindowEmperor()
{
    _app.Close();
    _app = nullptr;
}

void _buildArgsFromCommandline(std::vector<winrt::hstring>& args)
{
    if (auto commandline{ GetCommandLineW() })
    {
        auto argc = 0;

        // Get the argv, and turn them into a hstring array to pass to the app.
        wil::unique_any<LPWSTR*, decltype(&::LocalFree), ::LocalFree> argv{ CommandLineToArgvW(commandline, &argc) };
        if (argv)
        {
            for (auto& elem : wil::make_range(argv.get(), argc))
            {
                args.emplace_back(elem);
            }
        }
    }
    if (args.empty())
    {
        args.emplace_back(L"wt.exe");
    }
}

bool WindowEmperor::HandleCommandlineArgs()
{
    std::vector<winrt::hstring> args;
    _buildArgsFromCommandline(args);
    auto cwd{ wil::GetCurrentDirectoryW<std::wstring>() };

    Remoting::CommandlineArgs eventArgs{ { args }, { cwd } };

    const auto isolatedMode{ _app.Logic().IsolatedMode() };

    const auto result = _manager.ProposeCommandline(eventArgs, isolatedMode);

    if (result.ShouldCreateWindow())
    {
        _createNewWindowThread(Remoting::WindowRequestedArgs{ result, eventArgs });

        _becomeMonarch();
    }
    else
    {
        const auto res = _app.Logic().GetParseCommandlineMessage(eventArgs.Commandline());
        if (!res.Message.empty())
        {
            AppHost::s_DisplayMessageBox(res);
            ExitThread(res.ExitCode);
        }
    }

    return result.ShouldCreateWindow();
}

void WindowEmperor::WaitForWindows()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
}

void WindowEmperor::_createNewWindowThread(const Remoting::WindowRequestedArgs& args)
{
    Remoting::Peasant peasant{ _manager.CreatePeasant(args) };
    auto window{ std::make_shared<WindowThread>(_app.Logic(), args, _manager, peasant) };
    std::weak_ptr<WindowEmperor> weakThis{ weak_from_this() };

    std::thread t([weakThis, window]() {
        window->CreateHost();

        if (auto self{ weakThis.lock() })
        {
            self->_windowStartedHandler(window);
        }

        window->RunMessagePump();

        if (auto self{ weakThis.lock() })
        {
            self->_windowExitedHandler(window->Peasant().GetID());
        }
    });
    LOG_IF_FAILED(SetThreadDescription(t.native_handle(), L"Window Thread"));

    t.detach();
}

// Handler for a WindowThread's Started event, which it raises once the window
// thread starts and XAML is ready to go on that thread. Set up some callbacks
// now that we know this window is set up and ready to go.
// Q: Why isn't adding these callbacks just a part of _createNewWindowThread?
// A: Until the thread actually starts, the AppHost (and its Logic()) haven't
// been ctor'd or initialized, so trying to add callbacks immediately will A/V
void WindowEmperor::_windowStartedHandler(const std::shared_ptr<WindowThread>& sender)
{
    // Add a callback to the window's logic to let us know when the window's
    // quake mode state changes. We'll use this to check if we need to add
    // or remove the notification icon.
    sender->Logic().IsQuakeWindowChanged({ this, &WindowEmperor::_windowIsQuakeWindowChanged });
    sender->UpdateSettingsRequested({ this, &WindowEmperor::_windowRequestUpdateSettings });

    // Summon the window to the foreground, since we might not _currently_ be in
    // the foreground, but we should act like the new window is.
    //
    // TODO: GH#14957 - use AllowSetForeground from the original wt.exe instead
    Remoting::SummonWindowSelectionArgs args{};
    args.OnCurrentDesktop(false);
    args.WindowID(sender->Peasant().GetID());
    args.SummonBehavior().MoveToCurrentDesktop(false);
    args.SummonBehavior().ToggleVisibility(false);
    args.SummonBehavior().DropdownDuration(0);
    args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::InPlace);
    _manager.SummonWindow(args);

    // Now that the window is ready to go, we can add it to our list of windows,
    // because we know it will be well behaved.
    //
    // Be sure to only modify the list of windows under lock.
    {
        auto lockedWindows{ _windows.lock() };
        lockedWindows->push_back(sender);
    }
}
void WindowEmperor::_windowExitedHandler(uint64_t senderID)
{
    auto lockedWindows{ _windows.lock() };

    // find the window in _windows who's peasant's Id matches the peasant's Id
    // and remove it
    std::erase_if(*lockedWindows,
                  [&](const auto& w) {
                      return w->Peasant().GetID() == senderID;
                  });

    if (lockedWindows->size() == 0)
    {
        _close();
    }
}
// Method Description:
// - Set up all sorts of handlers now that we've determined that we're a process
//   that will end up hosting the windows. These include:
//   - Setting up a message window to handle hotkeys and notification icon
//     invokes.
//   - Setting up the global hotkeys.
//   - Setting up the notification icon.
//   - Setting up callbacks for when the settings change.
//   - Setting up callbacks for when the number of windows changes.
//   - Setting up the throttled func for layout persistence. Arguments:
// - <none>
void WindowEmperor::_becomeMonarch()
{
    // Add a callback to the window manager so that when the Monarch wants a new
    // window made, they come to us
    _manager.RequestNewWindow([this](auto&&, const Remoting::WindowRequestedArgs& args) {
        _createNewWindowThread(args);
    });

    _createMessageWindow();

    _setupGlobalHotkeys();

    // When the settings change, we'll want to update our global hotkeys and our
    // notification icon based on the new settings.
    _app.Logic().SettingsChanged([this](auto&&, const TerminalApp::SettingsLoadEventArgs& args) {
        if (SUCCEEDED(args.Result()))
        {
            _setupGlobalHotkeys();
            _checkWindowsForNotificationIcon();
        }
    });

    // On startup, immediately check if we need to show the notification icon.
    _checkWindowsForNotificationIcon();

    // Set the number of open windows (so we know if we are the last window)
    // and subscribe for updates if there are any changes to that number.

    _revokers.WindowCreated = _manager.WindowCreated(winrt::auto_revoke, { this, &WindowEmperor::_numberOfWindowsChanged });
    _revokers.WindowClosed = _manager.WindowClosed(winrt::auto_revoke, { this, &WindowEmperor::_numberOfWindowsChanged });

    // If the monarch receives a QuitAll event it will signal this event to be
    // ran before each peasant is closed.
    _revokers.QuitAllRequested = _manager.QuitAllRequested(winrt::auto_revoke, { this, &WindowEmperor::_quitAllRequested });

    // The monarch should be monitoring if it should save the window layout.
    // We want at least some delay to prevent the first save from overwriting
    _getWindowLayoutThrottler.emplace(std::move(std::chrono::seconds(10)), std::move([this]() { _saveWindowLayoutsRepeat(); }));
    _getWindowLayoutThrottler.value()();

    // BODGY
    //
    // We've got a weird crash that happens terribly inconsistently, but pretty
    // readily on migrie's laptop, only in Debug mode. Apparently, there's some
    // weird ref-counting magic that goes on during teardown, and our
    // Application doesn't get closed quite right, which can cause us to crash
    // into the debugger. This of course, only happens on exit, and happens
    // somewhere in the XamlHost.dll code.
    //
    // Crazily, if we _manually leak the Application_ here, then the crash
    // doesn't happen. This doesn't matter, because we really want the
    // Application to live for _the entire lifetime of the process_, so the only
    // time when this object would actually need to get cleaned up is _during
    // exit_. So we can safely leak this Application object, and have it just
    // get cleaned up normally when our process exits.
    auto a{ _app };
    ::winrt::detach_abi(a);
}

// sender and args are always nullptr
void WindowEmperor::_numberOfWindowsChanged(const winrt::Windows::Foundation::IInspectable&,
                                            const winrt::Windows::Foundation::IInspectable&)
{
    if (_getWindowLayoutThrottler)
    {
        _getWindowLayoutThrottler.value()();
    }

    // If we closed out the quake window, and don't otherwise need the tray
    // icon, let's get rid of it.
    _checkWindowsForNotificationIcon();
}

// Raised from our windowManager (on behalf of the monarch). We respond by
// giving the monarch an async function that the manager should wait on before
// completing the quit.
void WindowEmperor::_quitAllRequested(const winrt::Windows::Foundation::IInspectable&,
                                      const winrt::Microsoft::Terminal::Remoting::QuitAllRequestedArgs& args)
{
    // Make sure that the current timer is destroyed so that it doesn't attempt
    // to run while we are in the middle of quitting.
    if (_getWindowLayoutThrottler.has_value())
    {
        _getWindowLayoutThrottler.reset();
    }

    // Tell the monarch to wait for the window layouts to save before
    // everyone quits.
    args.BeforeQuitAllAction(_saveWindowLayouts());
}

#pragma region LayoutPersistence

winrt::Windows::Foundation::IAsyncAction WindowEmperor::_saveWindowLayouts()
{
    // Make sure we run on a background thread to not block anything.
    co_await winrt::resume_background();

    if (_app.Logic().ShouldUsePersistedLayout())
    {
        try
        {
            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Collect",
                              TraceLoggingDescription("Logged when collecting window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));

            const auto layoutJsons = _manager.GetAllWindowLayouts();

            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Save",
                              TraceLoggingDescription("Logged when writing window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));

            _app.Logic().SaveWindowLayoutJsons(layoutJsons);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Failed",
                              TraceLoggingDescription("An error occurred when collecting or writing window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));
        }
    }

    co_return;
}

winrt::fire_and_forget WindowEmperor::_saveWindowLayoutsRepeat()
{
    // Make sure we run on a background thread to not block anything.
    co_await winrt::resume_background();

    co_await _saveWindowLayouts();

    // Don't need to save too frequently.
    co_await winrt::resume_after(30s);

    // As long as we are supposed to keep saving, request another save.
    // This will be delayed by the throttler so that at most one save happens
    // per 10 seconds, if a save is requested by another source simultaneously.
    if (_getWindowLayoutThrottler.has_value())
    {
        TraceLoggingWrite(g_hWindowsTerminalProvider,
                          "AppHost_requestGetLayout",
                          TraceLoggingDescription("Logged when triggering a throttled write of the window state"),
                          TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                          TraceLoggingKeyword(TIL_KEYWORD_TRACE));

        _getWindowLayoutThrottler.value()();
    }
}
#pragma endregion

#pragma region WindowProc

static WindowEmperor* GetThisFromHandle(HWND const window) noexcept
{
    const auto data = GetWindowLongPtr(window, GWLP_USERDATA);
    return reinterpret_cast<WindowEmperor*>(data);
}
[[nodiscard]] LRESULT __stdcall WindowEmperor::_wndProc(HWND const window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
{
    WINRT_ASSERT(window);

    if (WM_NCCREATE == message)
    {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        WindowEmperor* that = static_cast<WindowEmperor*>(cs->lpCreateParams);
        WINRT_ASSERT(that);
        WINRT_ASSERT(!that->_window);
        that->_window = wil::unique_hwnd(window);
        SetWindowLongPtr(that->_window.get(), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
    }
    else if (WindowEmperor* that = GetThisFromHandle(window))
    {
        return that->_messageHandler(message, wparam, lparam);
    }

    return DefWindowProc(window, message, wparam, lparam);
}
void WindowEmperor::_createMessageWindow()
{
    WNDCLASS wc{};
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    wc.lpszClassName = TERMINAL_MESSAGE_CLASS_NAME;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowEmperor::_wndProc;
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClass(&wc);
    WINRT_ASSERT(!_window);

    WINRT_VERIFY(CreateWindow(wc.lpszClassName,
                              L"Windows Terminal",
                              0,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              HWND_MESSAGE,
                              nullptr,
                              wc.hInstance,
                              this));
}

LRESULT WindowEmperor::_messageHandler(UINT const message, WPARAM const wParam, LPARAM const lParam) noexcept
{
    // use C++11 magic statics to make sure we only do this once.
    // This won't change over the lifetime of the application
    static const UINT WM_TASKBARCREATED = []() { return RegisterWindowMessageW(L"TaskbarCreated"); }();

    switch (message)
    {
    case WM_HOTKEY:
    {
        _hotkeyPressed(static_cast<long>(wParam));
        return 0;
    }
    case CM_NOTIFY_FROM_NOTIFICATION_AREA:
    {
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        {
            _notificationIcon->NotificationIconPressed();
            return 0;
        }
        case WM_CONTEXTMENU:
        {
            const til::point eventPoint{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            _notificationIcon->ShowContextMenu(eventPoint, _manager.GetPeasantInfos());
            return 0;
        }
        }
        break;
    }
    case WM_MENUCOMMAND:
    {
        _notificationIcon->MenuItemSelected((HMENU)lParam, (UINT)wParam);
        return 0;
    }
    default:
    {
        // We'll want to receive this message when explorer.exe restarts
        // so that we can re-add our icon to the notification area.
        // This unfortunately isn't a switch case because we register the
        // message at runtime.
        if (message == WM_TASKBARCREATED)
        {
            _notificationIcon->ReAddNotificationIcon();
            return 0;
        }
    }
    }
    return DefWindowProc(_window.get(), message, wParam, lParam);
}

winrt::fire_and_forget WindowEmperor::_close()
{
    // Important! Switch back to the main thread for the emperor. That way, the
    // quit will go to the emperor's message pump.
    co_await wil::resume_foreground(_dispatcher);
    PostQuitMessage(0);
}

#pragma endregion
#pragma region GlobalHotkeys

// Method Description:
// - Called when the monarch failed to summon a window for a given set of
//   SummonWindowSelectionArgs. In this case, we should create the specified
//   window ourselves.
// - This is to support the scenario like `globalSummon(Name="_quake")` being
//   used to summon the window if it already exists, or create it if it doesn't.
// Arguments:
// - args: Contains information on how we should name the window
// Return Value:
// - <none>
static winrt::fire_and_forget _createNewTerminalWindow(Settings::Model::GlobalSummonArgs args)
{
    // Hop to the BG thread
    co_await winrt::resume_background();

    // This will get us the correct exe for dev/preview/release. If you
    // don't stick this in a local, it'll get mangled by ShellExecute. I
    // have no idea why.
    const auto exePath{ GetWtExePath() };

    // If we weren't given a name, then just use new to force the window to be
    // unnamed.
    winrt::hstring cmdline{
        fmt::format(L"-w {}",
                    args.Name().empty() ? L"new" :
                                          args.Name())
    };

    SHELLEXECUTEINFOW seInfo{ 0 };
    seInfo.cbSize = sizeof(seInfo);
    seInfo.fMask = SEE_MASK_NOASYNC;
    seInfo.lpVerb = L"open";
    seInfo.lpFile = exePath.c_str();
    seInfo.lpParameters = cmdline.c_str();
    seInfo.nShow = SW_SHOWNORMAL;
    LOG_IF_WIN32_BOOL_FALSE(ShellExecuteExW(&seInfo));

    co_return;
}

void WindowEmperor::_hotkeyPressed(const long hotkeyIndex)
{
    if (hotkeyIndex < 0 || static_cast<size_t>(hotkeyIndex) > _hotkeys.size())
    {
        return;
    }

    const auto& summonArgs = til::at(_hotkeys, hotkeyIndex);
    Remoting::SummonWindowSelectionArgs args{ summonArgs.Name() };

    // desktop:any - MoveToCurrentDesktop=false, OnCurrentDesktop=false
    // desktop:toCurrent - MoveToCurrentDesktop=true, OnCurrentDesktop=false
    // desktop:onCurrent - MoveToCurrentDesktop=false, OnCurrentDesktop=true
    args.OnCurrentDesktop(summonArgs.Desktop() == Settings::Model::DesktopBehavior::OnCurrent);
    args.SummonBehavior().MoveToCurrentDesktop(summonArgs.Desktop() == Settings::Model::DesktopBehavior::ToCurrent);
    args.SummonBehavior().ToggleVisibility(summonArgs.ToggleVisibility());
    args.SummonBehavior().DropdownDuration(summonArgs.DropdownDuration());

    switch (summonArgs.Monitor())
    {
    case Settings::Model::MonitorBehavior::Any:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::InPlace);
        break;
    case Settings::Model::MonitorBehavior::ToCurrent:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::ToCurrent);
        break;
    case Settings::Model::MonitorBehavior::ToMouse:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::ToMouse);
        break;
    }

    _manager.SummonWindow(args);
    if (args.FoundMatch())
    {
        // Excellent, the window was found. We have nothing else to do here.
    }
    else
    {
        // We should make the window ourselves.
        _createNewTerminalWindow(summonArgs);
    }
}

bool WindowEmperor::_registerHotKey(const int index, const winrt::Microsoft::Terminal::Control::KeyChord& hotkey) noexcept
{
    const auto vkey = hotkey.Vkey();
    auto hotkeyFlags = MOD_NOREPEAT;
    {
        const auto modifiers = hotkey.Modifiers();
        WI_SetFlagIf(hotkeyFlags, MOD_WIN, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Windows));
        WI_SetFlagIf(hotkeyFlags, MOD_ALT, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Menu));
        WI_SetFlagIf(hotkeyFlags, MOD_CONTROL, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Control));
        WI_SetFlagIf(hotkeyFlags, MOD_SHIFT, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Shift));
    }

    // TODO GH#8888: We should display a warning of some kind if this fails.
    // This can fail if something else already bound this hotkey.
    const auto result = ::RegisterHotKey(_window.get(), index, hotkeyFlags, vkey);
    LOG_LAST_ERROR_IF(!result);
    TraceLoggingWrite(g_hWindowsTerminalProvider,
                      "RegisterHotKey",
                      TraceLoggingDescription("Emitted when setting hotkeys"),
                      TraceLoggingInt64(index, "index", "the index of the hotkey to add"),
                      TraceLoggingUInt64(vkey, "vkey", "the key"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_WIN), "win", "is WIN in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_ALT), "alt", "is ALT in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_CONTROL), "control", "is CONTROL in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_SHIFT), "shift", "is SHIFT in the modifiers"),
                      TraceLoggingBool(result, "succeeded", "true if we succeeded"),
                      TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                      TraceLoggingKeyword(TIL_KEYWORD_TRACE));

    return result;
}

// Method Description:
// - Call UnregisterHotKey once for each previously registered hotkey.
// Return Value:
// - <none>
void WindowEmperor::_unregisterHotKey(const int index) noexcept
{
    TraceLoggingWrite(
        g_hWindowsTerminalProvider,
        "UnregisterHotKey",
        TraceLoggingDescription("Emitted when clearing previously set hotkeys"),
        TraceLoggingInt64(index, "index", "the index of the hotkey to remove"),
        TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
        TraceLoggingKeyword(TIL_KEYWORD_TRACE));

    LOG_IF_WIN32_BOOL_FALSE(::UnregisterHotKey(_window.get(), index));
}

winrt::fire_and_forget WindowEmperor::_setupGlobalHotkeys()
{
    // The hotkey MUST be registered on the main thread. It will fail otherwise!
    co_await wil::resume_foreground(_dispatcher);

    if (!_window)
    {
        // MSFT:36797001 There's a surprising number of hits of this callback
        // getting triggered during teardown. As a best practice, we really
        // should make sure _window exists before accessing it on any coroutine.
        // We might be getting called back after the app already began getting
        // cleaned up.
        co_return;
    }
    // Unregister all previously registered hotkeys.
    //
    // RegisterHotKey(), will not unregister hotkeys automatically.
    // If a hotkey with a given HWND and ID combination already exists
    // then a duplicate one will be added, which we don't want.
    // (Additionally we want to remove hotkeys that were removed from the settings.)
    for (auto i = 0, count = gsl::narrow_cast<int>(_hotkeys.size()); i < count; ++i)
    {
        _unregisterHotKey(i);
    }

    _hotkeys.clear();

    // Re-register all current hotkeys.
    for (const auto& [keyChord, cmd] : _app.Logic().GlobalHotkeys())
    {
        if (auto summonArgs = cmd.ActionAndArgs().Args().try_as<Settings::Model::GlobalSummonArgs>())
        {
            auto index = gsl::narrow_cast<int>(_hotkeys.size());
            const auto succeeded = _registerHotKey(index, keyChord);

            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_setupGlobalHotkey",
                              TraceLoggingDescription("Emitted when setting a single hotkey"),
                              TraceLoggingInt64(index, "index", "the index of the hotkey to add"),
                              TraceLoggingWideString(cmd.Name().c_str(), "name", "the name of the command"),
                              TraceLoggingBoolean(succeeded, "succeeded", "true if we succeeded"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));
            _hotkeys.emplace_back(summonArgs);
        }
    }
}

#pragma endregion

#pragma region NotificationIcon
// Method Description:
// - Creates a Notification Icon and hooks up its handlers
// Arguments:
// - <none>
// Return Value:
// - <none>
void WindowEmperor::_createNotificationIcon()
{
    _notificationIcon = std::make_unique<NotificationIcon>(_window.get());
    _notificationIcon->SummonWindowRequested([this](auto& args) { _manager.SummonWindow(args); });
}

// Method Description:
// - Deletes our notification icon if we have one.
// Arguments:
// - <none>
// Return Value:
// - <none>
void WindowEmperor::_destroyNotificationIcon()
{
    _notificationIcon->RemoveIconFromNotificationArea();
    _notificationIcon = nullptr;
}

void WindowEmperor::_checkWindowsForNotificationIcon()
{
    // We need to check some conditions to show the notification icon.
    //
    // * If there's a Quake window somewhere, we'll want to keep the
    //   notification icon.
    // * There's two settings - MinimizeToNotificationArea and
    //   AlwaysShowNotificationIcon. If either one of them are true, we want to
    //   make sure there's a notification icon.
    //
    // If both are false, we want to remove our icon from the notification area.
    // When we remove our icon from the notification area, we'll also want to
    // re-summon any hidden windows, but right now we're not keeping track of
    // who's hidden, so just summon them all. Tracking the work to do a "summon
    // all minimized" in GH#10448
    //
    // To avoid races between us thinking the settings updated, and the windows
    // themselves getting the new settings, only ask the app logic for the
    // RequestsTrayIcon setting value, and combine that with the result of each
    // window (which won't change during a settings reload).
    bool needsIcon = _app.Logic().RequestsTrayIcon();
    {
        auto windows{ _windows.lock_shared() };
        for (const auto& _windowThread : *windows)
        {
            needsIcon |= _windowThread->Logic().IsQuakeWindow();
        }
    }

    if (needsIcon)
    {
        _showNotificationIconRequested();
    }
    else
    {
        _hideNotificationIconRequested();
    }
}

void WindowEmperor::_showNotificationIconRequested()
{
    if (!_notificationIcon)
    {
        _createNotificationIcon();
    }
}

void WindowEmperor::_hideNotificationIconRequested()
{
    // Destroy it only if our settings allow it
    if (_notificationIcon)
    {
        // If we no longer want the tray icon, but we did have one, then quick
        // re-summon all our windows, so they don't get lost when the icon
        // disappears forever.
        _manager.SummonAllWindows();

        _destroyNotificationIcon();
    }
}
#pragma endregion

// A callback to the window's logic to let us know when the window's
// quake mode state changes. We'll use this to check if we need to add
// or remove the notification icon.
winrt::fire_and_forget WindowEmperor::_windowIsQuakeWindowChanged(winrt::Windows::Foundation::IInspectable sender,
                                                                  winrt::Windows::Foundation::IInspectable args)
{
    co_await wil::resume_foreground(this->_dispatcher);
    _checkWindowsForNotificationIcon();
}
winrt::fire_and_forget WindowEmperor::_windowRequestUpdateSettings()
{
    // We MUST be on the main thread to update the settings. We will crash when trying to enumerate fragment extensions otherwise.
    co_await wil::resume_foreground(this->_dispatcher);
    _app.Logic().ReloadSettings();
}
