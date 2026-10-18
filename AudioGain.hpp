#pragma once

#include <algorithm>
#include <cmath>

class PlaybackVolumeBoost
{
	winrt::com_ptr<IMMDeviceEnumerator> m_enumerator;
	winrt::com_ptr<IMMDevice> m_device;
	winrt::com_ptr<IAudioEndpointVolume> m_volume;
	std::wstring m_outputId;
	ULONGLONG m_lastAudio = 0;
	float m_minimumDb = 0;
	float m_maximumDb = 0;
	bool m_boosted = false;

	void EnsureEnumerator()
	{
		if (!m_enumerator)
			winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), m_enumerator.put_void()));
	}

	bool HasPhoneAudio(const std::wstring& sourceName)
	{
		winrt::com_ptr<IAudioSessionManager2> manager;
		winrt::check_hresult(m_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void()));
		winrt::com_ptr<IAudioSessionEnumerator> sessions;
		winrt::check_hresult(manager->GetSessionEnumerator(sessions.put()));
		int count = 0;
		winrt::check_hresult(sessions->GetCount(&count));
		for (int index = 0; index < count; ++index)
		{
			winrt::com_ptr<IAudioSessionControl> session;
			winrt::check_hresult(sessions->GetSession(index, session.put()));
			AudioSessionState state;
			winrt::check_hresult(session->GetState(&state));
			if (state != AudioSessionStateActive)
				continue;
			wil::unique_cotaskmem_string name;
			winrt::check_hresult(session->GetDisplayName(name.put()));
			if (!name || !StrStrIW(name.get(), sourceName.c_str()))
				continue;
			auto volume = session.as<ISimpleAudioVolume>();
			BOOL muted = FALSE;
			float level = 0;
			winrt::check_hresult(volume->GetMute(&muted));
			winrt::check_hresult(volume->GetMasterVolume(&level));
			if (muted || level == 0)
				continue;
			auto meter = session.as<IAudioMeterInformation>();
			float peak = 0;
			winrt::check_hresult(meter->GetPeakValue(&peak));
			if (peak > 0.0001f)
				return true;
		}
		return false;
	}

	void TrackManualVolume()
	{
		float currentDb = 0;
		winrt::check_hresult(m_volume->GetMasterVolumeLevel(&currentDb));
		if (std::fabs(currentDb - g_savedVolumeBoost.appliedDb) > 0.1f)
		{
			g_savedVolumeBoost.baselineDb = std::clamp(g_savedVolumeBoost.baselineDb +
				currentDb - g_savedVolumeBoost.appliedDb, m_minimumDb, m_maximumDb);
			g_savedVolumeBoost.appliedDb = currentDb;
			SaveSettings();
		}
	}

public:
	bool Active() const { return m_boosted; }

	void Recover()
	{
		if (g_savedVolumeBoost.outputId.empty())
			return;
		EnsureEnumerator();
		winrt::com_ptr<IMMDevice> device;
		winrt::check_hresult(m_enumerator->GetDevice(g_savedVolumeBoost.outputId.c_str(), device.put()));
		winrt::com_ptr<IAudioEndpointVolume> volume;
		winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.put_void()));
		float minimumDb = 0, maximumDb = 0, incrementDb = 0, currentDb = 0;
		winrt::check_hresult(volume->GetVolumeRange(&minimumDb, &maximumDb, &incrementDb));
		winrt::check_hresult(volume->GetMasterVolumeLevel(&currentDb));
		if (std::isfinite(g_savedVolumeBoost.baselineDb) &&
			std::fabs(currentDb - g_savedVolumeBoost.appliedDb) < 0.1f)
			winrt::check_hresult(volume->SetMasterVolumeLevel(std::clamp(g_savedVolumeBoost.baselineDb, minimumDb, maximumDb), nullptr));
		g_savedVolumeBoost = {};
		SaveSettings();
	}

	void Restore()
	{
		if (!Active())
			return;
		TrackManualVolume();
		winrt::check_hresult(m_volume->SetMasterVolumeLevel(g_savedVolumeBoost.baselineDb, nullptr));
		m_boosted = false;
		g_savedVolumeBoost = {};
		SaveSettings();
	}

	void Update(const std::wstring& sourceName)
	{
		if (sourceName.empty())
		{
			Restore();
			m_lastAudio = 0;
			return;
		}
		EnsureEnumerator();
		winrt::com_ptr<IMMDevice> device;
		winrt::check_hresult(m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()));
		wil::unique_cotaskmem_string id;
		winrt::check_hresult(device->GetId(id.put()));
		if (m_outputId != id.get())
		{
			Restore();
			m_volume = nullptr;
			m_device = device;
			m_outputId = id.get();
			m_lastAudio = 0;
			winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, m_volume.put_void()));
			float incrementDb = 0;
			winrt::check_hresult(m_volume->GetVolumeRange(&m_minimumDb, &m_maximumDb, &incrementDb));
		}
		const auto now = GetTickCount64();
		if (HasPhoneAudio(sourceName))
			m_lastAudio = now;
		if (!m_lastAudio || now - m_lastAudio > 1000)
		{
			Restore();
			return;
		}
		if (Active())
		{
			TrackManualVolume();
			return;
		}
		float baselineDb = 0;
		winrt::check_hresult(m_volume->GetMasterVolumeLevel(&baselineDb));
		const float boostedDb = (std::min)(baselineDb + 6.0f, m_maximumDb);
		if (boostedDb - baselineDb < 0.1f)
			return;
		g_savedVolumeBoost = { m_outputId, baselineDb, boostedDb };
		SaveSettings();
		winrt::check_hresult(m_volume->SetMasterVolumeLevel(g_savedVolumeBoost.appliedDb, nullptr));
		m_boosted = true;
		winrt::check_hresult(m_volume->GetMasterVolumeLevel(&g_savedVolumeBoost.appliedDb));
		SaveSettings();
	}
};

PlaybackVolumeBoost g_playbackVolumeBoost;
