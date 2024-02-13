#pragma once
#include "internal/SCCommon.h"
#include <DXGI.h>
#include <memory>
#include <wrl.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "WindowsGraphicsCapture.util.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")


 struct DX_RESOURCES {
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

struct WGC_RESOURCES {
    DXGI_OUTPUT_DESC OutputDesc;
    UINT Output;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem *CaptureItem;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool *framePool;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession *session;
};

namespace SL {
    namespace Screen_Capture {
        class WGCFrameProcessor : public BaseFrameProcessor {
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> StagingSurf;

            winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_CaptureItem;
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool;
            winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session;

            
            DXGI_OUTPUT_DESC OutputDesc;
            UINT Output;
            std::vector<BYTE> MetaDataBuffer;
            Monitor SelectedMonitor;

        public:

            void Pause() {}
            void Resume() {}
            DUPL_RETURN Init(std::shared_ptr<Thread_Data> data, Monitor &monitor);
            DUPL_RETURN ProcessFrame(const Monitor &currentmonitorinfo);

        private:
            void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
                                winrt::Windows::Foundation::IInspectable const &args);

            DUPL_RETURN Initialize(DX_RESOURCES &data);
            DUPL_RETURN Initialize(WGC_RESOURCES &r, ID3D11Device *device, const UINT adapter, const UINT output);
        };

    } // namespace Screen_Capture
} // namespace SL