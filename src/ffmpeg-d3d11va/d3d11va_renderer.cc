#include "d3d11va_renderer.h"
#include "av_log.h"

#include <wrl.h>
using Microsoft::WRL::ComPtr;

D3D11VARenderer::D3D11VARenderer()
{

}

D3D11VARenderer::~D3D11VARenderer()
{

}

void D3D11VARenderer::RenderFrame(AVFrame* frame)
{
	std::lock_guard<std::mutex> locker(mutex_);

	if (!d3d11_context_) {
		return;
	}

	ID3D11Texture2D* texture = (ID3D11Texture2D*)frame->data[0];
	int index = (int)frame->data[1];


	if (pixel_format_ != DX::PIXEL_FORMAT_NV12 ||
		width_ != frame->width || height_ != frame->height) {
		if (!CreateTexture(frame->width, frame->height, DX::PIXEL_FORMAT_NV12)) {
			return;
		}
	}

	Begin();

	ID3D11Texture2D* nv12_texture = input_textures_[DX::PIXEL_PLANE_NV12]->GetTexture();
	ID3D11ShaderResourceView* nv12_texture_y_srv = input_textures_[DX::PIXEL_PLANE_NV12]->GetNV12YShaderResourceView();
	ID3D11ShaderResourceView* nv12_texture_uv_srv = input_textures_[DX::PIXEL_PLANE_NV12]->GetNV12UVShaderResourceView();

	if (!useShareTexture)
	{
		d3d11_context_->CopySubresourceRegion(
			nv12_texture,
			0,
			0,
			0,
			0,
			(ID3D11Resource*)texture,
			index,
			NULL);
	}
	else
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> deviceCtx;
		ComPtr<ID3D11Texture2D> videoTextureShared;

		HANDLE textureHandle = input_textures_[DX::PIXEL_PLANE_NV12]->GetTextureHandle();

		texture->GetDevice(device.GetAddressOf());
		if (!device) {
			LOG("GetDevice fail");
			return;
		}

		device->GetImmediateContext(deviceCtx.GetAddressOf());
		if (!deviceCtx) {
			LOG("GetImmediateContext fail");
			return;
		}

		HRESULT hr = device->OpenSharedResource(textureHandle,
			__uuidof(ID3D11Texture2D),
			(void**)videoTextureShared.GetAddressOf());
		if (FAILED(hr)) {
			LOG("OpenSharedResource fail");
			return;
		}

		deviceCtx->CopySubresourceRegion(
			videoTextureShared.Get(),
			0,
			0, 0, 0,
			texture,
			index,
			NULL);

		deviceCtx->Flush();
	}

	DX::D3D11RenderTexture* render_target = render_targets_[DX::PIXEL_SHADER_NV12_BT601].get();
	if (render_target) {
		this->UpdateMatrix(render_target);

		render_target->Begin();
		render_target->PSSetTexture(0, nv12_texture_y_srv);
		render_target->PSSetTexture(1, nv12_texture_uv_srv);
		render_target->PSSetSamplers(0, linear_sampler_);
		render_target->PSSetSamplers(1, point_sampler_);
		render_target->Draw();
		render_target->End();
		render_target->PSSetTexture(0, NULL);
		render_target->PSSetTexture(1, NULL);
		output_texture_ = render_target;
	}

	Process();
	End();
}
