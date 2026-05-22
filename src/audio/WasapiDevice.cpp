#include "entity/audio/WasapiDevice.hpp"
#include "entity/profile/Tracy.hpp"

// Windows SDK headers confined to this translation unit via the Impl pimpl.
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <iostream>

using Microsoft::WRL::ComPtr;

namespace entity {

struct WasapiDevice::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioRenderClient>  renderClient;
    ComPtr<IAudioClock>         clock;
    UINT32                      bufferFrameCount{0};
};

bool WasapiDevice::start(int requestedSampleRate, int channels, FillCallback fill) {
    if (m_running.load()) return true;

    m_sampleRate = requestedSampleRate;
    m_channels   = channels;
    m_fill       = std::move(fill);

    // No COM work on the editor thread. Spawn the render thread and block
    // until it signals init success/failure.
    std::promise<bool> initPromise;
    std::future<bool>  initFuture = initPromise.get_future();
    m_thread = std::thread(&WasapiDevice::run, this, std::move(initPromise));
    return initFuture.get();  // blocks until run() signals
}

void WasapiDevice::run(std::promise<bool> initPromise) {
    tracy::SetThreadName("AudioRender");

    // --- COM init on the render thread ---
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[WasapiDevice] CoInitializeEx failed: " << std::hex << hr << std::endl;
        initPromise.set_value(false);
        return;
    }
    const bool comInitedByUs = SUCCEEDED(hr); // RPC_E_CHANGED_MODE => already inited, don't uninit

    auto impl = new Impl();

    // --- Device enumeration and client setup ---
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(impl->enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] CoCreateInstance(MMDeviceEnumerator) failed: "
                  << std::hex << hr << std::endl;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                    impl->device.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] GetDefaultAudioEndpoint failed: "
                  << std::hex << hr << std::endl;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                 reinterpret_cast<void**>(impl->client.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] Activate(IAudioClient) failed: "
                  << std::hex << hr << std::endl;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    // Build float32 stereo WAVEFORMATEXTENSIBLE at the requested rate.
    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = static_cast<WORD>(m_channels);
    wfx.Format.nSamplesPerSec  = static_cast<DWORD>(m_sampleRate);
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = static_cast<WORD>(m_channels * 4);
    wfx.Format.nAvgBytesPerSec = static_cast<DWORD>(m_sampleRate * m_channels * 4);
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = (m_channels == 2)
                        ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                        : 0;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // Shared-mode event-driven. Both hnsBufferDuration and hnsPeriodicity
    // must be 0 for shared-mode event-driven (MSDN requirement).
    // AUTOCONVERTPCM lets us always pass our desired format; the driver resamples.
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                   0, 0,
                                   reinterpret_cast<WAVEFORMATEX*>(&wfx),
                                   nullptr);
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] IAudioClient::Initialize failed: "
                  << std::hex << hr << std::endl;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        std::cerr << "[WasapiDevice] CreateEvent failed" << std::endl;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }
    m_renderEvent = ev;

    hr = impl->client->SetEventHandle(ev);
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] SetEventHandle failed: " << std::hex << hr << std::endl;
        CloseHandle(ev); m_renderEvent = nullptr;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->client->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(impl->renderClient.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] GetService(IAudioRenderClient) failed: "
                  << std::hex << hr << std::endl;
        CloseHandle(ev); m_renderEvent = nullptr;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->client->GetService(__uuidof(IAudioClock),
                                   reinterpret_cast<void**>(impl->clock.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] GetService(IAudioClock) failed: "
                  << std::hex << hr << std::endl;
        CloseHandle(ev); m_renderEvent = nullptr;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->client->GetBufferSize(&impl->bufferFrameCount);
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] GetBufferSize failed: " << std::hex << hr << std::endl;
        CloseHandle(ev); m_renderEvent = nullptr;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    hr = impl->client->Start();
    if (FAILED(hr)) {
        std::cerr << "[WasapiDevice] IAudioClient::Start failed: " << std::hex << hr << std::endl;
        CloseHandle(ev); m_renderEvent = nullptr;
        delete impl;
        if (comInitedByUs) CoUninitialize();
        initPromise.set_value(false);
        return;
    }

    // Publish impl and mark running before signalling success so
    // framesPlayed() is safe as soon as start() returns true.
    m_impl = impl;
    m_running.store(true, std::memory_order_release);
    initPromise.set_value(true);  // unblocks start()

    // --- MMCSS priority for the render loop ---
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // --- Render loop ---
    while (m_running.load()) {
        DWORD waitResult = WaitForSingleObject(
            static_cast<HANDLE>(m_renderEvent), 2000);
        if (waitResult != WAIT_OBJECT_0) continue;
        if (!m_running.load()) break;

        UINT32 padding = 0;
        hr = impl->client->GetCurrentPadding(&padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { m_running.store(false); break; }
        if (FAILED(hr)) continue;

        const UINT32 available = impl->bufferFrameCount - padding;
        if (available == 0) continue;

        BYTE* buffer = nullptr;
        hr = impl->renderClient->GetBuffer(available, &buffer);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { m_running.store(false); break; }
        if (FAILED(hr)) continue;

        if (m_fill) {
            m_fill(reinterpret_cast<float*>(buffer), available);
        } else {
            std::memset(buffer, 0, available * m_channels * sizeof(float));
        }

        impl->renderClient->ReleaseBuffer(available, 0);
    }

    // --- COM teardown on the render thread ---
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);

    impl->client->Stop();

    // Release COM objects before CoUninitialize.
    impl->clock.Reset();
    impl->renderClient.Reset();
    impl->client.Reset();
    impl->device.Reset();
    impl->enumerator.Reset();
    delete impl;
    m_impl = nullptr;

    if (m_renderEvent) {
        CloseHandle(static_cast<HANDLE>(m_renderEvent));
        m_renderEvent = nullptr;
    }

    if (comInitedByUs) CoUninitialize();
}

void WasapiDevice::stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    // Signal the render event so the thread wakes and exits the loop.
    if (m_renderEvent) SetEvent(static_cast<HANDLE>(m_renderEvent));
    if (m_thread.joinable()) m_thread.join();
    // Thread has done all COM teardown; m_impl and m_renderEvent are
    // already nulled by run().
}

uint64_t WasapiDevice::framesPlayed() const {
    Impl* impl = m_impl;
    if (!impl || !impl->clock) return 0;
    UINT64 pos = 0, freq = 1;
    if (FAILED(impl->clock->GetPosition(&pos, nullptr))) return 0;
    if (FAILED(impl->clock->GetFrequency(&freq)) || freq == 0) return 0;
    // Use double math to avoid integer overflow on long sessions.
    return static_cast<uint64_t>(
        static_cast<double>(pos) / static_cast<double>(freq)
        * static_cast<double>(m_sampleRate));
}

} // namespace entity
