#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring);
void StopStartupAutoConnect();
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		{
			g_closing = true;
			StopStartupAutoConnect();
			SaveSettings();
			auto connections = std::move(g_audioPlaybackConnections);
			for (const auto& connection : connections)
			{
				connection.second.second.Close();
				g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
			}
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		DestroyIcon(g_hIconConnected);
		DestroyIcon(g_hIconDisconnected);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect && !g_lastDevices.empty())
		{
			g_startupAutoConnect = true;
			g_startupConnectAttempts = 1;
			LOG_LAST_ERROR_IF(SetTimer(hWnd, STARTUP_CONNECT_TIMER, STARTUP_CONNECT_INTERVAL, nullptr) == 0);
			ConnectDevice(g_devicePicker, g_lastDevices.front());
		}
		break;
	case WM_TIMER:
		if (wParam == STARTUP_CONNECT_TIMER)
		{
			if (!g_reconnect || g_lastDevices.empty() ||
				(g_startupConnectAttempts >= STARTUP_CONNECT_ATTEMPTS && !g_startupConnecting))
			{
				StopStartupAutoConnect();
			}
			else if (!g_startupConnecting)
			{
				++g_startupConnectAttempts;
				ConnectDevice(g_devicePicker, g_lastDevices.front());
			}
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	ToggleMenuFlyoutItem autoConnectItem;
	autoConnectItem.Text(_(L"Auto-connect on startup"));
	autoConnectItem.IsChecked(g_reconnect);
	autoConnectItem.Click([](const auto& sender, const auto&) {
		g_reconnect = sender.as<ToggleMenuFlyoutItem>().IsChecked();
		if (!g_reconnect)
			StopStartupAutoConnect();
		SaveSettings();
	});

	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(autoConnectItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

void StopStartupAutoConnect()
{
	g_startupAutoConnect = false;
	KillTimer(g_hWnd, STARTUP_CONNECT_TIMER);
}

winrt::fire_and_forget OnConnectionStateChanged(AudioPlaybackConnection connection, winrt::apartment_context uiThread)
{
	co_await uiThread;
	if (g_closing)
		co_return;
	try
	{
		if (connection.State() == AudioPlaybackConnectionState::Closed)
		{
			auto it = g_audioPlaybackConnections.find(std::wstring(connection.DeviceId()));
			if (it != g_audioPlaybackConnections.end() && it->second.second == connection)
			{
				g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
				g_audioPlaybackConnections.erase(it);
			}
		}
		UpdateNotifyIcon();
	}
	CATCH_LOG();
}

IAsyncAction ConnectDevice(DevicePicker picker, DeviceInformation device, bool startup = false)
{
	const std::wstring deviceId(device.Id());
	if (g_closing || g_audioPlaybackConnections.find(deviceId) != g_audioPlaybackConnections.end())
		co_return;

	bool success = false;
	std::wstring errorMessage;
	AudioPlaybackConnection connection = nullptr;

	try
	{
		picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([uiThread = winrt::apartment_context()](const auto& sender, const auto&) {
				OnConnectionStateChanged(sender, uiThread);
			});

			co_await connection.StartAsync();
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (g_closing || it == g_audioPlaybackConnections.end() || it->second.second != connection)
				co_return;
			if (startup && !g_startupAutoConnect)
			{
				g_audioPlaybackConnections.erase(it);
				connection.Close();
				UpdateNotifyIcon();
				picker.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
				co_return;
			}
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (g_closing)
		co_return;

	auto it = g_audioPlaybackConnections.find(deviceId);
	if (connection && (it == g_audioPlaybackConnections.end() || it->second.second != connection))
		co_return;
	if (startup && !g_startupAutoConnect)
	{
		if (it != g_audioPlaybackConnections.end())
			g_audioPlaybackConnections.erase(it);
		if (connection)
			connection.Close();
		UpdateNotifyIcon();
		picker.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
		co_return;
	}

	if (success)
	{
		g_lastDevices = { deviceId };
		SaveSettings();
		StopStartupAutoConnect();
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		if (it != g_audioPlaybackConnections.end())
		{
			g_audioPlaybackConnections.erase(it);
			connection.Close();
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
	UpdateNotifyIcon();
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring deviceId)
{
	if (g_closing || !g_startupAutoConnect || g_startupConnecting)
		co_return;
	g_startupConnecting = true;
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		if (!g_closing && g_startupAutoConnect && g_reconnect && device)
			co_await ConnectDevice(picker, device, true);
	}
	CATCH_LOG();
	g_startupConnecting = false;
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		StopStartupAutoConnect();
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		StopStartupAutoConnect();
		auto device = args.Device();
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			auto connection = it->second.second;
			g_audioPlaybackConnections.erase(it);
			connection.Close();
		}
		UpdateNotifyIcon();
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconConnected = SvgTohIcon(svg, width, height, { 0.2f, 0.8f, 0.2f, 1 });
	g_hIconDisconnected = SvgTohIcon(svg, width, height, { 0.9f, 0.2f, 0.2f, 1 });
}

void UpdateNotifyIcon()
{
	bool connected = false;
	for (const auto& connection : g_audioPlaybackConnections)
	{
		if (connection.second.second.State() == AudioPlaybackConnectionState::Opened)
		{
			connected = true;
			break;
		}
	}
	g_nid.hIcon = connected ? g_hIconConnected : g_hIconDisconnected;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
