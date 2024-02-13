#include "WGCFrameProcessor.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include "WindowsGraphicsCapture.util.h"
#include <d3d11.h>
#include <atlcomcli.h>

using namespace Graphics::Capture::Util;
using namespace Graphics::Capture;

using namespace winrt;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace Microsoft::WRL;

namespace SL {
namespace Screen_Capture {

    // These are the errors we expect from general Dxgi API due to a transition
    static HRESULT SystemTransitionsExpectedErrors[] = {
        DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_ACCESS_LOST, static_cast<HRESULT>(WAIT_ABANDONED),
        S_OK // Terminate list with zero valued HRESULT
    };

    // These are the errors we expect from IDXGIOutput1::DuplicateOutput due to a transition
    static HRESULT CreateDuplicationExpectedErrors[] = {
        DXGI_ERROR_DEVICE_REMOVED, static_cast<HRESULT>(E_ACCESSDENIED), DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_SESSION_DISCONNECTED,
        S_OK // Terminate list with zero valued HRESULT
    };

    // These are the errors we expect from IDXGIOutputDuplication methods due to a transition
    static HRESULT FrameInfoExpectedErrors[] = {
        DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_INVALID_CALL,
        S_OK // Terminate list with zero valued HRESULT
    };

    // These are the errors we expect from IDXGIAdapter::EnumOutputs methods due to outputs becoming stale during a transition
    static HRESULT EnumOutputsExpectedErrors[] = {
        DXGI_ERROR_NOT_FOUND,
        S_OK // Terminate list with zero valued HRESULT
    };

    static DUPL_RETURN ProcessFailure(ID3D11Device *Device, LPCWSTR Str, LPCWSTR Title, HRESULT hr, HRESULT *ExpectedErrors = nullptr)
    {
        HRESULT TranslatedHr;
#if defined _DEBUG || !defined NDEBUG
        std::wcout << "HRESULT: " << std::hex << hr << "\t" <<Str << "\t" << Title << std::endl;
#endif
        // On an error check if the DX device is lost
        if (Device) {
            HRESULT DeviceRemovedReason = Device->GetDeviceRemovedReason();

            switch (DeviceRemovedReason) {
            case DXGI_ERROR_DEVICE_REMOVED:
            case DXGI_ERROR_DEVICE_RESET:
            case static_cast<HRESULT>(E_OUTOFMEMORY): {
                // Our device has been stopped due to an external event on the GPU so map them all to
                // device removed and continue processing the condition
                TranslatedHr = DXGI_ERROR_DEVICE_REMOVED;
                break;
            }

            case S_OK: {
                // Device is not removed so use original error
                TranslatedHr = hr;
                break;
            }

            default: {
                // Device is removed but not a error we want to remap
                TranslatedHr = DeviceRemovedReason;
            }
            }
        }
        else {
            TranslatedHr = hr;
        }

        // Check if this error was expected or not
        if (ExpectedErrors) {
            HRESULT *CurrentResult = ExpectedErrors;

            while (*CurrentResult != S_OK) {
                if (*(CurrentResult++) == TranslatedHr) {
                    return DUPL_RETURN_ERROR_EXPECTED;
                }
            }
        }

        return DUPL_RETURN_ERROR_UNEXPECTED;
    }


    static RECT ConvertRect(RECT Dirty, const DXGI_OUTPUT_DESC &DeskDesc)
    {
        RECT DestDirty = Dirty;
        INT Width = DeskDesc.DesktopCoordinates.right - DeskDesc.DesktopCoordinates.left;
        INT Height = DeskDesc.DesktopCoordinates.bottom - DeskDesc.DesktopCoordinates.top;

        // Set appropriate coordinates compensated for rotation
        switch (DeskDesc.Rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: {

            DestDirty.left = Width - Dirty.bottom;
            DestDirty.top = Dirty.left;
            DestDirty.right = Width - Dirty.top;
            DestDirty.bottom = Dirty.right;

            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180: {
            DestDirty.left = Width - Dirty.right;
            DestDirty.top = Height - Dirty.bottom;
            DestDirty.right = Width - Dirty.left;
            DestDirty.bottom = Height - Dirty.top;

            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270: {
            DestDirty.left = Dirty.top;
            DestDirty.top = Height - Dirty.right;
            DestDirty.right = Dirty.bottom;
            DestDirty.bottom = Height - Dirty.left;

            break;
        }
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY: {
            break;
        }
        default:
            break;
        }
        return DestDirty;
    }

    class AquireFrameRAII {

        IDXGIOutputDuplication *_DuplLock;
        bool AquiredLock;
        void TryRelease()
        {
            if (AquiredLock) {
                auto hr = _DuplLock->ReleaseFrame();
                if (FAILED(hr) && hr != DXGI_ERROR_WAIT_TIMEOUT) {
                    ProcessFailure(nullptr, L"Failed to release frame in DUPLICATIONMANAGER", L"Error", hr, FrameInfoExpectedErrors);
                }
            }
            AquiredLock = false;
        }

      public:
        AquireFrameRAII(IDXGIOutputDuplication *dupl) : _DuplLock(dupl), AquiredLock(false) {}

        ~AquireFrameRAII() { TryRelease(); }
        HRESULT AcquireNextFrame(UINT TimeoutInMilliseconds, DXGI_OUTDUPL_FRAME_INFO *pFrameInfo, IDXGIResource **ppDesktopResource)
        {
            auto hr = _DuplLock->AcquireNextFrame(TimeoutInMilliseconds, pFrameInfo, ppDesktopResource);
            TryRelease();
            AquiredLock = SUCCEEDED(hr);
            return hr;
        }
    };
    class MAPPED_SUBRESOURCERAII {
        ID3D11DeviceContext *_Context;
        ID3D11Resource *_Resource;
        UINT _Subresource;

      public:
        MAPPED_SUBRESOURCERAII(ID3D11DeviceContext *context) : _Context(context), _Resource(nullptr), _Subresource(0) {}

        ~MAPPED_SUBRESOURCERAII() { _Context->Unmap(_Resource, _Subresource); }
        HRESULT Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
        {
            if (_Resource != nullptr) {
                _Context->Unmap(_Resource, _Subresource);
            }
            _Resource = pResource;
            _Subresource = Subresource;
            return _Context->Map(_Resource, _Subresource, MapType, MapFlags, pMappedResource);
        }
    };

    DUPL_RETURN WGCFrameProcessor::Init(std::shared_ptr<Thread_Data> data, Monitor &monitor)
    {
        SelectedMonitor = monitor;
        DX_RESOURCES res;
        auto ret = Initialize(res);
        if (ret != DUPL_RETURN_SUCCESS) {
            return ret;
        }
        WGC_RESOURCES wgc;
        ret = Initialize(wgc, res.Device.Get(), Adapter(SelectedMonitor), Id(SelectedMonitor));
        if (ret != DUPL_RETURN_SUCCESS) {
            return ret;
        }
        Device = res.Device;
        DeviceContext = res.DeviceContext;
        OutputDesc = wgc.OutputDesc;
        Output = wgc.Output;
        m_CaptureItem = *wgc.CaptureItem;
        m_framePool = *wgc.framePool;
        m_session =*wgc.session;

        Data = data;

        return ret;
    }

     DUPL_RETURN WGCFrameProcessor::Initialize(DX_RESOURCES &data)
    {

        HRESULT hr = S_OK;

        // Driver types supported
        D3D_DRIVER_TYPE DriverTypes[] = {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
            D3D_DRIVER_TYPE_REFERENCE,
        };
        UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

        // Feature levels supported
        D3D_FEATURE_LEVEL FeatureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1};
        UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

        D3D_FEATURE_LEVEL FeatureLevel;

        // Create device
        for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex) {
            hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, 0, FeatureLevels, NumFeatureLevels, D3D11_SDK_VERSION,
                                   data.Device.GetAddressOf(), &FeatureLevel, data.DeviceContext.GetAddressOf());

            if (SUCCEEDED(hr)) {
                // Device creation success, no need to loop anymore
                break;
            }
        }
        if (FAILED(hr)) {
            return ProcessFailure(nullptr, L"Failed to create device in InitializeDx", L"Error", hr);
        }

        return DUPL_RETURN_SUCCESS;
    }

    DUPL_RETURN WGCFrameProcessor::Initialize(WGC_RESOURCES &r, ID3D11Device *device, const UINT adapter, const UINT output)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory> pFactory;

        // Create a DXGIFactory object.
        HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)pFactory.GetAddressOf());
        if (FAILED(hr)) {
            return ProcessFailure(nullptr, L"Failed to construct DXGIFactory", L"Error", hr);
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> DxgiAdapter;
        hr = pFactory->EnumAdapters(adapter, DxgiAdapter.GetAddressOf());

        if (FAILED(hr)) {
            return ProcessFailure(device, L"Failed to get DXGI Adapter", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        // Get output
        Microsoft::WRL::ComPtr<IDXGIOutput> DxgiOutput;
        hr = DxgiAdapter->EnumOutputs(output, DxgiOutput.GetAddressOf());

        if (FAILED(hr)) {
            return ProcessFailure(device, L"Failed to get specified output in DUPLICATIONMANAGER", L"Error", hr, EnumOutputsExpectedErrors);
        }

        DxgiOutput->GetDesc(&r.OutputDesc);

        try {
            *(r.CaptureItem) = CreateCaptureItemForMonitor(r.OutputDesc.Monitor);
        }
        catch (winrt::hresult_error const &ex) {
            hr = ex.code();
            return ProcessFailure(device, L"Failed to Create Capture Item For Monitor", L"Error", hr, nullptr);
        }

        CComPtr<IDXGIDevice> DxgiDevice = nullptr;
        hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&DxgiDevice));

        if (FAILED(hr)) {
            return ProcessFailure(device, L"Failed to QueryInterface IDXGIDevice ", L"Error", hr, nullptr);
        }

        try {
            auto direct3DDevice = Graphics::Capture::Util::CreateDirect3DDevice(DxgiDevice);
            // Creating our frame pool with 'Create' instead of 'CreateFreeThreaded'
            // means that the frame pool's FrameArrived event is called on the thread
            // the frame pool was created on. This also means that the creating thread
            // must have a DispatcherQueue. If you use this method, it's best not to do
            // it on the UI thread.
            *r.framePool =
                Direct3D11CaptureFramePool::CreateFreeThreaded(direct3DDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, r.CaptureItem->Size());

            *r.session = r.framePool->CreateCaptureSession(*r.CaptureItem);

            if (IsGraphicsCaptureIsBorderRequiredPropertyAvailable()) {
                r.session->IsBorderRequired(false);
            }

                r.framePool->FrameArrived({this, &WGCFrameProcessor::OnFrameArrived});

            if (IsGraphicsCaptureCursorCapturePropertyAvailable()) {
                    r.session->IsCursorCaptureEnabled(true);
             }
              
            /*
                 m_session.StartCapture();

            */
        }
        catch (winrt::hresult_error const &ex) 
        {
            hr = ex.code();
            return ProcessFailure(device, L"Failed to create WindowsGraphicsCapture session ", L"Error", hr, nullptr);
        }

        // IDirect3DDevice

        //  IDirect3DDevice  direct3DDevice = reinterpret_cast<IDirect3DDevice>(d3d11Device);
        //  *r.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(dev,
        //  winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        //                                                                               r.CaptureItem->Size());

        r.Output = output;
        return DUPL_RETURN_SUCCESS;
    }


    void WGCFrameProcessor::OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args)
    {

    }

    //
    // Process a given frame and its metadata
    //

    DUPL_RETURN WGCFrameProcessor::ProcessFrame(const Monitor &currentmonitorinfo)
    {
        return DUPL_RETURN_SUCCESS;
    }
} // namespace Screen_Capture
} // namespace SL


