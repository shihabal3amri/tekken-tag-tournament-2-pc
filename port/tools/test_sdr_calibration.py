"""Execute the production SDR transform on D3D11 WARP and compare readback."""
import ast, re, subprocess, tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT, compile_one
CODE=r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
#define HR(x) if(FAILED(x)){printf("FAIL line %d\n",__LINE__);return 1;}
int main(){ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
const char* vs="float4 main(uint id:SV_VertexID):SV_Position {float2 t=float2((id<<1)&2,id&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}";
const char* ps=R"(cbuffer P:register(b0){float brightness;float gamma;float contrast;float reserved;};
float4 main(float4 pos:SV_Position):SV_Target {float4 c=float4((pos.x-0.5)/255.0,0.25,0.75,1);
float2 origin=float2(0,0),srcSize=float2(256,64);
@REFERENCE@
@TRANSFORM@
return c;})";
ComPtr<ID3DBlob> vcode,pcode,error;HR(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vcode,&error));
HR(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&pcode,&error));
ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
HR(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vertex));
HR(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&pixel));
D3D11_TEXTURE2D_DESC d{};d.Width=256;d.Height=64;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;
ComPtr<ID3D11Texture2D> output,readback;ComPtr<ID3D11RenderTargetView> rtv;
HR(device->CreateTexture2D(&d,nullptr,&output));HR(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&d,nullptr,&readback));
D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;HR(device->CreateBuffer(&bd,nullptr,&cb));
context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,256,64,0,1};context->RSSetViewports(1,&vp);
auto* target=rtv.Get();context->OMSetRenderTargets(1,&target,nullptr);auto* buffer=cb.Get();context->PSSetConstantBuffers(0,1,&buffer);
unsigned checks=0;float values[][4]={{1,1,1,0},{0.5,1,1,0},{1.5,1,1,0},{1,0.5,1,0},{1,2.5,1,0},{1,1,0.5,0},{1,1,1.5,0},{0.75,1.8,1.2,0}};
for(int reference:{0,1,0})for(auto& value:values){value[3]=float(reference);context->UpdateSubresource(cb.Get(),0,nullptr,value,0,0);context->Draw(3,0);context->CopyResource(readback.Get(),output.Get());
D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
for(int y=0;y<64;++y)for(int x=0;x<256;++x)for(int channel=0;channel<4;++channel){
 double c=channel==0?x/255.0:channel==1?0.25:channel==2?0.75:1;
 const double u=(x+0.5)/256.0,v=(y+0.5)/64.0;
 if(reference && channel<3) {
  const int band=std::min(7,int(std::floor((v-0.12)/0.76*8)));
  if(v>0.12 && v<0.88 && u>0.02 && u<0.18)c=band*4.0/255;
  if(v>0.12 && v<0.88 && u>0.82 && u<0.98)c=(227+band*4.0)/255;
  if(v>0.03 && v<0.08 && u>0.2 && u<0.8)c=(u-0.2)/0.6;
 }
 if(channel<3)c=std::pow(std::clamp(std::clamp((c-0.5)*value[2]+0.5,0.0,1.0)*value[0],0.0,1.0),1.0/value[1]);
 const int expected=int(std::lround(c*255));const int actual=((unsigned char*)mapped.pData)[y*mapped.RowPitch+4*x+channel];++checks;
 if(std::abs(expected-actual)>1){printf("FAIL pixel%d,%d channel%d reference%d got%d want%d\n",x,y,channel,reference,actual,expected);return 1;}
}context->Unmap(readback.Get(),0);}
printf("PASS %u SDR GPU readback checks: reference panels on/off, neutral, range endpoints, combined controls, clipping and alpha\n",checks);
}
'''
source=(ROOT/'port/src/runtime/kernel/gpu_hw_d3d11.h').read_text()
match=re.search(r'body \+= ("  if\(brightness.*?);',source)
if not match:raise SystemExit('Production SDR transform not found')
# The line is one C++ string literal. Avoid a copied implementation as the test target.
line=next(line.strip() for line in source.splitlines() if 'body += "  if(brightness' in line)
transform=ast.literal_eval(line[len('body += '):-1])
start=source.index('body += "  if(reserved>0.5)')
end=source.index(';\n',start)
reference=''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"',source[start:end]))
with tempfile.TemporaryDirectory(prefix='ttt2-sdr-') as temp:
    exe=compile_one(Path(temp),'sdr',CODE.replace('@REFERENCE@',reference).replace('@TRANSFORM@',transform),[])
    subprocess.run([str(exe)],check=True,timeout=60)
