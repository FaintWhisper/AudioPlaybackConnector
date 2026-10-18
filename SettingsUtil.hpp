#pragma once

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr auto BUFFER_SIZE = 4096;

void DefaultSettings()
{
	g_reconnect = true;
	g_boostWhilePlaying = false;
	g_pausePcMedia = false;
	g_savedVolumeBoost = {};
	g_lastDevices.clear();
}

void LoadSettings()
{
	try
	{
		DefaultSettings();

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string string;
		while (1)
		{
			size_t size = string.size();
			string.resize(size + BUFFER_SIZE);
			DWORD read = 0;
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, BUFFER_SIZE, &read, nullptr));
			string.resize(size + read);
			if (read == 0)
				break;
		}

		std::wstring utf16 = Utf8ToUtf16(string);
		auto jsonObj = JsonObject::Parse(utf16);
		g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();
		g_boostWhilePlaying = jsonObj.GetNamedBoolean(L"boostWhilePlaying", false);
		g_pausePcMedia = jsonObj.GetNamedBoolean(L"pausePcMedia", false);
		if (jsonObj.HasKey(L"boostOutputId") && jsonObj.HasKey(L"boostBaselineDb") && jsonObj.HasKey(L"boostAppliedDb"))
		{
			g_savedVolumeBoost.outputId = jsonObj.GetNamedString(L"boostOutputId");
			g_savedVolumeBoost.baselineDb = static_cast<float>(jsonObj.GetNamedNumber(L"boostBaselineDb"));
			g_savedVolumeBoost.appliedDb = static_cast<float>(jsonObj.GetNamedNumber(L"boostAppliedDb"));
		}

		auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
		g_lastDevices.reserve(lastDevices.Size());
		for (const auto& i : lastDevices)
		{
			if (i.ValueType() == JsonValueType::String && !i.GetString().empty())
			{
				g_lastDevices.push_back(std::wstring(i.GetString()));
				break;
			}
		}
	}
	CATCH_LOG();
}

void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		jsonObj.Insert(L"boostWhilePlaying", JsonValue::CreateBooleanValue(g_boostWhilePlaying));
		jsonObj.Insert(L"pausePcMedia", JsonValue::CreateBooleanValue(g_pausePcMedia));
		if (!g_savedVolumeBoost.outputId.empty())
		{
			jsonObj.Insert(L"boostOutputId", JsonValue::CreateStringValue(g_savedVolumeBoost.outputId));
			jsonObj.Insert(L"boostBaselineDb", JsonValue::CreateNumberValue(g_savedVolumeBoost.baselineDb));
			jsonObj.Insert(L"boostAppliedDb", JsonValue::CreateNumberValue(g_savedVolumeBoost.appliedDb));
		}

		JsonArray lastDevices;
		for (const auto& deviceId : g_lastDevices)
		{
			lastDevices.Append(JsonValue::CreateStringValue(deviceId));
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
		THROW_HR_IF(E_FAIL, written != utf8.size());
	}
	CATCH_LOG();
}
