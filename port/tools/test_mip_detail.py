"""Verify production sampler mip bias by sampling colored mip levels on WARP."""
import ast,re,subprocess,tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT,compile_one
source=(ROOT/'port/src/runtime/kernel/gpu_hw_d3d11.h').read_text()
sampler=re.search(r'inline D3D11_SAMPLER_DESC SamplerDesc\(.*?\n\}',source,re.S).group()
CODE=r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
@SAMPLER@
#define HR(x) if(FAILED(x)){printf("FAIL line %d\n",__LINE__);return 1;}
int main(){ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
const char* vs="float4 main(uint id:SV_VertexID):SV_Position {float2 t=float2((id<<1)&2,id&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}";
const char* ps="Texture2D<float4> tex:register(t0);SamplerState smp:register(s0);float4 main(float4 pos:SV_Position):SV_Target{return tex.Sample(smp,pos.xy/256.0);}";
ComPtr<ID3DBlob> vcode,pcode,error;HR(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vcode,&error));HR(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&pcode,&error));
ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;HR(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vertex));HR(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&pixel));
std::vector<uint32_t> levels[9];D3D11_SUBRESOURCE_DATA data[9]{};
for(int level=0;level<9;++level){unsigned w=256u>>level;unsigned c=16+level*24;levels[level].assign(w*w,0xff000000u|c|(c<<8)|(c<<16));data[level]={levels[level].data(),w*4,0};}
D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=256;d.MipLevels=9;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
ComPtr<ID3D11Texture2D> texture,output,readback;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
HR(device->CreateTexture2D(&d,data,&texture));HR(device->CreateShaderResourceView(texture.Get(),nullptr,&srv));
d.Height=1;d.MipLevels=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;HR(device->CreateTexture2D(&d,nullptr,&output));HR(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&d,nullptr,&readback));
context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
D3D11_VIEWPORT vp{0,0,256,1,0,1};context->RSSetViewports(1,&vp);auto* target=rtv.Get();context->OMSetRenderTargets(1,&target,nullptr);auto* resource=srv.Get();context->PSSetShaderResources(0,1,&resource);
uint32_t info[4]={2,2,1,0};unsigned checks=0;
for(unsigned bias=0;bias<4;++bias){auto descriptor=SamplerDesc(info,1|(8<<10),0,bias);ComPtr<ID3D11SamplerState> state;HR(device->CreateSamplerState(&descriptor,&state));auto* sampler=state.Get();context->PSSetSamplers(0,1,&sampler);context->Draw(3,0);context->CopyResource(readback.Get(),output.Get());
D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
for(int x=0;x<256;++x){++checks;const int got=((unsigned char*)mapped.pData)[x*4],expected=16+bias*24;if(abs(got-expected)>1){printf("FAIL mip bias%u x%d got%d want%d\n",bias,x,got,expected);return 1;}}
context->Unmap(readback.Get(),0);if(SamplerDesc(info,0,0,bias).MipLODBias!=0)return 2;
}
printf("PASS %u GPU mip samples: real levels 0..3 selected; unmipped textures preserve original bias\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-mips-') as temp:
    exe=compile_one(Path(temp),'mips',CODE.replace('@SAMPLER@',sampler),[])
    subprocess.run([str(exe)],check=True,timeout=60)
