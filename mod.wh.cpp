// ==WindhawkMod==
// @id              audio-device-tray-switcher
// @name            Audio Device Switcher (Tray Icon)
// @description     Adds a system tray icon for one-click switching of the default playback and recording devices
// @version         1.1
// @author          Dinonugget10203
// @github          https://github.com/Dinonugget10203/windhawk-audio-io-taskbar-switcher
// @license MIT
// @include         explorer.exe
// @compilerOptions -lole32 -luser32 -lshell32 -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Device Switcher (Tray Icon)

**DISCLAIMER** This was created using Claude. It is not 100% ai, but about 20% ai

Adds a small speaker icon to the notification area. Click it (left- or
right-click) to open a lightweight popup menu listing every currently
**active** Playback (output) and Recording (input) device, grouped, with
the current default marked with a bullet. Pick a device to make it the new
default immediately, across all roles (Console, Multimedia, and
Communications).

Optional features (toggleable in the mod's settings):
- **Live device-change detection**: the tray tooltip stays in sync
  automatically when devices are plugged/unplugged or the default changes
  from elsewhere (Settings, another app).
- **Keyboard shortcuts**: `Ctrl+Alt+O` cycles to the next active output
  device, `Ctrl+Alt+I` cycles to the next active input device, with a
  balloon notification confirming the switch.

This relies on the same undocumented `IPolicyConfig` COM interface that
Windows' own Sound control panel and third-party utilities (EarTrumpet,
SoundSwitch, NirCmd, ...) use to change the default audio device, since
Microsoft has never shipped a public API for it. It has been stable since
Windows 7, but as with anything undocumented it could theoretically change
in a future Windows update.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- EnableLiveUpdates: true
  $name: Live device-change detection
  $description: >-
    Automatically refresh the tray tooltip when devices are plugged in,
    unplugged, or the default changes from elsewhere
- EnableHotkeys: true
  $name: Keyboard shortcuts
  $description: >-
    Ctrl+Alt+O cycles the default output device, Ctrl+Alt+I cycles the
    default input device
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Undocumented Core Audio interface used to change the default endpoint.
// Its vtable layout has been stable since Windows 7 and is the same
// interface used by many well-known open source audio-switching tools.
// We only ever call SetDefaultEndpoint, so the unused parameters of the
// other methods are simplified to opaque types - only the slot order and
// count need to match the real implementation.
// ----------------------------------------------------------------------
#undef INTERFACE
#define INTERFACE IPolicyConfig
DECLARE_INTERFACE_(IPolicyConfig, IUnknown)
{
    STDMETHOD(GetMixFormat)(THIS_ PCWSTR, void**) PURE;
    STDMETHOD(GetDeviceFormat)(THIS_ PCWSTR, INT, void**) PURE;
    STDMETHOD(ResetDeviceFormat)(THIS_ PCWSTR) PURE;
    STDMETHOD(SetDeviceFormat)(THIS_ PCWSTR, void*, void*) PURE;
    STDMETHOD(GetProcessingPeriod)(THIS_ PCWSTR, INT, void*, void*) PURE;
    STDMETHOD(SetProcessingPeriod)(THIS_ PCWSTR, void*) PURE;
    STDMETHOD(GetShareMode)(THIS_ PCWSTR, void*) PURE;
    STDMETHOD(SetShareMode)(THIS_ PCWSTR, void*) PURE;
    STDMETHOD(GetPropertyValue)(THIS_ PCWSTR, const PROPERTYKEY&, PROPVARIANT*) PURE;
    STDMETHOD(SetPropertyValue)(THIS_ PCWSTR, const PROPERTYKEY&, PROPVARIANT*) PURE;
    STDMETHOD(SetDefaultEndpoint)(THIS_ PCWSTR wszDeviceId, ERole eRole) PURE;
    STDMETHOD(SetEndpointVisibility)(THIS_ PCWSTR, INT) PURE;
};
#undef INTERFACE

// CLSID/IID pairs declared locally (with hardcoded, documented/verified
// values) so the mod needs no extra import libs beyond ole32/user32/shell32.

// MMDeviceEnumerator / IMMDeviceEnumerator (documented, standard values).
static const CLSID kClsidMMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIidMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

// CPolicyConfigClient / IPolicyConfig (undocumented, Windows 7+).
static const CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID kIidPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

// PKEY_Device_FriendlyName (documented, standard value).
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// IUnknown / IMMNotificationClient (documented, standard values) - used by
// our IMMNotificationClient::QueryInterface implementation below.
static const IID kIidUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID kIidMMNotificationClient = {
    0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};

// ----------------------------------------------------------------------
// Minimal RAII helper for COM interface pointers. Avoids depending on
// <wrl/client.h>, which isn't guaranteed to be available in every
// mingw-w64 toolchain configuration Windhawk might use.
// ----------------------------------------------------------------------
template <typename T>
struct ComPtr
{
    T* p = nullptr;

    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ~ComPtr() { Reset(); }

    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }

    void Reset()
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }
};

struct AudioDeviceInfo
{
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

namespace {

constexpr wchar_t kWindowClassName[] = L"WindhawkAudioTraySwitcherWnd";
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_APP_DEVICES_CHANGED = WM_APP + 2;
constexpr UINT kOutputDeviceCmdBase = 0x1000;
constexpr UINT kInputDeviceCmdBase = 0x2000;
constexpr UINT kMaxDevicesPerCategory = 0x1000;  // generous headroom
constexpr int kHotkeyIdCycleOutput = 1;
constexpr int kHotkeyIdCycleInput = 2;

HWND g_hWnd = nullptr;
HICON g_hTrayIcon = nullptr;
HANDLE g_hThread = nullptr;
HANDLE g_hInitEvent = nullptr;
UINT g_taskbarCreatedMsg = 0;

bool g_settingEnableLiveUpdates = true;
bool g_settingEnableHotkeys = true;

std::vector<AudioDeviceInfo> g_outputDevices;
std::vector<AudioDeviceInfo> g_inputDevices;

}  // namespace

// ----------------------------------------------------------------------
// Device enumeration
// ----------------------------------------------------------------------

bool EnumerateDevices(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                       std::vector<AudioDeviceInfo>& outList)
{
    outList.clear();

    // Find the id of the current default device (Console role) so we can
    // mark it in the menu.
    std::wstring defaultId;
    {
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice)) &&
            defaultDevice.p)
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&id)) && id)
            {
                defaultId = id;
                CoTaskMemFree(id);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) ||
        !collection.p)
    {
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device.p)
        {
            continue;
        }

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId)
        {
            continue;
        }
        std::wstring deviceId = rawId;
        CoTaskMemFree(rawId);

        std::wstring friendlyName = L"(Unknown device)";
        ComPtr<IPropertyStore> propStore;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &propStore)) && propStore.p)
        {
            PROPVARIANT pv{};  // zero-initialized => vt == VT_EMPTY
            if (SUCCEEDED(propStore->GetValue(kPkeyDeviceFriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal)
            {
                friendlyName = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }

        AudioDeviceInfo info;
        info.id = std::move(deviceId);
        info.name = std::move(friendlyName);
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        outList.push_back(std::move(info));
    }

    return true;
}

void RefreshDeviceLists()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   kIidMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr) || !enumerator.p)
    {
        Wh_Log(L"Failed to create MMDeviceEnumerator: 0x%08X", hr);
        g_outputDevices.clear();
        g_inputDevices.clear();
        return;
    }

    EnumerateDevices(enumerator, eRender, g_outputDevices);
    EnumerateDevices(enumerator, eCapture, g_inputDevices);
}

// ----------------------------------------------------------------------
// Changing the default device
// ----------------------------------------------------------------------

bool SetDefaultAudioDevice(const std::wstring& deviceId)
{
    ComPtr<IPolicyConfig> policyConfig;
    HRESULT hr = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL,
                                   kIidPolicyConfig, (void**)&policyConfig);
    if (FAILED(hr) || !policyConfig.p)
    {
        Wh_Log(L"Failed to create PolicyConfig client: 0x%08X", hr);
        return false;
    }

    // Set the device as default for all three roles, so it behaves as the
    // single, unambiguous default a user expects.
    const ERole roles[] = {eConsole, eMultimedia, eCommunications};
    bool anySucceeded = false;
    for (ERole role : roles)
    {
        HRESULT roleHr = policyConfig->SetDefaultEndpoint(deviceId.c_str(), role);
        if (SUCCEEDED(roleHr))
        {
            anySucceeded = true;
        }
        else
        {
            Wh_Log(L"SetDefaultEndpoint failed for role %d: 0x%08X", (int)role, roleHr);
        }
    }

    return anySucceeded;
}

// ----------------------------------------------------------------------
// Live device-change notifications (IMMNotificationClient)
// ----------------------------------------------------------------------

class AudioNotificationClient : public IMMNotificationClient
{
public:
    AudioNotificationClient() : m_refCount(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == kIidUnknown || riid == kIidMMNotificationClient)
        {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG newCount = InterlockedDecrement(&m_refCount);
        if (newCount == 0)
        {
            delete this;
        }
        return (ULONG)newCount;
    }

    // IMMNotificationClient - these fire on an internal MMDevice API
    // thread, so we just post a message and let our own worker thread
    // (which owns the window and the COM STA we use elsewhere) do the
    // actual work.
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        PostChangeNotification();
        return S_OK;
    }

    STDMETHODIMP OnDeviceAdded(LPCWSTR) override
    {
        PostChangeNotification();
        return S_OK;
    }

    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override
    {
        PostChangeNotification();
        return S_OK;
    }

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        PostChangeNotification();
        return S_OK;
    }

    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    void PostChangeNotification()
    {
        if (g_hWnd)
        {
            PostMessageW(g_hWnd, WM_APP_DEVICES_CHANGED, 0, 0);
        }
    }

    LONG m_refCount;
};

namespace {
IMMDeviceEnumerator* g_notifyEnumerator = nullptr;
AudioNotificationClient* g_notificationClient = nullptr;
}  // namespace

void RegisterForDeviceNotifications()
{
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   kIidMMDeviceEnumerator, (void**)&g_notifyEnumerator);
    if (FAILED(hr) || !g_notifyEnumerator)
    {
        Wh_Log(L"Live updates: failed to create enumerator: 0x%08X", hr);
        return;
    }

    g_notificationClient = new AudioNotificationClient();
    hr = g_notifyEnumerator->RegisterEndpointNotificationCallback(g_notificationClient);
    if (FAILED(hr))
    {
        Wh_Log(L"Live updates: RegisterEndpointNotificationCallback failed: 0x%08X", hr);
        g_notificationClient->Release();
        g_notificationClient = nullptr;
        g_notifyEnumerator->Release();
        g_notifyEnumerator = nullptr;
    }
}

void UnregisterForDeviceNotifications()
{
    if (g_notifyEnumerator && g_notificationClient)
    {
        g_notifyEnumerator->UnregisterEndpointNotificationCallback(g_notificationClient);
    }
    if (g_notificationClient)
    {
        g_notificationClient->Release();
        g_notificationClient = nullptr;
    }
    if (g_notifyEnumerator)
    {
        g_notifyEnumerator->Release();
        g_notifyEnumerator = nullptr;
    }
}

// ----------------------------------------------------------------------
// Tray icon
// ----------------------------------------------------------------------

HICON CreateAudioTrayIconHandle()
{
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0)
    {
        size = 16;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;  // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

    HICON result = nullptr;

    if (colorBmp && bits)
    {
        HGDIOBJ oldBmp = SelectObject(memDC, colorBmp);

        // Start fully transparent.
        ZeroMemory(bits, (size_t)size * size * 4);

        // Draw the "Volume" glyph (U+E767) from Segoe MDL2 Assets, which is
        // present on every Windows 10/11 install. This avoids depending on
        // SHGetStockIconInfo/SHSTOCKICONID, whose entries vary between SDK
        // header versions (and SIID_AUDIO in particular doesn't exist).
        HFONT font = CreateFontW(
            size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        HGDIOBJ oldFont = SelectObject(memDC, font);

        SetTextColor(memDC, RGB(255, 255, 255));
        SetBkMode(memDC, TRANSPARENT);

        RECT rc = {0, 0, size, size};
        DrawTextW(memDC, L"\uE767", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, oldFont);
        DeleteObject(font);

        // The glyph was drawn in solid white with grayscale antialiasing, so
        // any channel value doubles as the correct alpha for a premultiplied
        // white pixel.
        DWORD* pixels = (DWORD*)bits;
        bool anyPixelSet = false;
        for (int i = 0; i < size * size; i++)
        {
            BYTE a = (BYTE)(pixels[i] & 0xFF);
            if (a != 0)
            {
                anyPixelSet = true;
            }
            pixels[i] = ((DWORD)a << 24) | ((DWORD)a << 16) | ((DWORD)a << 8) | a;
        }

        if (anyPixelSet)
        {
            // The AND mask's content doesn't matter for a 32-bpp icon with a
            // real alpha channel, but CreateIconIndirect still requires a
            // validly-sized mask bitmap to be supplied.
            HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, nullptr);
            if (maskBmp)
            {
                ICONINFO iconInfo = {};
                iconInfo.fIcon = TRUE;
                iconInfo.hbmColor = colorBmp;
                iconInfo.hbmMask = maskBmp;
                result = CreateIconIndirect(&iconInfo);
                DeleteObject(maskBmp);
            }
        }

        SelectObject(memDC, oldBmp);
    }

    if (colorBmp)
    {
        DeleteObject(colorBmp);
    }
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!result)
    {
        Wh_Log(L"Failed to render Segoe MDL2 Assets volume glyph, falling back to default app icon");
        result = LoadIconW(nullptr, IDI_APPLICATION);
    }

    return result;
}

void AddTrayIcon(HWND hWnd)
{
    if (g_hTrayIcon)
    {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = nullptr;
    }
    g_hTrayIcon = CreateAudioTrayIconHandle();

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hTrayIcon;
    lstrcpynW(nid.szTip, L"Audio Device Switcher", ARRAYSIZE(nid.szTip));

    Shell_NotifyIconW(NIM_ADD, &nid);

    // Use the modern callback message layout (see TrayWndProc below).
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon(HWND hWnd)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdateTrayTooltip()
{
    if (!g_hWnd)
    {
        return;
    }

    RefreshDeviceLists();

    std::wstring outputName = L"(none)";
    for (const auto& d : g_outputDevices)
    {
        if (d.isDefault)
        {
            outputName = d.name;
            break;
        }
    }

    std::wstring inputName = L"(none)";
    for (const auto& d : g_inputDevices)
    {
        if (d.isDefault)
        {
            inputName = d.name;
            break;
        }
    }

    std::wstring tip = L"Out: " + outputName + L"\nIn: " + inputName;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowBalloonNotification(const std::wstring& title, const std::wstring& text)
{
    if (!g_hWnd)
    {
        return;
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ----------------------------------------------------------------------
// Hotkey-driven cycling
// ----------------------------------------------------------------------

void CycleDefaultDevice(EDataFlow flow)
{
    RefreshDeviceLists();
    std::vector<AudioDeviceInfo>& list = (flow == eRender) ? g_outputDevices : g_inputDevices;

    if (list.empty())
    {
        return;
    }

    size_t currentIndex = 0;
    bool foundDefault = false;
    for (size_t i = 0; i < list.size(); i++)
    {
        if (list[i].isDefault)
        {
            currentIndex = i;
            foundDefault = true;
            break;
        }
    }

    size_t nextIndex = foundDefault ? (currentIndex + 1) % list.size() : 0;
    const AudioDeviceInfo& next = list[nextIndex];

    Wh_Log(L"Hotkey: cycling %s device to: %s", (flow == eRender) ? L"output" : L"input",
           next.name.c_str());

    if (SetDefaultAudioDevice(next.id))
    {
        ShowBalloonNotification(
            (flow == eRender) ? L"Default output changed" : L"Default input changed",
            next.name);
        UpdateTrayTooltip();
    }
}

// ----------------------------------------------------------------------
// Popup menu
// ----------------------------------------------------------------------

void AppendHeaderItem(HMENU hMenu, const wchar_t* text)
{
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, text);
}

void AppendDeviceItem(HMENU hMenu, UINT cmdId, const AudioDeviceInfo& device)
{
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE;
    mii.fType = MFT_STRING | MFT_RADIOCHECK;
    mii.fState = device.isDefault ? MFS_CHECKED : MFS_UNCHECKED;
    mii.wID = cmdId;
    mii.dwTypeData = const_cast<LPWSTR>(device.name.c_str());
    mii.cch = (UINT)device.name.size();

    InsertMenuItemW(hMenu, GetMenuItemCount(hMenu), TRUE, &mii);
}

void HandleMenuCommand(UINT cmdId)
{
    bool switched = false;

    if (cmdId >= kOutputDeviceCmdBase && cmdId < kOutputDeviceCmdBase + kMaxDevicesPerCategory)
    {
        size_t index = cmdId - kOutputDeviceCmdBase;
        if (index < g_outputDevices.size())
        {
            Wh_Log(L"Switching default output device to: %s",
                   g_outputDevices[index].name.c_str());
            switched = SetDefaultAudioDevice(g_outputDevices[index].id);
        }
    }
    else if (cmdId >= kInputDeviceCmdBase && cmdId < kInputDeviceCmdBase + kMaxDevicesPerCategory)
    {
        size_t index = cmdId - kInputDeviceCmdBase;
        if (index < g_inputDevices.size())
        {
            Wh_Log(L"Switching default input device to: %s",
                   g_inputDevices[index].name.c_str());
            switched = SetDefaultAudioDevice(g_inputDevices[index].id);
        }
    }

    if (switched)
    {
        UpdateTrayTooltip();
    }
}

void ShowTrayMenu(HWND hWnd)
{
    RefreshDeviceLists();

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
    {
        return;
    }

    AppendHeaderItem(hMenu, L"Playback (Output) devices");
    if (g_outputDevices.empty())
    {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"    No active devices found");
    }
    else
    {
        for (size_t i = 0; i < g_outputDevices.size(); i++)
        {
            AppendDeviceItem(hMenu, kOutputDeviceCmdBase + (UINT)i, g_outputDevices[i]);
        }
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendHeaderItem(hMenu, L"Recording (Input) devices");
    if (g_inputDevices.empty())
    {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"    No active devices found");
    }
    else
    {
        for (size_t i = 0; i < g_inputDevices.size(); i++)
        {
            AppendDeviceItem(hMenu, kInputDeviceCmdBase + (UINT)i, g_inputDevices[i]);
        }
    }

    SetForegroundWindow(hWnd);

    POINT pt;
    GetCursorPos(&pt);

    UINT cmd = TrackPopupMenu(
        hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hWnd, nullptr);

    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);

    if (cmd != 0)
    {
        HandleMenuCommand(cmd);
    }
}

// ----------------------------------------------------------------------
// Window procedure / message loop
// ----------------------------------------------------------------------

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
    {
        UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_CONTEXTMENU)
        {
            ShowTrayMenu(hWnd);
        }
        return 0;
    }

    case WM_APP_DEVICES_CHANGED:
        UpdateTrayTooltip();
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyIdCycleOutput)
        {
            CycleDefaultDevice(eRender);
        }
        else if (wParam == kHotkeyIdCycleInput)
        {
            CycleDefaultDevice(eCapture);
        }
        return 0;

    case WM_COMMAND:
        HandleMenuCommand(LOWORD(wParam));
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        if (g_settingEnableHotkeys)
        {
            UnregisterHotKey(hWnd, kHotkeyIdCycleOutput);
            UnregisterHotKey(hWnd, kHotkeyIdCycleInput);
        }
        RemoveTrayIcon(hWnd);
        PostQuitMessage(0);
        return 0;

    default:
        if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg)
        {
            AddTrayIcon(hWnd);
            UpdateTrayTooltip();
            return 0;
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

DWORD WINAPI TrayThreadProc(LPVOID)
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = (hrCo == S_OK || hrCo == S_FALSE);
    if (!comInitialized)
    {
        Wh_Log(L"CoInitializeEx failed: 0x%08X", hrCo);
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Wh_Log(L"RegisterClassExW failed: %u", GetLastError());
        SetEvent(g_hInitEvent);
        if (comInitialized) CoUninitialize();
        return 0;
    }

    g_hWnd = CreateWindowExW(0, kWindowClassName, L"AudioDeviceSwitcherTrayHost", WS_POPUP, 0, 0,
                              0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_hWnd)
    {
        AddTrayIcon(g_hWnd);
        UpdateTrayTooltip();

        if (g_settingEnableLiveUpdates)
        {
            RegisterForDeviceNotifications();
        }

        if (g_settingEnableHotkeys)
        {
            if (!RegisterHotKey(g_hWnd, kHotkeyIdCycleOutput, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                 'O'))
            {
                Wh_Log(L"Failed to register output-cycle hotkey (Ctrl+Alt+O): %u",
                       GetLastError());
            }
            if (!RegisterHotKey(g_hWnd, kHotkeyIdCycleInput, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                                 'I'))
            {
                Wh_Log(L"Failed to register input-cycle hotkey (Ctrl+Alt+I): %u", GetLastError());
            }
        }
    }
    else
    {
        Wh_Log(L"CreateWindowExW failed: %u", GetLastError());
    }

    // Let Wh_ModInit know setup finished (successfully or not).
    SetEvent(g_hInitEvent);

    MSG msg;
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0)
    {
        if (ret == -1)
        {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterForDeviceNotifications();

    if (g_hTrayIcon)
    {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = nullptr;
    }

    UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));

    if (comInitialized)
    {
        CoUninitialize();
    }

    return 0;
}

// ----------------------------------------------------------------------
// Windhawk mod entry points
// ----------------------------------------------------------------------

BOOL Wh_ModInit()
{
    Wh_Log(L"Audio Device Switcher: initializing");

    g_settingEnableLiveUpdates = Wh_GetIntSetting(L"EnableLiveUpdates") != 0;
    g_settingEnableHotkeys = Wh_GetIntSetting(L"EnableHotkeys") != 0;

    g_hInitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hInitEvent)
    {
        return FALSE;
    }

    g_hThread = CreateThread(nullptr, 0, TrayThreadProc, nullptr, 0, nullptr);
    if (!g_hThread)
    {
        CloseHandle(g_hInitEvent);
        g_hInitEvent = nullptr;
        return FALSE;
    }

    WaitForSingleObject(g_hInitEvent, 5000);
    CloseHandle(g_hInitEvent);
    g_hInitEvent = nullptr;

    if (!g_hWnd)
    {
        Wh_Log(L"Audio Device Switcher: setup failed");
        if (g_hThread)
        {
            WaitForSingleObject(g_hThread, 2000);
            CloseHandle(g_hThread);
            g_hThread = nullptr;
        }
        return FALSE;
    }

    Wh_Log(L"Audio Device Switcher: initialized (live updates: %s, hotkeys: %s)",
           g_settingEnableLiveUpdates ? L"on" : L"off", g_settingEnableHotkeys ? L"on" : L"off");
    return TRUE;
}

void Wh_ModUninit()
{
    Wh_Log(L"Audio Device Switcher: uninitializing");

    if (g_hWnd)
    {
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    }

    if (g_hThread)
    {
        // Crucial: wait for the thread to fully exit before returning, since
        // Windhawk unloads this DLL right after Wh_ModUninit returns.
        WaitForSingleObject(g_hThread, 5000);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }

    g_hWnd = nullptr;
}

BOOL Wh_ModSettingsChanged(BOOL* bReload)
{
    Wh_Log(L"Audio Device Switcher: settings changed, reloading");
    *bReload = TRUE;
    return TRUE;
}
