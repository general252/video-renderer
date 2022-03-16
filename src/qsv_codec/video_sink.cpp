#include "video_sink.h"

VideoSink::VideoSink()
{

}

VideoSink::~VideoSink()
{
	Destroy();
}

bool VideoSink::Init(HWND hwnd, int width, int height)
{
	if (!DX::D3D11Renderer::Init(hwnd)) {
		printf("[VideoSink] Init dx11 renderer failed.");
		return false;
	}

	color_converter_ = std::make_shared<DX::D3D11YUVToRGBConverter>(d3d11_device_);
	if (!color_converter_->Init(width, height)) {
		printf("[VideoSink] Init color converter failed.");
		goto failed;
	}

	yuv420_decoder_ = std::make_shared<D3D11VADecoder>(d3d11_device_);
	yuv420_decoder_->SetOption(AV_DECODER_OPTION_WIDTH, width);
	yuv420_decoder_->SetOption(AV_DECODER_OPTION_HEIGHT, height);
	if (!yuv420_decoder_->Init()) {
		printf("[VideoSink] Init yuv420 decoder failed.");
		goto failed;
	}

	chroma420_decoder_ = std::make_shared<D3D11VADecoder>(d3d11_device_);
	chroma420_decoder_->SetOption(AV_DECODER_OPTION_WIDTH, width);
	chroma420_decoder_->SetOption(AV_DECODER_OPTION_HEIGHT, height);
	if (!chroma420_decoder_->Init()) {
		printf("[VideoSink] Init chroma420 decoder failed.");
		goto failed;
	}

	return true;

failed:
	DX::D3D11Renderer::Destroy();
	return false;
}

void VideoSink::Destroy()
{
	if (yuv420_decoder_) {
		yuv420_decoder_->Destroy();
	}

	if (chroma420_decoder_) {
		chroma420_decoder_->Destroy();
	}

	DX::D3D11Renderer::Destroy();
}

void VideoSink::RenderFrame(DX::Image& image)
{
	ID3D11Texture2D* texture = nullptr;

	HRESULT hr = d3d11_device_->OpenSharedResource((HANDLE)(uintptr_t)image.shared_handle,
		__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
	if (FAILED(hr)) {
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	texture->GetDesc(&desc);

	if (pixel_format_ != DX::PIXEL_FORMAT_ARGB ||
		width_ != desc.Width || height_ != desc.Height) {
		if (!CreateTexture(desc.Width, desc.Height, DX::PIXEL_FORMAT_ARGB)) {
			return;
		}
	}

	Begin();

	ID3D11Texture2D* argb_texture = input_textures_[DX::PIXEL_PLANE_ARGB]->GetTexture();
	ID3D11ShaderResourceView* argb_texture_svr = input_textures_[DX::PIXEL_PLANE_ARGB]->GetShaderResourceView();

	d3d11_context_->CopySubresourceRegion(
		argb_texture,
		0,
		0,
		0,
		0,
		(ID3D11Resource*)texture,
		0,
		NULL);

	DX::D3D11RenderTexture* render_target = render_targets_[DX::PIXEL_SHADER_ARGB].get();
	if (render_target) {
		render_target->Begin();
		render_target->PSSetTexture(0, argb_texture_svr);
		render_target->PSSetSamplers(0, linear_sampler_);
		render_target->PSSetSamplers(1, point_sampler_);
		render_target->Draw();
		render_target->End();
		render_target->PSSetTexture(0, NULL);
		output_texture_ = render_target;
	}

	End();
}

void VideoSink::RenderNV12(std::vector<std::vector<uint8_t>>& compressed_frame)
{
	if (compressed_frame.size() != 2) {
		return;
	}

	if (compressed_frame[0].empty() ||
		compressed_frame[1].empty()) {
		return;
	}

	std::shared_ptr<AVFrame> yuv420_frame, chroma420_frame;

	int ret = yuv420_decoder_->Send(compressed_frame[0]);
	if (ret < 0) {
		printf("[VideoSink] Send yuv420 frame failed. \n");
		return;
	}

	ret = yuv420_decoder_->Recv(yuv420_frame);
	if (ret < 0) {
		printf("[VideoSink] Recv yuv420 frame failed. \n");
		return;
	}

	ret = chroma420_decoder_->Send(compressed_frame[1]);
	if (ret < 0) {
		printf("[VideoSink] Send chroma420 frame failed. \n");
		return;
	}

	ret = chroma420_decoder_->Recv(chroma420_frame);
	if (ret < 0) {
		printf("[VideoSink] Recv chroma420 frame failed. \n");
		return;
	}

	ID3D11Texture2D* yuv420_texture = (ID3D11Texture2D*)yuv420_frame->data[0];
	int yuv420_index = (int)yuv420_frame->data[1];

	D3D11_TEXTURE2D_DESC desc;
	yuv420_texture->GetDesc(&desc);

	if (pixel_format_ != DX::PIXEL_FORMAT_NV12 ||
		width_ != desc.Width || height_ != desc.Height) {
		if (!CreateTexture(desc.Width, desc.Height, DX::PIXEL_FORMAT_NV12)) {			
			return;
		}
	}

	Begin();

	auto input_texture = input_textures_[DX::PIXEL_PLANE_NV12];
	ID3D11Texture2D* nv12_texture = input_texture->GetTexture();
	ID3D11ShaderResourceView* nv12_texture_y_srv = input_texture->GetNV12YShaderResourceView();
	ID3D11ShaderResourceView* nv12_texture_uv_srv = input_texture->GetNV12UVShaderResourceView();

	d3d11_context_->CopySubresourceRegion(
		nv12_texture,
		0,
		0,
		0,
		0,
		yuv420_texture,
		yuv420_index,
		NULL);

	auto render_target = render_targets_[DX::PIXEL_SHADER_NV12_BT601];
	if (render_target) {
		render_target->Begin();
		render_target->PSSetTexture(0, nv12_texture_y_srv);
		render_target->PSSetTexture(1, nv12_texture_uv_srv);
		render_target->PSSetSamplers(0, linear_sampler_);
		render_target->PSSetSamplers(1, point_sampler_);
		render_target->Draw();
		render_target->End();
		render_target->PSSetTexture(0, NULL);
		render_target->PSSetTexture(1, NULL);
		output_texture_ = render_target.get();
	}

	Process();
	End();

}

void VideoSink::RenderARGB(std::vector<std::vector<uint8_t>>& compressed_frame)
{
	if (compressed_frame.size() != 2) {
		return;
	}

	if (compressed_frame[0].empty() || 
		compressed_frame[1].empty()) {
		return;
	}

	std::shared_ptr<AVFrame> yuv420_frame, chroma420_frame;

	int ret = yuv420_decoder_->Send(compressed_frame[0]);
	if (ret < 0) {
		printf("[VideoSink] Send yuv420 frame failed. \n");
		return;
	}

	ret = yuv420_decoder_->Recv(yuv420_frame);
	if (ret < 0) {
		printf("[VideoSink] Recv yuv420 frame failed. \n");
		return;
	}

	ret = chroma420_decoder_->Send(compressed_frame[1]);
	if (ret < 0) {
		printf("[VideoSink] Send chroma420 frame failed. \n");
		return;
	}

	ret = chroma420_decoder_->Recv(chroma420_frame);
	if (ret < 0) {
		printf("[VideoSink] Recv chroma420 frame failed. \n");
		return;
	}

	ID3D11Texture2D* yuv420_texture = (ID3D11Texture2D*)yuv420_frame->data[0];
	int yuv420_index = (int)yuv420_frame->data[1];

	ID3D11Texture2D* chroma420_texture = (ID3D11Texture2D*)chroma420_frame->data[0];
	int chroma420_index = (int)chroma420_frame->data[1];

	if (!color_converter_->Combine(yuv420_texture, yuv420_index, 
		chroma420_texture, chroma420_index)) {
		printf("[VideoSink] Combine frame failed. \n");
		return;
	}

	ID3D11Texture2D* combine_texture = color_converter_->GetRGBATexture();
	D3D11_TEXTURE2D_DESC desc;
	combine_texture->GetDesc(&desc);

	if (pixel_format_ != DX::PIXEL_FORMAT_ARGB ||
		width_ != desc.Width || height_ != desc.Height) {
		if (!CreateTexture(desc.Width, desc.Height, DX::PIXEL_FORMAT_ARGB)) {			
			return;
		}
	}

	Begin();

	auto input_texture = input_textures_[DX::PIXEL_PLANE_ARGB];
	ID3D11Texture2D* argb_texture = input_texture->GetTexture();
	ID3D11ShaderResourceView* argb_texture_svr = input_texture->GetShaderResourceView();

	d3d11_context_->CopySubresourceRegion(
		argb_texture,
		0,
		0,
		0,
		0,
		combine_texture,
		0,
		NULL);

	auto render_target = render_targets_[DX::PIXEL_SHADER_ARGB];
	if (render_target) {
		render_target->Begin();
		render_target->PSSetTexture(0, argb_texture_svr);
		render_target->PSSetSamplers(0, linear_sampler_);
		render_target->PSSetSamplers(1, point_sampler_);
		render_target->Draw();
		render_target->End();
		render_target->PSSetTexture(0, NULL);
		output_texture_ = render_target.get();
	}

	End();
}
