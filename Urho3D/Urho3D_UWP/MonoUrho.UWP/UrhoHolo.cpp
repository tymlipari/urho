
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <windows.graphics.holographic.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <SDL\SDL.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Scene/Node.h>

using namespace Microsoft::WRL;
using namespace Urho3D;
using namespace Windows::Graphics::Holographic;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
using namespace Windows::UI::Core;

static HolographicSpace^ holographicSpace;
static HolographicFrame^ current_frame;

static ComPtr<ID3D11Device> d3ddevice;
static ComPtr<ID3D11DeviceContext> context;
static Direct3D11::IDirect3DDevice^ m_d3dInteropDevice;
static ComPtr<ID3D11Device4> m_d3dDevice;
static ComPtr<ID3D11DeviceContext3> m_d3dContext;
static ComPtr<IDXGIAdapter3> m_dxgiAdapter;
static D3D_FEATURE_LEVEL m_d3dFeatureLevel = D3D_FEATURE_LEVEL_10_0;

static ComPtr<ID3D11Texture2D> cameraBackBuffer;
static ComPtr<ID3D11Resource> resource;

namespace DX
{
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr))
		{
			// Set a breakpoint on this line to catch Win32 API errors.
			throw Platform::Exception::CreateException(hr);
		}
	}
}

extern "C"
{
    bool HoloLens_IsImmersiveView()
    {
        return holographicSpace != nullptr;
    }

    __declspec(dllexport) void InitializeSpace(_In_ ABI::Windows::Graphics::Holographic::IHolographicSpace** space)
	{
		SDL_SetMainReady();
		holographicSpace = reinterpret_cast<HolographicSpace^>(space);
	}

    HRESULT SDL_UWP_CreateHolographicSwapChain(int width, int height, int multiSample, ID3D11Device** device, IDXGISwapChain** sc, ID3D11DeviceContext** dc)
	{
		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0
		};

		LUID id = { holographicSpace->PrimaryAdapterId.LowPart, holographicSpace->PrimaryAdapterId.HighPart };

		if ((id.HighPart != 0) && (id.LowPart != 0))
		{
			ComPtr<IDXGIFactory1> dxgiFactory;
			DX::ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)));
			ComPtr<IDXGIFactory4> dxgiFactory4;
			DX::ThrowIfFailed(dxgiFactory.As(&dxgiFactory4));
			DX::ThrowIfFailed(dxgiFactory4->EnumAdapterByLuid(id, IID_PPV_ARGS(&m_dxgiAdapter)));
		}

		const HRESULT hr = D3D11CreateDevice(
			m_dxgiAdapter.Get(),        // Either nullptr, or the primary adapter determined by Windows Holographic.
			D3D_DRIVER_TYPE_HARDWARE,   // Create a device using the hardware graphics driver.
			0,                          // Should be 0 unless the driver is D3D_DRIVER_TYPE_SOFTWARE.
			creationFlags,              // Set debug and Direct2D compatibility flags.
			featureLevels,              // List of feature levels this app can support.
			ARRAYSIZE(featureLevels),   // Size of the list above.
			D3D11_SDK_VERSION,          // Always set this to D3D11_SDK_VERSION for Windows Store apps.
			&d3ddevice,                    // Returns the Direct3D device created.
			&m_d3dFeatureLevel,         // Returns feature level of device created.
			&context                    // Returns the device immediate context.
		);

		if (FAILED(hr))
		{
			// If the initialization fails, fall back to the WARP device.
			// For more information on WARP, see:
			// http://go.microsoft.com/fwlink/?LinkId=286690
			DX::ThrowIfFailed(
				D3D11CreateDevice(
					nullptr,              // Use the default DXGI adapter for WARP.
					D3D_DRIVER_TYPE_WARP, // Create a WARP device instead of a hardware device.
					0,
					creationFlags,
					featureLevels,
					ARRAYSIZE(featureLevels),
					D3D11_SDK_VERSION,
					&d3ddevice,
					&m_d3dFeatureLevel,
					&context
				)
			);
		}

		DX::ThrowIfFailed(d3ddevice.As(&m_d3dDevice));
		DX::ThrowIfFailed(context.As(&m_d3dContext));
		ComPtr<IDXGIDevice3> dxgiDevice;
		DX::ThrowIfFailed(m_d3dDevice.As(&dxgiDevice));
		m_d3dInteropDevice = CreateDirect3DDevice(dxgiDevice.Get());
		holographicSpace->SetDirect3D11Device(m_d3dInteropDevice);
		current_frame = holographicSpace->CreateNextFrame();
		ComPtr<IDXGIAdapter> dxgiAdapter;
		DX::ThrowIfFailed(dxgiDevice->GetAdapter(&dxgiAdapter));
		DX::ThrowIfFailed(dxgiAdapter.As(&m_dxgiAdapter));

		*device = m_d3dDevice.Get();
		*dc = m_d3dContext.Get();
		return S_OK;
	}

    void Camera_SetHoloProjection(Camera* camera, const class Matrix4& view, const class Matrix4& projection)
	{
		float nearClip = projection.m32_ / projection.m22_; //0.1
		float farClip = projection.m32_ / (projection.m22_ + 1); //20
		float fovVertical = 360 * atanf(1 / projection.m11_) / M_PI; //17.1
		float aspect = projection.m11_ / projection.m00_; //1.76

		camera->SetSkew(projection.m10_);
		camera->SetProjectionOffset(Vector2(-projection.m20_ / 2.0f, -projection.m21_ / 2.0f));
		camera->SetAspectRatio(aspect);
		camera->SetFov(fovVertical);
		camera->SetNearClip(nearClip);
		camera->SetFarClip(farClip);

		Matrix4 viewt = view.Inverse().Transpose();
		Quaternion rotation = viewt.Rotation();
		rotation.x_ *= -1; //RH to LH
		rotation.y_ *= -1;

		auto cameraNode = camera->GetNode();
		cameraNode->SetWorldPosition(Vector3(viewt.m03_, viewt.m13_, -viewt.m23_));
		cameraNode->SetWorldRotation(rotation);
	}
	
	__declspec(dllexport) void Camera_SetHoloProjections(
		Camera* leftEyeCamera, Camera* rightEyeCamera, Camera* cullingCamera, 
		const class Matrix4& leftView, const class Matrix4& leftProjection, 
		const class Matrix4& rightView, const class Matrix4& rightProjection)
	{
		Camera_SetHoloProjection(leftEyeCamera, leftView, leftProjection);
		Camera_SetHoloProjection(rightEyeCamera, rightView, rightProjection);

		if (cullingCamera)
		{
			auto leftCameraNode = leftEyeCamera->GetNode();
			auto rightCameraNode = rightEyeCamera->GetNode();
			
			auto leftToRightVec = (rightCameraNode->GetWorldPosition() - leftCameraNode->GetWorldPosition());
			float separation = leftToRightVec.Length();

			// Note: atanf and tanf cancel, as does reciprocal
			// float fovHorizontal = 360 * atanf(1 / projection.m00_) / M_PI;
			// float fovHorizontalHalfedRad = atanf(1 / leftProjection.m00_);
			// float cullEyePullback = (0.5f * separation) / tanf(fovHorizontalHalfedRad);
			float cullEyePullback = (0.5f * separation) * leftProjection.m00_;

			// Move cull camera between eyes and pull back
			auto cullCameraPosition = (leftCameraNode->GetWorldPosition() + (0.5f * leftToRightVec)) - (leftCameraNode->GetWorldDirection().Normalized() * cullEyePullback);
			
			// Copy projection and rotation from left camera and set the position
			Camera_SetHoloProjection(cullingCamera, leftView, leftProjection);
			cullingCamera->GetNode()->SetWorldPosition(cullCameraPosition);
			
			// Move culling camera's near and far planes ahead to match that of the eye cameras
			cullingCamera->SetNearClip(cullingCamera->GetNearClip() + cullEyePullback);
			cullingCamera->SetFarClip(cullingCamera->GetFarClip() + cullEyePullback);
		}
	}

	ID3D11Texture2D* HoloLens_GetBackbuffer()
	{
		if (current_frame->CurrentPrediction->CameraPoses->Size > 0)
		{
			auto pose = current_frame->CurrentPrediction->CameraPoses->First()->Current;//TOOD: remove First()
			auto renderingParams = current_frame->GetRenderingParameters(pose);
			IDirect3DSurface^ surface = renderingParams->Direct3D11BackBuffer;
			DX::ThrowIfFailed(Windows::Graphics::DirectX::Direct3D11::GetDXGIInterfaceFromObject(surface, IID_PPV_ARGS(&resource)));
			DX::ThrowIfFailed(resource.As(&cameraBackBuffer));
		}
		return cameraBackBuffer.Get();
	}

	__declspec(dllexport) void HoloLens_SetCurrentFrame(_In_ ABI::Windows::Graphics::Holographic::IHolographicFrame** frame)
	{
		current_frame = reinterpret_cast<HolographicFrame^>(frame);
	}
}