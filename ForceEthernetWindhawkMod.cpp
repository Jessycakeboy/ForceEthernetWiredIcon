// ==WindhawkMod==
// @id              force-ethernet-tray-icon-fork
// @name            Force Ethernet Tray Icon - Fork
// @description     Always shows the wired (Ethernet) icon for the network icon in the Windows 11 taskbar tray, including classic taskbars such as StartAllBack
// @version         1.2.0
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lversion -lgdi32 -ladvapi32 -lshell32
// ==/WindhawkMod==

// Symbol lookup, IconView hook and XAML tree-walking plumbing is adapted from
// "Taskbar tray system icon tweaks" by m417z (GPL-3.0), which this mod is
// therefore also published under.

// ==WindhawkModReadme==
/*
# Force Ethernet Tray Icon

Makes the network icon in the Windows 11 system tray (the Wi-Fi / network /
volume group next to the clock) always show the wired Ethernet glyph, whatever
the real connection type is: Wi-Fi at any signal level, mobile data, etc.

Only Windows 11 is supported.

## How it works

The network icon is made of text glyphs (Segoe Fluent Icons) inside the taskbar's
XAML tree, usually as stacked layers (a dim background and the bright signal
level). The mod watches every text layer of the icon and swaps any Wi-Fi, signal
or mobile glyph for the Ethernet glyph. The glyph Windows asked for is
remembered and put back when the mod is disabled.

## Supported taskbars

- **Stock Windows 11 taskbar**: the glyph in the XAML tray is swapped for the
  Ethernet glyph.
- **Classic taskbar (e.g. StartAllBack)**: the classic network tray icon is
  intercepted when Windows updates it and replaced with an Ethernet icon drawn
  from the Segoe Fluent Icons font. The new icon appears the next time the
  network icon updates (connect/disconnect, toggle Wi-Fi) or after restarting
  Explorer. The same applies when disabling the mod: the original icon comes
  back on the next update.
  StartAllBack creates the network icon itself, so the mod recognizes it by the
  calling module (StartAllBackX64.dll) plus its GUID. If the wrong icon ever
  gets replaced, copy the right icon's GUID from the log into the GUID option.

## Options

- **Keep offline and airplane mode icons**: leaves the "no network" and
  airplane mode icons alone. Stock Windows 11 taskbar only.
- **Classic taskbar icon color**: the color of the Ethernet icon on a classic
  taskbar. "Match system theme" follows the Windows taskbar light/dark setting;
  pick white or black if your classic taskbar is themed differently.

## Troubleshooting

Open the mod's log (Windhawk > the mod > Log output) after toggling Wi-Fi.

- Lines beginning with "TextBlock" come from the stock taskbar path.
- Lines beginning with "Shell_NotifyIconW" come from the classic taskbar path
  and show which module is updating which tray icon, with its GUID. The icon is
  replaced when the caller is pnidui.dll, or StartAllBackX64.dll updating a
  GUID-identified icon (unless the GUID option is set).

If nothing changes, include that log in a bug report.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- keepOfflineIcons: false
  $name: Keep offline and airplane mode icons
  $description: >-
    Leave the "no network" and airplane mode icons untouched instead of
    replacing them with the Ethernet icon. Only applies to the stock Windows 11
    taskbar.
- networkIconGuid: ""
  $name: Classic taskbar network icon GUID (advanced)
  $description: >-
    Leave empty to detect the classic network icon automatically. If the wrong
    icon gets replaced, paste the GUID of the network icon from the mod log
    here, in the form {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}.
- iconColor: auto
  $name: Classic taskbar icon color
  $description: >-
    Color of the Ethernet icon on a classic taskbar (e.g. StartAllBack).
  $options:
  - auto: Match system theme
  - white: White
  - black: Black
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

using namespace winrt::Windows::UI::Xaml;

constexpr WCHAR kEthernetGlyph = L'\uE839';

enum class IconColor {
    automatic,
    white,
    black,
};

struct {
    bool keepOfflineIcons;
    IconColor iconColor;
    bool hasNetworkGuid;
    GUID networkGuid;
} g_settings;

std::atomic<bool> g_systemTrayModuleHooked;
std::atomic<bool> g_unloading;
bool g_taskbarDllHooked;

using FrameworkElementLoadedEventRevoker = winrt::impl::event_revoker<
    IFrameworkElement,
    &winrt::impl::abi<IFrameworkElement>::type::remove_Loaded>;

std::list<FrameworkElementLoadedEventRevoker> g_autoRevokerList;

// One entry per TextBlock inside the network text icon. The icon is made of
// several stacked layers (e.g. a dim background glyph and the bright signal
// glyph), so every TextBlock in it is handled the same way.
struct BlockState {
    winrt::weak_ref<Controls::TextBlock> textBlock;
    winrt::weak_ref<FrameworkElement> content;
    int64_t textChangedToken = 0;
    winrt::hstring realText;
    bool replaced = false;
    bool inSet = false;
};

std::vector<std::shared_ptr<BlockState>> g_blockStates;

////////////////////////////////////////////////////////////////////////////////
// Visual tree helpers

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

FrameworkElement EnumParentElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    auto parent = element;
    while (true) {
        parent = Media::VisualTreeHelper::GetParent(parent)
                     .try_as<FrameworkElement>();
        if (!parent) {
            return nullptr;
        }

        if (enumCallback(parent)) {
            return parent;
        }
    }
}

FrameworkElement GetParentElementByName(FrameworkElement element, PCWSTR name) {
    return EnumParentElements(element, [name](FrameworkElement parent) {
        return parent.Name() == name;
    });
}

bool IsChildOfElementByName(FrameworkElement element, PCWSTR name) {
    return !!GetParentElementByName(element, name);
}

FrameworkElement EnumChildElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            Wh_Log(L"Failed to get child %d of %d", i + 1, childrenCount);
            continue;
        }

        if (enumCallback(child)) {
            return child;
        }
    }

    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    return EnumChildElements(element, [className](FrameworkElement child) {
        return winrt::get_class_name(child) == className;
    });
}

////////////////////////////////////////////////////////////////////////////////
// Glyph classification

// Glyphs Windows uses for the network tray icon (Segoe Fluent Icons).
bool IsNetworkGlyph(WCHAR c) {
    switch (c) {
        case L'\uE709':  // Airplane
        case L'\uE7F4':  // TVMonitor
        case L'\uE839':  // Ethernet
        case L'\uE86C':  // SignalBars1
        case L'\uE86D':  // SignalBars2
        case L'\uE86E':  // SignalBars3
        case L'\uE86F':  // SignalBars4
        case L'\uE870':  // SignalBars5
        case L'\uEAA1':
        case L'\uEAA2':
        case L'\uEAA3':
        case L'\uEAA4':
        case L'\uEAA5':
        case L'\uEAA8':
        case L'\uEC1E':  // SignalRoaming
        case L'\uEC3C':  // MobWifi1
        case L'\uEC3D':  // MobWifi2
        case L'\uEC3E':  // MobWifi3
        case L'\uEC3F':  // MobWifi4
        case L'\uF384':  // NetworkOffline
        case L'\uF8C3':  // SysWifi4
        case L'\uF8C4':  // SysWifi3
        case L'\uF8C5':  // SysWifi2
        case L'\uF8C6':  // SysWifi1
        case L'\uF8C7':  // SysSignalBars5
        case L'\uF8C8':  // SysSignalBars4
        case L'\uF8C9':  // SysSignalBars3
        case L'\uF8CA':  // SysSignalBars2
        case L'\uF8CB':  // SysSignalBars1
        case L'\uF8CC':  // SysNetworkOffline
            return true;
    }

    return false;
}

bool IsOfflineGlyph(WCHAR c) {
    switch (c) {
        case L'\uE709':  // Airplane
        case L'\uF384':  // NetworkOffline
        case L'\uF8CC':  // SysNetworkOffline
            return true;
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////
// Glyph replacement logic

void CollectTextBlocks(FrameworkElement element,
                       std::vector<Controls::TextBlock>& out) {
    if (auto textBlock = element.try_as<Controls::TextBlock>()) {
        out.push_back(textBlock);
        return;
    }

    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (child) {
            CollectTextBlocks(child, out);
        }
    }
}

void RegisterTextBlocks(FrameworkElement content);

// Replaces the TextBlock's network glyph with the Ethernet glyph, remembering
// what Windows asked for so it can be restored when the mod is turned off.
void ApplyBlock(BlockState& state, bool rescan) {
    if (state.inSet) {
        return;
    }

    auto textBlock = state.textBlock.get();
    if (!textBlock) {
        return;
    }

    winrt::hstring text = textBlock.Text();
    std::wstring_view view = text;

    bool force = false;
    if (view.length() == 1) {
        WCHAR c = view[0];
        Wh_Log(L"TextBlock '%s' glyph %04X", textBlock.Name().c_str(),
               (unsigned)c);

        if (!g_unloading && IsNetworkGlyph(c)) {
            force = c != kEthernetGlyph &&
                    !(g_settings.keepOfflineIcons && IsOfflineGlyph(c));
        }
    }

    if (force) {
        state.realText = text;
        state.replaced = true;

        state.inSet = true;
        textBlock.Text(winrt::hstring(std::wstring_view(&kEthernetGlyph, 1)));
        state.inSet = false;
    } else {
        state.replaced = false;
    }

    if (rescan) {
        // Layers can be created after the icon first loads.
        if (auto content = state.content.get()) {
            RegisterTextBlocks(content);
        }
    }
}

void RegisterTextBlocks(FrameworkElement content) {
    std::vector<Controls::TextBlock> blocks;
    CollectTextBlocks(content, blocks);

    for (auto& textBlock : blocks) {
        bool known = false;
        for (auto& existing : g_blockStates) {
            if (existing->textBlock.get() == textBlock) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }

        auto state = std::make_shared<BlockState>();
        state->textBlock = textBlock;
        state->content = content;

        Wh_Log(L"Registering TextBlock '%s'", textBlock.Name().c_str());

        state->textChangedToken = textBlock.RegisterPropertyChangedCallback(
            Controls::TextBlock::TextProperty(),
            [state](DependencyObject sender, DependencyProperty property) {
                ApplyBlock(*state, true);
            });
        g_blockStates.push_back(state);

        ApplyBlock(*state, false);
    }
}

void CleanupAllStates() {
    for (auto& state : g_blockStates) {
        if (auto textBlock = state->textBlock.get()) {
            textBlock.UnregisterPropertyChangedCallback(
                Controls::TextBlock::TextProperty(), state->textChangedToken);

            if (state->replaced) {
                textBlock.Text(state->realText);
            }
        }
    }

    g_blockStates.clear();
}

enum class ProcessResult {
    kDone,
    kNotReady,
    kNotApplicable,
};

// Hooks up one icon of the Control Center button (SystemTrayIcon IconView).
ProcessResult ProcessControlCenterIcon(FrameworkElement systemTrayIconElement) {
    if (g_unloading) {
        return ProcessResult::kNotApplicable;
    }

    FrameworkElement containerGrid =
        FindChildByName(systemTrayIconElement, L"ContainerGrid");
    if (!containerGrid) {
        return ProcessResult::kNotReady;
    }

    FrameworkElement contentGrid = FindChildByName(containerGrid, L"ContentGrid");
    if (!contentGrid) {
        return ProcessResult::kNotReady;
    }

    if (FindChildByClassName(contentGrid, L"SystemTray.BatteryIconContent")) {
        return ProcessResult::kNotApplicable;
    }

    FrameworkElement textIconContent =
        FindChildByClassName(contentGrid, L"SystemTray.TextIconContent");
    if (!textIconContent) {
        return ProcessResult::kNotReady;
    }

    std::vector<Controls::TextBlock> blocks;
    CollectTextBlocks(textIconContent, blocks);
    if (blocks.empty()) {
        return ProcessResult::kNotReady;
    }

    RegisterTextBlocks(textIconContent);

    return ProcessResult::kDone;
}

// Content can be populated a moment after the icon loads, so retry a few times.
void ProcessControlCenterIconWithRetry(FrameworkElement systemTrayIconElement,
                                       int attempt) {
    if (ProcessControlCenterIcon(systemTrayIconElement) !=
        ProcessResult::kNotReady) {
        return;
    }

    if (attempt >= 10) {
        Wh_Log(L"Giving up on icon, content never became ready");
        return;
    }

    systemTrayIconElement.Dispatcher().TryRunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
        [systemTrayIconElement, attempt]() {
            ProcessControlCenterIconWithRetry(systemTrayIconElement,
                                              attempt + 1);
        });
}

bool ApplyControlCenterButtonStyle(FrameworkElement controlCenterButton) {
    FrameworkElement stackPanel = nullptr;

    FrameworkElement child = controlCenterButton;
    if ((child =
             FindChildByClassName(child, L"Windows.UI.Xaml.Controls.Grid")) &&
        (child = FindChildByName(child, L"ContentPresenter")) &&
        (child = FindChildByClassName(
             child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (child = FindChildByClassName(
             child, L"Windows.UI.Xaml.Controls.StackPanel"))) {
        stackPanel = child;
    }

    if (!stackPanel) {
        return false;
    }

    EnumChildElements(stackPanel, [](FrameworkElement child) {
        auto childClassName = winrt::get_class_name(child);
        if (childClassName != L"Windows.UI.Xaml.Controls.ContentPresenter") {
            Wh_Log(L"Unsupported class name %s of child",
                   childClassName.c_str());
            return false;
        }

        FrameworkElement systemTrayIconElement =
            FindChildByName(child, L"SystemTrayIcon");
        if (!systemTrayIconElement) {
            Wh_Log(L"Failed to get SystemTrayIcon of child");
            return false;
        }

        ProcessControlCenterIconWithRetry(systemTrayIconElement, 0);
        return false;
    });

    return true;
}

bool ApplyStyle(XamlRoot xamlRoot) {
    FrameworkElement systemTrayFrameGrid = nullptr;

    FrameworkElement child = xamlRoot.Content().try_as<FrameworkElement>();
    if (child &&
        (child = FindChildByClassName(child, L"SystemTray.SystemTrayFrame")) &&
        (child = FindChildByName(child, L"SystemTrayFrameGrid"))) {
        systemTrayFrameGrid = child;
    }

    if (!systemTrayFrameGrid) {
        return false;
    }

    FrameworkElement controlCenterButton =
        FindChildByName(systemTrayFrameGrid, L"ControlCenterButton");
    if (!controlCenterButton) {
        return false;
    }

    return ApplyControlCenterButtonStyle(controlCenterButton);
}

////////////////////////////////////////////////////////////////////////////////
// Hooks: catch icons created after the mod loads

using IconView_IconView_t = void*(WINAPI*)(void* pThis);
IconView_IconView_t IconView_IconView_Original;
void* WINAPI IconView_IconView_Hook(void* pThis) {
    Wh_Log(L">");

    void* ret = IconView_IconView_Original(pThis);

    FrameworkElement iconView = nullptr;
    ((IUnknown**)pThis)[1]->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(iconView));
    if (!iconView) {
        return ret;
    }

    g_autoRevokerList.emplace_back();
    auto autoRevokerIt = g_autoRevokerList.end();
    --autoRevokerIt;

    *autoRevokerIt = iconView.Loaded(
        winrt::auto_revoke_t{},
        [autoRevokerIt](winrt::Windows::Foundation::IInspectable const& sender,
                        RoutedEventArgs const& e) {
            Wh_Log(L">");

            g_autoRevokerList.erase(autoRevokerIt);

            if (g_unloading) {
                return;
            }

            auto iconView = sender.try_as<FrameworkElement>();
            if (!iconView) {
                return;
            }

            auto className = winrt::get_class_name(iconView);
            Wh_Log(L"className: %s", className.c_str());

            if (className == L"SystemTray.IconView" &&
                iconView.Name() == L"SystemTrayIcon" &&
                IsChildOfElementByName(iconView, L"ControlCenterButton")) {
                ProcessControlCenterIconWithRetry(iconView, 0);
            }
        });

    return ret;
}

void* CTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    if (!CTaskBand_ITaskListWndSite_vftable ||
        !CTaskBand_GetTaskbarHost_Original || !TaskbarHost_FrameHeight_Original ||
        !std__Ref_count_base__Decref_Original) {
        return nullptr;
    }

    HWND hTaskSwWnd = (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }

        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                      taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t taskbarElementIUnknownOffset = 0x48;

#if defined(_M_X64)
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }
#elif defined(_M_ARM64)
    // Just use the default offset which will hopefully work in most cases.
#else
#error "Unsupported architecture"
#endif

    auto* taskbarElementIUnknown =
        *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] +
                      taskbarElementIUnknownOffset);

    FrameworkElement taskbarElement = nullptr;
    taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(taskbarElement));

    auto result = taskbarElement ? taskbarElement.XamlRoot() : nullptr;

    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

void LoadSettings() {
    g_settings.keepOfflineIcons = Wh_GetIntSetting(L"keepOfflineIcons");

    PCWSTR iconColor = Wh_GetStringSetting(L"iconColor");
    g_settings.iconColor = IconColor::automatic;
    if (wcscmp(iconColor, L"white") == 0) {
        g_settings.iconColor = IconColor::white;
    } else if (wcscmp(iconColor, L"black") == 0) {
        g_settings.iconColor = IconColor::black;
    }
    Wh_FreeStringSetting(iconColor);

    PCWSTR networkGuid = Wh_GetStringSetting(L"networkIconGuid");
    g_settings.hasNetworkGuid =
        networkGuid[0] &&
        CLSIDFromString(networkGuid, &g_settings.networkGuid) == S_OK;
    Wh_FreeStringSetting(networkGuid);
}

void ApplySettings() {
    struct ApplySettingsParam {
        HWND hTaskbarWnd;
    };

    Wh_Log(L"Applying settings");

    if (!g_taskbarDllHooked) {
        Wh_Log(L"XAML taskbar path unavailable");
        return;
    }

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        Wh_Log(L"No taskbar found");
        return;
    }

    ApplySettingsParam param{
        .hTaskbarWnd = hTaskbarWnd,
    };

    RunFromWindowThread(
        hTaskbarWnd,
        [](void* pParam) {
            ApplySettingsParam& param = *(ApplySettingsParam*)pParam;

            g_autoRevokerList.clear();

            // Restore everything first; on a settings change the icons are
            // then re-processed below with the new settings.
            CleanupAllStates();

            if (g_unloading) {
                return;
            }

            auto xamlRoot = GetTaskbarXamlRoot(param.hTaskbarWnd);
            if (!xamlRoot) {
                Wh_Log(L"Getting XamlRoot failed");
                return;
            }

            if (!ApplyStyle(xamlRoot)) {
                Wh_Log(L"ApplyStyle failed");
            }
        },
        &param);
}

bool HookSystemTraySymbols(HMODULE module) {
    // SystemTray.dll, Taskbar.View.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))"},
            &IconView_IconView_Original,
            IconView_IconView_Hook,
        },
    };

    if (!HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;

    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);
        if (hGlobal) {
            void* pData = LockResource(hGlobal);
            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

// Returns the module that hosts winrt::SystemTray::* in the current build.
// SystemTray.dll is the newer home; Taskbar.View.dll is used on older builds.
HMODULE GetSystemTrayModuleHandle() {
    HMODULE module = GetModuleHandle(L"SystemTray.dll");
    if (!module) {
        module = GetModuleHandle(L"Taskbar.View.dll");
        if (module) {
            // First known module version without SystemTray is Taskbar.View.dll
            // 2604.8002.200.6000.
            VS_FIXEDFILEINFO* fixedFileInfo =
                GetModuleVersionInfo(module, nullptr);
            WORD moduleMajor =
                fixedFileInfo ? HIWORD(fixedFileInfo->dwFileVersionMS) : 0;
            if (!moduleMajor || moduleMajor >= 2604) {
                Wh_Log(L"Skipping Taskbar.View.dll version %d", moduleMajor);
                module = nullptr;
            }
        }
    }

    return module;
}

void HandleLoadedModuleIfSystemTray(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_systemTrayModuleHooked && GetSystemTrayModuleHandle() == module &&
        !g_systemTrayModuleHooked.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookSystemTraySymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfSystemTray(module, lpLibFileName);
    }

    return module;
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
    };

    return HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks));
}

////////////////////////////////////////////////////////////////////////////////
// Classic (Win32) tray icons, e.g. StartAllBack's classic taskbar.
//
// The classic network icon is created by pnidui.dll inside explorer.exe, which
// hands its icon to the tray via Shell_NotifyIconW. The icon is swapped there.

struct CachedIcon {
    int size;
    COLORREF color;
    HICON icon;
};

std::mutex g_iconCacheMutex;
std::vector<CachedIcon> g_iconCache;

COLORREF GetClassicIconColor() {
    switch (g_settings.iconColor) {
        case IconColor::white:
            return RGB(255, 255, 255);

        case IconColor::black:
            return RGB(0, 0, 0);

        default:
            break;
    }

    DWORD light = 0;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light,
                     &size) != ERROR_SUCCESS) {
        light = 0;
    }

    return light ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

HFONT CreateGlyphFont(HDC hdc, int height) {
    PCWSTR faces[] = {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"};
    for (PCWSTR face : faces) {
        HFONT font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, face);
        if (!font) {
            continue;
        }

        HGDIOBJ oldFont = SelectObject(hdc, font);
        WCHAR actual[LF_FACESIZE]{};
        GetTextFaceW(hdc, ARRAYSIZE(actual), actual);
        SelectObject(hdc, oldFont);

        if (_wcsicmp(actual, face) == 0) {
            return font;
        }

        DeleteObject(font);
    }

    return nullptr;
}

// Renders the Ethernet glyph into a 32-bit icon of the given size and color.
HICON CreateEthernetIcon(int size, COLORREF color) {
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP colorBitmap =
        CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!colorBitmap || !bits) {
        DeleteDC(hdc);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(hdc, colorBitmap);
    memset(bits, 0, (size_t)size * size * 4);

    HICON icon = nullptr;

    HFONT font = CreateGlyphFont(hdc, size);
    if (font) {
        HGDIOBJ oldFont = SelectObject(hdc, font);

        // White text on black: the coverage ends up in every channel.
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        const WCHAR glyph[] = {kEthernetGlyph, 0};
        RECT rect = {0, 0, size, size};
        DrawTextW(hdc, glyph, 1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();

        SelectObject(hdc, oldFont);
        DeleteObject(font);

        // Turn the coverage into premultiplied alpha in the requested color.
        uint32_t* pixels = (uint32_t*)bits;
        uint32_t r = GetRValue(color);
        uint32_t g = GetGValue(color);
        uint32_t b = GetBValue(color);
        for (int i = 0; i < size * size; i++) {
            uint32_t a = (pixels[i] >> 8) & 0xFF;
            pixels[i] = (a << 24) | ((r * a / 255) << 16) |
                        ((g * a / 255) << 8) | (b * a / 255);
        }

        HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
        if (maskBitmap) {
            ICONINFO iconInfo{};
            iconInfo.fIcon = TRUE;
            iconInfo.hbmMask = maskBitmap;
            iconInfo.hbmColor = colorBitmap;
            icon = CreateIconIndirect(&iconInfo);
            DeleteObject(maskBitmap);
        }
    } else {
        Wh_Log(L"No icon font found");
    }

    SelectObject(hdc, oldBitmap);
    DeleteObject(colorBitmap);
    DeleteDC(hdc);

    return icon;
}

HICON GetEthernetIcon() {
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size <= 0) {
        size = 16;
    }

    COLORREF color = GetClassicIconColor();

    std::lock_guard<std::mutex> lock(g_iconCacheMutex);

    for (auto& cached : g_iconCache) {
        if (cached.size == size && cached.color == color) {
            return cached.icon;
        }
    }

    HICON icon = CreateEthernetIcon(size, color);
    if (icon) {
        g_iconCache.push_back({size, color, icon});
    }

    return icon;
}

void DestroyCachedIcons() {
    std::lock_guard<std::mutex> lock(g_iconCacheMutex);

    for (auto& cached : g_iconCache) {
        DestroyIcon(cached.icon);
    }

    g_iconCache.clear();
}

// Writes the file name of the module containing the address (empty if none).
void GetModuleNameFromAddress(const void* address, WCHAR* name, size_t count) {
    name[0] = L'\0';

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module)) {
        return;
    }

    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        return;
    }

    PCWSTR fileName = wcsrchr(path, L'\\');
    fileName = fileName ? fileName + 1 : path;
    wcsncpy(name, fileName, count - 1);
    name[count - 1] = L'\0';
}

using Shell_NotifyIconW_t = BOOL(WINAPI*)(DWORD dwMessage,
                                          PNOTIFYICONDATAW lpData);
Shell_NotifyIconW_t Shell_NotifyIconW_Original;
BOOL WINAPI Shell_NotifyIconW_Hook(DWORD dwMessage, PNOTIFYICONDATAW pData) {
    constexpr DWORD kMinSize = offsetof(NOTIFYICONDATAW, szTip);

    if (pData && pData->cbSize >= kMinSize &&
        pData->cbSize <= sizeof(NOTIFYICONDATAW)) {
        WCHAR callerName[64];
        GetModuleNameFromAddress(__builtin_return_address(0), callerName,
                                 ARRAYSIZE(callerName));

        bool hasGuid = (pData->uFlags & NIF_GUID) &&
                       pData->cbSize >= offsetof(NOTIFYICONDATAW, guidItem) +
                                            sizeof(GUID);
        GUID guid = hasGuid ? pData->guidItem : GUID{};

        Wh_Log(L"Shell_NotifyIconW msg=%lu flags=0x%lX uID=%u caller=%s "
               L"guid=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               (unsigned long)dwMessage, (unsigned long)pData->uFlags,
               (unsigned)pData->uID, callerName, (unsigned long)guid.Data1,
               (unsigned)guid.Data2, (unsigned)guid.Data3,
               (unsigned)guid.Data4[0], (unsigned)guid.Data4[1],
               (unsigned)guid.Data4[2], (unsigned)guid.Data4[3],
               (unsigned)guid.Data4[4], (unsigned)guid.Data4[5],
               (unsigned)guid.Data4[6], (unsigned)guid.Data4[7]);

        // Which tray icon is the network icon:
        // - an explicit GUID from the settings, if given;
        // - otherwise anything coming from pnidui.dll (the classic network
        //   icon), or the GUID-identified icon StartAllBack registers itself.
        bool isNetworkIcon;
        if (g_settings.hasNetworkGuid) {
            isNetworkIcon =
                hasGuid && IsEqualGUID(guid, g_settings.networkGuid);
        } else {
            isNetworkIcon =
                _wcsicmp(callerName, L"pnidui.dll") == 0 ||
                (_wcsicmp(callerName, L"StartAllBackX64.dll") == 0 &&
                 pData->uID == 0 && hasGuid);
        }

        if (!g_unloading && isNetworkIcon &&
            (dwMessage == NIM_ADD || dwMessage == NIM_MODIFY) &&
            (pData->uFlags & NIF_ICON)) {
            if (HICON icon = GetEthernetIcon()) {
                NOTIFYICONDATAW copy{};
                memcpy(&copy, pData, pData->cbSize);
                copy.hIcon = icon;
                return Shell_NotifyIconW_Original(dwMessage, &copy);
            }
        }
    }

    return Shell_NotifyIconW_Original(dwMessage, pData);
}

bool HookClassicTray() {
    HMODULE shell32 =
        LoadLibraryEx(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!shell32) {
        Wh_Log(L"Failed to load shell32.dll");
        return false;
    }

    auto pShell_NotifyIconW =
        (Shell_NotifyIconW_t)GetProcAddress(shell32, "Shell_NotifyIconW");
    if (!pShell_NotifyIconW) {
        Wh_Log(L"Shell_NotifyIconW not found");
        return false;
    }

    return WindhawkUtils::SetFunctionHook(pShell_NotifyIconW,
                                          Shell_NotifyIconW_Hook,
                                          &Shell_NotifyIconW_Original);
}

// Diagnostics: logs the modules in explorer.exe that relate to the taskbar and
// tray, to show what a given taskbar setup actually uses.
void LogRelevantModules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Module snapshot failed");
        return;
    }

    static const PCWSTR kNeedles[] = {
        L"startallback", L"startisback", L"pnidui",  L"sndvol",
        L"stobject",     L"batmeter",    L"taskbar", L"systemtray",
        L"notification",
    };

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok;
         ok = Module32NextW(snapshot, &entry)) {
        std::wstring name = entry.szModule;
        for (auto& c : name) {
            c = (WCHAR)towlower(c);
        }

        for (PCWSTR needle : kNeedles) {
            if (name.find(needle) != std::wstring::npos) {
                Wh_Log(L"Module: %s (%s)", entry.szModule, entry.szExePath);
                break;
            }
        }
    }

    CloseHandle(snapshot);
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    // Classic taskbars (e.g. StartAllBack) use Win32 tray icons.
    bool classicHooked = HookClassicTray();

    // The stock Windows 11 taskbar uses a XAML tray. Failing to hook it is not
    // fatal: with a classic taskbar some or all of these symbols can be absent.
    if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
        g_systemTrayModuleHooked = true;
        HookSystemTraySymbols(systemTrayModule);
    } else {
        Wh_Log(L"System tray module not loaded yet");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    g_taskbarDllHooked = HookTaskbarDllSymbols();
    if (!g_taskbarDllHooked) {
        Wh_Log(L"XAML taskbar hooks unavailable (classic taskbar?)");
    }

    if (!classicHooked && !g_taskbarDllHooked) {
        Wh_Log(L"Nothing could be hooked");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    LogRelevantModules();

    if (!g_systemTrayModuleHooked) {
        if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
            if (!g_systemTrayModuleHooked.exchange(true)) {
                Wh_Log(L"Got system tray module");

                if (HookSystemTraySymbols(systemTrayModule)) {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }

    ApplySettings();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    g_unloading = true;

    ApplySettings();
}

void Wh_ModUninit() {
    Wh_Log(L">");

    DestroyCachedIcons();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    ApplySettings();
}

