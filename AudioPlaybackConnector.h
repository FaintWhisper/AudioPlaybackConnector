#pragma once

#include "resource.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_CONNECTION_STATE_CHANGED = WM_APP + 3;
constexpr UINT_PTR AUDIO_BOOST_TIMER = 1;

struct ConnectionStateChange
{
	std::wstring deviceId;
	AudioPlaybackConnection connection;
	AudioPlaybackConnectionState state;
};

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
MenuFlyout g_deviceFlyout = nullptr;
MenuFlyoutItem g_connectionMenuItem = nullptr;
ToggleMenuFlyoutItem g_boostMenuItem = nullptr;
struct PlaybackConnection
{
	DeviceInformation device;
	AudioPlaybackConnection connection;
	bool connected = false;
	bool opening = false;
	bool transportOpened = false;
	bool restarting = false;
};
std::unordered_map<std::wstring, PlaybackConnection> g_audioPlaybackConnections;
struct DeviceMenuEntry
{
	DeviceInformation device;
	MenuFlyoutItem item;
};
std::unordered_map<std::wstring, DeviceMenuEntry> g_deviceMenuItems;
HICON g_hIconConnected = nullptr;
HICON g_hIconDisconnected = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = true;
bool g_boostWhilePlaying = false;
struct SavedVolumeBoost
{
	std::wstring outputId;
	float baselineDb = 0;
	float appliedDb = 0;
};
SavedVolumeBoost g_savedVolumeBoost;
std::vector<std::wstring> g_lastDevices;
bool g_resolvingDevice = false;
bool g_closing = false;
std::wstring g_connectionError;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "AudioGain.hpp"
#include "Direct2DSvg.hpp"
