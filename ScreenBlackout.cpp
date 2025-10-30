// DirectX 11 Borderless Fullscreen Blackout
// ------------------------------------------------------------
// This program provides both a GUI launcher and a DirectX 11 renderer.
// The launcher appears first, offering a dropdown of available monitors, and OK/Cancel buttons.
// When OK is clicked, it launches the renderer mode on the selected monitor.
// In renderer mode, a borderless fullscreen black window is created on the chosen monitor.
// Press Ctrl+Q to exit gracefully.

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <commctrl.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>

#include <string>
#include <vector>
#include <iostream>

#include "Resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "setupapi.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------- Shared Types ----------------------------
struct MonitorInfo
{
    HMONITOR hMon;
    MONITORINFOEXW info;
    std::wstring friendlyName;
};

std::wstring GetMonitorFriendlyName(const std::wstring& deviceName)
{
    std::wstring result = L"Unknown Monitor";

    HDEVINFO hDevInfo = SetupDiGetClassDevsExW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT, nullptr, nullptr, nullptr);
    if (hDevInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); ++i)
    {
        HKEY hKey = SetupDiOpenDevRegKey(hDevInfo, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hKey)
        {
            BYTE edid[256];
            DWORD edidSize = sizeof(edid);
            DWORD type = 0;

            if (RegQueryValueExW(hKey, L"EDID", nullptr, &type, edid, &edidSize) == ERROR_SUCCESS && type == REG_BINARY)
            {
                // The display model name is stored as an ASCII string inside the descriptor starting at byte 54 (in one of the descriptor blocks).
                for (int j = 54; j + 17 < (int) edidSize; j += 18)
                {
                    // Look for descriptor with tag 0xFC (monitor name).
                    if (edid[j] == 0x00 && edid[j + 1] == 0x00 && edid[j + 2] == 0x00 && edid[j + 3] == 0xFC)
                    {
                        char name[14] = {};
                        memcpy(name, &edid[j + 5], 13);
                        // Trim trailing spaces/newlines.
                        for (int k = 12; k >= 0; --k)
                        {
                            if (name[k] == ' ' || name[k] == '\n' || name[k] == '\r') name[k] = 0;
                            else break;
                        }
                        std::wstring wname;
                        int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
                        wname.resize(len);
                        MultiByteToWideChar(CP_UTF8, 0, name, -1, &wname[0], len);
                        result = wname;
                        break;
                    }
                }
            }
            RegCloseKey(hKey);
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return result;
}

static std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [] (HMONITOR hMon, HDC, LPRECT, LPARAM data) -> BOOL
        {
            auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(data);
            MonitorInfo mi{};
            mi.hMon = hMon;
            mi.info.cbSize = sizeof(mi.info);
            bool foundMonitor = GetMonitorInfoW(hMon, &mi.info);
            if (foundMonitor)
            {
                // Enhance with model name.
                mi.friendlyName = GetMonitorFriendlyName(mi.info.szDevice);
                vec->push_back(mi);
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out)
    );

    return out;
}

// ---------------------------- Renderer ----------------------------
HWND g_hWnd = nullptr;
ComPtr<ID3D11Device> g_Device;
ComPtr<ID3D11DeviceContext> g_Context;
ComPtr<IDXGISwapChain> g_Swapchain;
ComPtr<ID3D11RenderTargetView> g_Rtv;

LRESULT CALLBACK RendererProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'Q')
            {
                PostQuitMessage(0);
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

bool InitD3D(HWND hwnd, int width, int height)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL obtained;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, fl, 2,
        D3D11_SDK_VERSION, &sd, &g_Swapchain, &g_Device, &obtained, &g_Context
    );
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = g_Swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = g_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_Rtv);
    if (FAILED(hr)) return false;

    g_Context->OMSetRenderTargets(1, g_Rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{ 0, 0, (float) width, (float) height, 0.0f, 1.0f };
    g_Context->RSSetViewports(1, &vp);
    return true;
}

int RunRenderer(int monitorIndex)
{
    auto mons = EnumerateMonitors();
    if (mons.empty()) return - 1;
    if (monitorIndex < 0 || monitorIndex >= (int) mons.size()) monitorIndex = 0;

    RECT r = mons[monitorIndex].info.rcMonitor;
    int width = r.right - r.left;
    int height = r.bottom - r.top;

    LPCWSTR CLASS_NAME = L"ScreenBlackoutRenderer";
    WNDCLASS wc{};
    wc.lpfnWndProc = RendererProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(
        0, CLASS_NAME, L"Screen Blackout App", WS_POPUP,
        r.left, r.top, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!g_hWnd) return -1;

    ShowWindow(g_hWnd, SW_SHOW);
    SetForegroundWindow(g_hWnd);

    if (!InitD3D(g_hWnd, width, height)) return -1;

    MSG msg{};
    bool running = true;
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const float black[4] = { 0,0,0,1 };
        g_Context->ClearRenderTargetView(g_Rtv.Get(), black);
        g_Swapchain->Present(1, 0);
    }

    return 0;
}

// ---------------------------- GUI Launcher ----------------------------

INT_PTR CALLBACK LauncherDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static std::vector<MonitorInfo>* pMons = nullptr;
    switch (msg)
    {
        case WM_INITDIALOG:
        {
            pMons = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
            HWND combo = GetDlgItem(hDlg, IDC_MONITOR_COMBO);
            for (size_t i = 0; i < pMons->size(); ++i)
            {
                const auto& m = (*pMons)[i];
                std::wstring monitorOption = L"[" + std::to_wstring(i) + L"]: " + m.friendlyName;
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) monitorOption.data());
            }
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            return TRUE;
        }
        case WM_COMMAND:
            switch (wParam)
            {
                case IDOK:
                {
                    INT_PTR sel = SendDlgItemMessageW(hDlg, IDC_MONITOR_COMBO, CB_GETCURSEL, 0, 0);
                    EndDialog(hDlg, sel);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, -IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hDlg, -IDCANCEL);
            return TRUE;
    }

    return FALSE;
}

int RunDialogGui(HINSTANCE hInst)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    auto monitors = EnumerateMonitors();
    if (monitors.empty())
    {
        MessageBoxW(nullptr, L"No monitors detected.", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    // This function requires an .rc resource file to define the window layout
    // but it's still better than creating the dialog template struct in memory by hand.
    int sel = (int) DialogBoxParamW(
        hInst, MAKEINTRESOURCE(IDD_MONITOR_DIALOG), nullptr,
        LauncherDlgProc, (LPARAM)&monitors
    );
    // This represents user clicking Cancel / Exit button so we just exit with success.
    if (sel == -IDCANCEL)
    {
        return 0;
    }

    if (sel >= 0 && sel < (int)monitors.size())
    {
        return RunRenderer(sel);
    }
    else
    {
        MessageBoxW(nullptr, L"Failed to select monitor.", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}

constexpr std::wstring_view monitorArg = L"--monitor";
constexpr std::wstring_view helpArg = L"--help";

constexpr int RUN_GUI = -1;
constexpr int RUN_HELP = -2;

int CheckArgs(std::wstring cmd)
{
    if (cmd.find(monitorArg) != std::wstring::npos)
    {
        int monitorIndex = 0;
        size_t numStart = cmd.find_first_of(L"0123456789");
        if (numStart != std::wstring::npos) monitorIndex = _wtoi(cmd.c_str() + numStart);
        
        return monitorIndex;
    }
    else if (cmd.find(helpArg) != std::wstring::npos)
    {
        return RUN_HELP;
    }

    return RUN_GUI;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int)
{
    std::wstring cmd = cmdLine ? cmdLine : L"";
    
    int arg = CheckArgs(cmd);
    if (arg == RUN_HELP)
    {
        // FIXME: This still doesn't print properly on Windows because the terminal
        // releases the prompt before this app exits. I don't understand why, Windows sucks.
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
        {
            return -1;
        }

        // Do not reopen already valid handles (allow redirection to/from file).
        FILE* fout;
        if (_get_osfhandle(_fileno(stdout)) < 0) freopen_s(&fout, "CONOUT$", "w", stdout);

        std::wcout << L"\nUsage: " << monitorArg << L" <index>" << std::endl;
        return 0;
    }
    else if (arg == RUN_GUI)
    {
        return RunDialogGui(hInst);
    }
    else
    {
        return RunRenderer(arg);
    }
}

#endif // _WIN32
