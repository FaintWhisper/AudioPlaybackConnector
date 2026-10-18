#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(std::wstring, bool);
winrt::fire_and_forget DisconnectDevice(DeviceInformation);
IAsyncAction ConnectDevice(DeviceInformation, bool requestOpen = true);
void SetupDeviceFlyout();
winrt::fire_and_forget ShowDeviceFlyout(Point);
void UpdateConnectionUi();
void SetupSvgIcon();
void UpdateNotifyIcon();
void UpdateAudioBoost();
void ConfigureAudioBoost();
bool HasOpenConnection();
void OnConnectionStateChanged(const std::wstring&, AudioPlaybackConnection, AudioPlaybackConnectionState);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;
	const auto instanceHandle = CreateMutexW(nullptr, FALSE, L"Local\\AudioPlaybackConnector");
	const auto instanceError = GetLastError();
	wil::unique_handle instanceMutex(instanceHandle);
	FAIL_FAST_LAST_ERROR_IF_NULL(instanceMutex);
	if (instanceError == ERROR_ALREADY_EXISTS)
		return EXIT_SUCCESS;

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
	SetupDeviceFlyout();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	UpdateNotifyIcon();
	ConfigureAudioBoost();

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
			KillTimer(hWnd, AUDIO_BOOST_TIMER);
			try
			{
				g_playbackVolumeBoost.Restore();
			}
			CATCH_LOG();
			SaveSettings();
			auto connections = std::move(g_audioPlaybackConnections);
			for (const auto& connection : connections)
			{
				if (!connection.second.restarting)
					connection.second.connection.Close();
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
			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi)
			};
			ShowDeviceFlyout(point);
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
		if (!g_lastDevices.empty())
		{
			ConnectDevice(g_lastDevices.front(), g_reconnect);
		}
		break;
	case WM_CONNECTION_STATE_CHANGED:
		{
			std::unique_ptr<ConnectionStateChange> change(reinterpret_cast<ConnectionStateChange*>(lParam));
			OnConnectionStateChanged(change->deviceId, change->connection, change->state);
			UpdateAudioBoost();
		}
		break;
	case WM_TIMER:
		if (wParam == AUDIO_BOOST_TIMER)
			UpdateAudioBoost();
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
	MenuFlyoutItem connectionItem;
	g_connectionMenuItem = connectionItem;
	connectionItem.Text(_(L"Connect"));
	connectionItem.Click([](const auto&, const auto&) {
		if (g_lastDevices.empty())
			return;
		auto it = g_audioPlaybackConnections.find(g_lastDevices.front());
		if (it != g_audioPlaybackConnections.end() && it->second.connected)
			DisconnectDevice(it->second.device);
		else
			ConnectDevice(g_lastDevices.front(), true);
	});

	ToggleMenuFlyoutItem autoConnectItem;
	autoConnectItem.Text(_(L"Auto-connect on startup"));
	autoConnectItem.IsChecked(g_reconnect);
	autoConnectItem.Click([](const auto& sender, const auto&) {
		g_reconnect = sender.as<ToggleMenuFlyoutItem>().IsChecked();
		SaveSettings();
	});

	g_boostMenuItem = ToggleMenuFlyoutItem();
	g_boostMenuItem.Text(_(L"Boost PC volume +6 dB while phone plays"));
	g_boostMenuItem.IsChecked(g_boostWhilePlaying);
	g_boostMenuItem.Click([](const auto& sender, const auto&) {
		g_boostWhilePlaying = sender.as<ToggleMenuFlyoutItem>().IsChecked();
		ConfigureAudioBoost();
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
		if (!HasOpenConnection())
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
	menu.Items().Append(connectionItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(autoConnectItem);
	menu.Items().Append(g_boostMenuItem);
	menu.Items().Append(MenuFlyoutSeparator());
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		UpdateConnectionUi();
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

void OnConnectionStateChanged(const std::wstring& deviceId, AudioPlaybackConnection connection, AudioPlaybackConnectionState state)
{
	if (g_closing)
		return;
	try
	{
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it == g_audioPlaybackConnections.end() || it->second.connection != connection || it->second.restarting)
			return;
		if (connection.State() != state)
			return;
		if (state == AudioPlaybackConnectionState::Opened)
		{
			it->second.transportOpened = true;
			if (!it->second.opening && !it->second.connected)
				ConnectDevice(it->second.device, false);
		}
		else if (state == AudioPlaybackConnectionState::Closed)
		{
			it->second.connected = false;
			if (it->second.transportOpened)
				DisconnectDevice(it->second.device);
		}
		UpdateNotifyIcon();
	}
	CATCH_LOG();
}

winrt::fire_and_forget CloseConnection(AudioPlaybackConnection connection)
{
	co_await winrt::resume_background();
	try
	{
		connection.Close();
	}
	CATCH_LOG();
}

IAsyncAction ConnectDevice(DeviceInformation device, bool requestOpen)
{
	const std::wstring deviceId(device.Id());
	if (g_closing)
		co_return;
	auto existing = g_audioPlaybackConnections.find(deviceId);
	if (existing != g_audioPlaybackConnections.end() &&
		(existing->second.opening || existing->second.connected || existing->second.restarting))
		co_return;

	bool success = false;
	bool started = existing != g_audioPlaybackConnections.end();
	std::wstring errorMessage;
	AudioPlaybackConnection connection = nullptr;

	try
	{
		g_connectionError.clear();
		connection = started ? existing->second.connection : AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			if (!started)
			{
				g_audioPlaybackConnections.emplace(deviceId, PlaybackConnection{ device, connection, false, true });
				// Avoid AudioPlaybackConnection::DeviceId's borrowed HSTRING bug: microsoft/windows-rs#3845.
				connection.StateChanged([window = g_hWnd, deviceId](const auto& sender, const auto&) {
					try
					{
						std::unique_ptr<ConnectionStateChange> change(new ConnectionStateChange{ deviceId, sender, sender.State() });
						if (PostMessageW(window, WM_CONNECTION_STATE_CHANGED, 0, reinterpret_cast<LPARAM>(change.get())))
							change.release();
						else
							LOG_LAST_ERROR();
					}
					CATCH_LOG();
				});
				UpdateNotifyIcon();
				co_await connection.StartAsync();
				started = true;
			}
			else
			{
				existing->second.opening = true;
			}
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (g_closing || it == g_audioPlaybackConnections.end() || it->second.connection != connection || it->second.restarting)
				co_return;
			const bool incoming = connection.State() == AudioPlaybackConnectionState::Opened;
			if (!requestOpen && !incoming)
			{
				it->second.opening = false;
				UpdateNotifyIcon();
				co_return;
			}
			it->second.transportOpened = incoming;
			UpdateNotifyIcon();
			// Activate local playback once for an incoming link; never open a disconnected peer unless requested.
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
	if (connection && (it == g_audioPlaybackConnections.end() || it->second.connection != connection || it->second.restarting))
		co_return;
	if (it != g_audioPlaybackConnections.end())
		it->second.opening = false;

	if (success)
	{
		it->second.connected = connection.State() == AudioPlaybackConnectionState::Opened;
		it->second.transportOpened = it->second.connected;
		if (it->second.connected)
		{
			g_lastDevices = { deviceId };
			SaveSettings();
		}
	}
	else
	{
		if (!started && it != g_audioPlaybackConnections.end())
		{
			g_audioPlaybackConnections.erase(it);
			CloseConnection(connection);
		}
		g_connectionError = errorMessage;
	}
	UpdateNotifyIcon();
}

winrt::fire_and_forget ConnectDevice(std::wstring deviceId, bool requestOpen)
{
	if (g_closing || g_resolvingDevice)
		co_return;
	g_resolvingDevice = true;
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		if (!g_closing && device)
			co_await ConnectDevice(device, requestOpen);
	}
	catch (winrt::hresult_error const& ex)
	{
		if (!g_closing)
		{
			g_connectionError = ex.message();
			UpdateNotifyIcon();
		}
		LOG_CAUGHT_EXCEPTION();
	}
	g_resolvingDevice = false;
	if (!g_closing)
		UpdateNotifyIcon();
}

winrt::fire_and_forget DisconnectDevice(DeviceInformation device)
{
	const std::wstring deviceId(device.Id());
	auto it = g_audioPlaybackConnections.find(deviceId);
	if (g_closing || it == g_audioPlaybackConnections.end() || it->second.restarting)
		co_return;
	auto connection = it->second.connection;
	it->second.connected = false;
	it->second.opening = false;
	it->second.transportOpened = false;
	it->second.restarting = true;
	UpdateNotifyIcon();
	const winrt::apartment_context uiThread;
	std::wstring errorMessage;
	bool closed = false;
	co_await winrt::resume_background();
	try
	{
		connection.Close();
		closed = true;
	}
	catch (winrt::hresult_error const& ex)
	{
		errorMessage = ex.message();
		LOG_CAUGHT_EXCEPTION();
	}
	co_await uiThread;
	if (g_closing)
		co_return;
	it = g_audioPlaybackConnections.find(deviceId);
	if (it == g_audioPlaybackConnections.end() || it->second.connection != connection)
		co_return;
	if (!closed)
	{
		it->second.restarting = false;
		g_connectionError = errorMessage;
		UpdateNotifyIcon();
		co_return;
	}
	g_audioPlaybackConnections.erase(it);
	try
	{
		co_await ConnectDevice(device, false);
	}
	CATCH_LOG();
}

void SetupDeviceFlyout()
{
	g_deviceFlyout = MenuFlyout();
	g_deviceFlyout.ShouldConstrainToRootBounds(false);
	g_deviceFlyout.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});
}

winrt::fire_and_forget ShowDeviceFlyout(Point point)
{
	try
	{
		auto devices = co_await DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());
		if (g_closing)
			co_return;
		g_deviceMenuItems.clear();
		g_deviceFlyout.Items().Clear();
		for (const auto& device : devices)
		{
			MenuFlyoutItem item;
			item.Click([device](const auto&, const auto&) {
				auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
				if (it != g_audioPlaybackConnections.end() && it->second.connected)
					DisconnectDevice(device);
				else
					ConnectDevice(device, true);
			});
			g_deviceMenuItems.emplace(std::wstring(device.Id()), DeviceMenuEntry{ device, item });
			g_deviceFlyout.Items().Append(item);
		}
		if (devices.Size() == 0)
		{
			MenuFlyoutItem item;
			item.Text(_(L"No audio devices found"));
			item.IsEnabled(false);
			g_deviceFlyout.Items().Append(item);
		}
		UpdateConnectionUi();
		SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
		SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
		SetForegroundWindow(g_hWnd);
		g_deviceFlyout.ShowAt(g_xamlCanvas, point);
	}
	catch (winrt::hresult_error const& ex)
	{
		if (!g_closing)
		{
			g_connectionError = ex.message();
			UpdateNotifyIcon();
		}
		LOG_CAUGHT_EXCEPTION();
	}
}

void UpdateConnectionUi()
{
	for (const auto& entry : g_deviceMenuItems)
	{
		auto it = g_audioPlaybackConnections.find(entry.first);
		const PlaybackConnection* connection = it == g_audioPlaybackConnections.end() ? nullptr : &it->second;
		const auto status = connection && connection->restarting ? _(L"Disconnecting") :
			connection && connection->opening ? _(L"Connecting") :
			connection && connection->connected ? _(L"Connected") : _(L"Disconnected");
		entry.second.item.Text(std::wstring(entry.second.device.Name()) + L" (" + status + L")");
		entry.second.item.IsEnabled(!connection || (!connection->opening && !connection->restarting));
	}
	if (g_connectionMenuItem)
	{
		auto it = g_lastDevices.empty() ? g_audioPlaybackConnections.end() : g_audioPlaybackConnections.find(g_lastDevices.front());
		const PlaybackConnection* connection = it == g_audioPlaybackConnections.end() ? nullptr : &it->second;
		g_connectionMenuItem.Text(connection && connection->connected ? _(L"Disconnect") : _(L"Connect"));
		g_connectionMenuItem.IsEnabled(!g_lastDevices.empty() && !g_resolvingDevice &&
			(!connection || (!connection->opening && !connection->restarting)));
	}
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

bool HasOpenConnection()
{
	for (const auto& connection : g_audioPlaybackConnections)
	{
		if (connection.second.connected && !connection.second.opening && !connection.second.restarting)
			return true;
	}
	return false;
}

void UpdateAudioBoost()
{
	if (g_closing)
		return;
	const bool wasActive = g_playbackVolumeBoost.Active();
	try
	{
		if (!wasActive && !g_savedVolumeBoost.outputId.empty())
			g_playbackVolumeBoost.Recover();
		std::wstring sourceName;
		if (g_boostWhilePlaying && !g_lastDevices.empty())
		{
			auto it = g_audioPlaybackConnections.find(g_lastDevices.front());
			if (it != g_audioPlaybackConnections.end() && it->second.connected && !it->second.restarting)
				sourceName = it->second.device.Name();
		}
		g_playbackVolumeBoost.Update(sourceName);
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		g_boostWhilePlaying = false;
		g_boostMenuItem.IsChecked(false);
		KillTimer(g_hWnd, AUDIO_BOOST_TIMER);
		try
		{
			g_playbackVolumeBoost.Restore();
		}
		CATCH_LOG();
		g_connectionError = std::wstring(_(L"Automatic volume boost failed")) + L": " + ex.message().c_str();
		SaveSettings();
		UpdateNotifyIcon();
	}
	if (wasActive != g_playbackVolumeBoost.Active())
		UpdateNotifyIcon();
}

void ConfigureAudioBoost()
{
	if (g_boostWhilePlaying)
		FAIL_FAST_LAST_ERROR_IF(SetTimer(g_hWnd, AUDIO_BOOST_TIMER, 250, nullptr) == 0);
	else
		KillTimer(g_hWnd, AUDIO_BOOST_TIMER);
	UpdateAudioBoost();
}

void UpdateNotifyIcon()
{
	UpdateConnectionUi();
	const bool connected = HasOpenConnection();
	g_nid.hIcon = connected ? g_hIconConnected : g_hIconDisconnected;
	std::wstring tooltip = _(L"AudioPlaybackConnector");
	tooltip += L": ";
	tooltip += connected ? _(L"Connected") : _(L"Disconnected");
	if (g_playbackVolumeBoost.Active())
		tooltip += L" (+6 dB)";
	if (!g_connectionError.empty())
		tooltip += L": " + g_connectionError;
	wcsncpy_s(g_nid.szTip, tooltip.c_str(), _TRUNCATE);

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
