"""Run the production SMAA passes on D3D11 WARP: flat colors, edges, clearing and resize."""
from pathlib import Path
import tempfile,subprocess
from test_vcmpgtuh import ROOT,compile_one
CODE=r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "smaa_d3d11.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
int main(){try{
ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
ttt2_smaa::Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP");
ttt2_smaa::Pass pass;
unsigned changed=0;
for(int test=0;test<4;++test){
 unsigned w=test==3?91:64,h=test==3?57:64;
 std::vector<uint32_t> pixels(w*h,0xFF8C5A28);
 if(test==1)for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)pixels[y*w+x]=x>y*0.71f+7.3f?0xFFFFFFFF:0xFF000000;
 D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};ComPtr<ID3D11Texture2D> input,output,staging;
 ttt2_smaa::Check(device->CreateTexture2D(&d,&data,&input),"input");ComPtr<ID3D11ShaderResourceView> srv;
 ttt2_smaa::Check(device->CreateShaderResourceView(input.Get(),nullptr,&srv),"srv");
 d.BindFlags=D3D11_BIND_RENDER_TARGET;ttt2_smaa::Check(device->CreateTexture2D(&d,nullptr,&output),"output");ComPtr<ID3D11RenderTargetView> rtv;
 ttt2_smaa::Check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv),"rtv");
 context->OMSetBlendState(nullptr,nullptr,0xFFFFFFFF);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(nullptr);
 pass.Run(device.Get(),context.Get(),srv.Get(),rtv.Get(),w,h);
 d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ttt2_smaa::Check(device->CreateTexture2D(&d,nullptr,&staging),"staging");context->CopyResource(staging.Get(),output.Get());
 D3D11_MAPPED_SUBRESOURCE mapped{};ttt2_smaa::Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"readback");
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
  uint32_t got=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch)[x];
  if(test!=1 && got!=pixels[y*w+x]){printf("FAIL flat/clear/resize test=%d x=%u y=%u got=%08X expected=%08X\n",test,x,y,got,pixels[y*w+x]);return 1;}
  if(test==1){changed+=got!=pixels[y*w+x];if((got>>24)!=255){puts("FAIL alpha");return 1;}}
 }
 context->Unmap(staging.Get(),0);
}
if(changed==0||changed>600){printf("FAIL edge changes=%u\n",changed);return 1;}
printf("PASS production SMAA 1x on WARP: %u edge pixels filtered; flat color, alpha, scratch clear and resize preserved\n",changed);
}catch(const std::exception& e){puts(e.what());return 1;}}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-smaa-') as temp:
 exe=compile_one(Path(temp),'smaa',CODE,[ROOT/'port/src/runtime/kernel'])
 subprocess.run([str(exe)],check=True,timeout=90)
