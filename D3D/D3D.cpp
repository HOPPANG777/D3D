// Direct3D 11: 회전하는 3D 큐브 (깊이버퍼 + 기본 조명 + 리사이즈 대응)
// 빌드: cl /EHsc /W4 /DUNICODE /DWIN32 /DWIN64 FileName.cpp user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib /link /SUBSYSTEM:WINDOWS

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// --------------------------------- 전역 ---------------------------------
HWND                    g_hWnd = nullptr;
UINT                    g_Width = 1280, g_Height = 720;

IDXGISwapChain* g_SwapChain = nullptr;              // [1]
ID3D11Device* g_Device = nullptr;                   // [2]
ID3D11DeviceContext* g_Context = nullptr;           // [3]
ID3D11RenderTargetView* g_RTV = nullptr;            // [4]
ID3D11Texture2D* g_Depth = nullptr;                 // [5]
ID3D11DepthStencilView* g_DSV = nullptr;            // [6]

ID3D11Buffer* g_VB = nullptr;                       // 정점 버퍼
ID3D11Buffer* g_IB = nullptr;                       // 인덱스 버퍼
ID3D11InputLayout* g_InputLayout = nullptr;         // 입력 레이아웃
ID3D11VertexShader* g_VS = nullptr;                 // VS
ID3D11PixelShader* g_PS = nullptr;                  // PS
ID3D11Buffer* g_CBuffer = nullptr;                  // 상수버퍼(b0: 행렬/조명)

ID3D11RasterizerState* g_RS = nullptr;              // 래스터라이저(백페이스 컬링)
ID3D11DepthStencilState* g_DSS = nullptr;           // 깊이 스텐실 상태
ID3D11SamplerState* g_Sampler = nullptr;            // (텍스처 없지만 샘플러 예시로 생성可)

LARGE_INTEGER           g_Freq{}, g_Prev{};
float                   g_Time = 0.0f;              // 누적 시간 (회전)

// --------------------------------- 유틸 ---------------------------------
#ifndef HR
#define HR(x) do{ HRESULT _hr=(x); if(FAILED(_hr)){ wchar_t buf[256]; wsprintf(buf,L"HR=0x%08X (%hs:%d)",_hr,__FILE__,__LINE__); MessageBoxW(nullptr,buf,L"D3D11 Error",MB_ICONERROR); ExitProcess((UINT)_hr);} }while(0)
#endif

// --------------------------------- 자료형 ---------------------------------
struct VertexPNT          // [7]
{
    XMFLOAT3 pos;         // POSITION
    XMFLOAT3 normal;      // NORMAL
    XMFLOAT3 color;       // COLOR (조명 전 기본 색)
};

struct CBFrame            // b0: 한 프레임 상수버퍼
{
    XMMATRIX World;
    XMMATRIX View;
    XMMATRIX Proj;
    XMFLOAT3 LightDirWS;  // 월드공간 방향광 (단위벡터)
    float    _pad0;
    XMFLOAT3 EyePosWS;    // 카메라 위치
    float    _pad1;
};

// --------------------------------- 선언 ---------------------------------
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void InitD3D(HWND);
void CreateBackbufferAndDSV(UINT, UINT);
void DestroyBackbufferAndDSV();
void CreatePipeline();
void CreateCube();
void UpdateAndDraw();
void Cleanup();

// --------------------------------- WinMain ---------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    // 윈도우
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"D3D11_Cube";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT rc{ 0,0,(LONG)g_Width,(LONG)g_Height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hWnd = CreateWindowW(wc.lpszClassName, L"D3D11 — Rotating Cube",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hWnd, SW_SHOW);

    // 초기화
    InitD3D(g_hWnd);
    CreatePipeline();
    CreateCube();

    QueryPerformanceFrequency(&g_Freq);
    QueryPerformanceCounter(&g_Prev);

    // 루프
    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        else { UpdateAndDraw(); }
    }

    Cleanup();
    return 0;
}

// --------------------------------- WndProc ---------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT m, WPARAM wParam, LPARAM lParam)
{
    switch (m)
    {
    case WM_SIZE:
        if (g_SwapChain)
        {
            UINT w = LOWORD(lParam), h = HIWORD(lParam);
            if (w && h)
            {
                DestroyBackbufferAndDSV();                                          // 기존 뷰/버퍼 파기
                HR(g_SwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));    // 스왑체인 리사이즈
                CreateBackbufferAndDSV(w, h);                                       // 새 RTV/DSV 생성 및 뷰포트 갱신
                g_Width = w; g_Height = h;
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, m, wParam, lParam);
}

// --------------------------------- InitD3D ---------------------------------
void InitD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};                      // [8]
    sd.BufferCount = 2;                             // 더블버퍼
    sd.BufferDesc.Width = g_Width;
    sd.BufferDesc.Height = g_Height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;                        // MSAA off(입문)
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // 플립 모델(권장)

#if defined(_DEBUG)
    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
#else
    UINT flags = 0;
#endif

    D3D_FEATURE_LEVEL fl;
    HR(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &g_SwapChain, &g_Device, &fl, &g_Context));

    CreateBackbufferAndDSV(g_Width, g_Height);
}

// --------------------------------- RTV/DSV 생성 ---------------------------------
void CreateBackbufferAndDSV(UINT width, UINT height)
{
    // RTV
    ID3D11Texture2D* back = nullptr;
    HR(g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back));
    HR(g_Device->CreateRenderTargetView(back, nullptr, &g_RTV));             // [4]
    back->Release();

    // DSV(깊이 텍스처 + 뷰)
    D3D11_TEXTURE2D_DESC td{};                                               // [5]
    td.Width = width; td.Height = height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    HR(g_Device->CreateTexture2D(&td, nullptr, &g_Depth));
    HR(g_Device->CreateDepthStencilView(g_Depth, nullptr, &g_DSV));          // [6]

    // 뷰 바인딩
    g_Context->OMSetRenderTargets(1, &g_RTV, g_DSV);

    // 뷰포트
    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)width; vp.Height = (FLOAT)height; vp.MinDepth = 0; vp.MaxDepth = 1;
    g_Context->RSSetViewports(1, &vp);
}

void DestroyBackbufferAndDSV()
{
    if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
    if (g_DSV) { g_DSV->Release(); g_DSV = nullptr; }
    if (g_Depth) { g_Depth->Release(); g_Depth = nullptr; }
}

// --------------------------------- 파이프라인(셰이더/상태) ---------------------------------
void CreatePipeline()
{
    // VS/PS 소스(문자열) — 수업 편의상 코드 내 삽입
    const char* HLSL_VS = R"(
        cbuffer CBFrame : register(b0)
        {
            matrix World;
            matrix View;
            matrix Proj;
            float3 LightDirWS;
            float  _pad0;
            float3 EyePosWS;
            float  _pad1;
        };
        struct VS_IN { float3 pos:POSITION; float3 n:NORMAL; float3 color:COLOR; };
        struct VS_OUT{ float4 sv:SV_POSITION; float3 nWS:NORMAL; float3 color:COLOR; float3 wpos:TEXCOORD0; };

        VS_OUT main(VS_IN i)
        {
            VS_OUT o;
            float4 wpos = mul(float4(i.pos,1), World);
            o.sv   = mul(mul(wpos, View), Proj);     // 클립공간
            o.nWS  = mul(float4(i.n,0), World).xyz;  // 월드공간 법선(비정규화)
            o.color= i.color;
            o.wpos = wpos.xyz;
            return o;
        }
    )";

    const char* HLSL_PS = R"(
        cbuffer CBFrame : register(b0)
        {
            matrix World;
            matrix View;
            matrix Proj;
            float3 LightDirWS;
            float  _pad0;
            float3 EyePosWS;
            float  _pad1;
        };
        struct PS_IN { float4 sv:SV_POSITION; float3 nWS:NORMAL; float3 color:COLOR; float3 wpos:TEXCOORD0; };

        float4 main(PS_IN i) : SV_Target
        {
            float3 N = normalize(i.nWS);
            float3 L = normalize(-LightDirWS);       // 빛이 오는 방향 (라이트→표면)
            float NdotL = max(dot(N,L), 0.0);

            float3 base = i.color;
            float3 ambient = 0.15 * base;
            float3 diffuse = NdotL * base;

            return float4(ambient + diffuse, 1.0);
        }
    )";

    // 컴파일
    ID3DBlob* vsb = nullptr,* psb = nullptr,* err = nullptr;
    HR(D3DCompile(HLSL_VS, strlen(HLSL_VS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err));
    HR(D3DCompile(HLSL_PS, strlen(HLSL_PS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &err));

    HR(g_Device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_VS));
    HR(g_Device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_PS));

    // 입력 레이아웃 (POSITION, NORMAL, COLOR)  [9]
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 12, D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 24, D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    HR(g_Device->CreateInputLayout(il, _countof(il), vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_InputLayout));
    vsb->Release(); psb->Release();

    // 래스터라이저(백페이스 컬링)  [10]
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_BACK; rs.DepthClipEnable = TRUE;
    HR(g_Device->CreateRasterizerState(&rs, &g_RS));

    // 깊이 스텐실 상태 (기본: less)
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_LESS;
    HR(g_Device->CreateDepthStencilState(&dsd, &g_DSS));
}

// --------------------------------- 큐브 생성 ---------------------------------
void CreateCube()
{
    // 24 정점(6면 × 4 정점), 각 면별 고정 법선과 색상
    VertexPNT v[] = {
        // +X (빨강)
        {{+1,-1,-1},{+1,0,0},{1,0,0}}, {{+1,-1,+1},{+1,0,0},{1,0,0}},
        {{+1,+1,+1},{+1,0,0},{1,0,0}}, {{+1,+1,-1},{+1,0,0},{1,0,0}},
        // -X (시안)
        {{-1,-1,+1},{-1,0,0},{0,1,1}}, {{-1,-1,-1},{-1,0,0},{0,1,1}},
        {{-1,+1,-1},{-1,0,0},{0,1,1}}, {{-1,+1,+1},{-1,0,0},{0,1,1}},
        // +Y (초록)
        {{-1,+1,-1},{0,+1,0},{0,1,0}}, {{+1,+1,-1},{0,+1,0},{0,1,0}},
        {{+1,+1,+1},{0,+1,0},{0,1,0}}, {{-1,+1,+1},{0,+1,0},{0,1,0}},
        // -Y (마젠타)
        {{-1,-1,+1},{0,-1,0},{1,0,1}}, {{+1,-1,+1},{0,-1,0},{1,0,1}},
        {{+1,-1,-1},{0,-1,0},{1,0,1}}, {{-1,-1,-1},{0,-1,0},{1,0,1}},
        // +Z (파랑)
        {{+1,-1,+1},{0,0,+1},{0,0,1}}, {{-1,-1,+1},{0,0,+1},{0,0,1}},
        {{-1,+1,+1},{0,0,+1},{0,0,1}}, {{+1,+1,+1},{0,0,+1},{0,0,1}},
        // -Z (노랑)
        {{-1,-1,-1},{0,0,-1},{1,1,0}}, {{+1,-1,-1},{0,0,-1},{1,1,0}},
        {{+1,+1,-1},{0,0,-1},{1,1,0}}, {{-1,+1,-1},{0,0,-1},{1,1,0}},
    };
    uint16_t idx[] = {
        0,1,2, 0,2,3,     4,5,6, 4,6,7,
        8,9,10, 8,10,11,  12,13,14, 12,14,15,
        16,17,18, 16,18,19, 20,21,22, 20,22,23
    };

    // VB
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(v);
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{ v };
    HR(g_Device->CreateBuffer(&vbd, &vinit, &g_VB));

    // IB
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = sizeof(idx);
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit{ idx };
    HR(g_Device->CreateBuffer(&ibd, &iinit, &g_IB));

    // 상수버퍼 (b0)
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CBFrame);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HR(g_Device->CreateBuffer(&cbd, nullptr, &g_CBuffer));
}

// --------------------------------- 프레임 갱신/드로우 ---------------------------------
void UpdateAndDraw()
{
    // deltaTime
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - g_Prev.QuadPart) / double(g_Freq.QuadPart);
    g_Prev = now; g_Time += float(dt);

    // 카메라/행렬
    XMVECTOR eye = XMVectorSet(0.0f, 2.0f, -6.0f, 1.0f);  // 뒤에서 앞으로 보는 카메라
    XMVECTOR at = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX View = XMMatrixLookAtLH(eye, at, up);        // [11]

    float aspect = (float)g_Width / (float)g_Height;
    XMMATRIX Proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f); // [12]

    XMMATRIX World = XMMatrixRotationY(g_Time) * XMMatrixRotationX(g_Time * 1.5f); // 회전
    
    // 조명/카메라
    XMFLOAT3 lightDirWS = XMFLOAT3(-0.5f, -1.0f, -0.2f);
    XMFLOAT3 eyePosWS;
    XMStoreFloat3(&eyePosWS, eye);

    // 상수버퍼 업데이트 (주의: HLSL은 column-major, XMMatrix는 row-major 반환 → Transpose)
    CBFrame cb{};
    cb.World = XMMatrixTranspose(World);
    cb.View = XMMatrixTranspose(View);
    cb.Proj = XMMatrixTranspose(Proj);
    cb.LightDirWS = lightDirWS;
    cb.EyePosWS = eyePosWS;
    g_Context->UpdateSubresource(g_CBuffer, 0, nullptr, &cb, 0, 0);

    // 상태 바인딩
    g_Context->IASetInputLayout(g_InputLayout);
    UINT stride = sizeof(VertexPNT), offset = 0;
    g_Context->IASetVertexBuffers(0, 1, &g_VB, &stride, &offset);
    g_Context->IASetIndexBuffer(g_IB, DXGI_FORMAT_R16_UINT, 0);
    g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    g_Context->VSSetShader(g_VS, nullptr, 0);
    g_Context->PSSetShader(g_PS, nullptr, 0);
    g_Context->VSSetConstantBuffers(0, 1, &g_CBuffer);
    g_Context->PSSetConstantBuffers(0, 1, &g_CBuffer);

    g_Context->RSSetState(g_RS);
    g_Context->OMSetDepthStencilState(g_DSS, 0);
    g_Context->OMSetRenderTargets(1, &g_RTV, g_DSV);

    // 클리어
    const float clear[4] = { 0.07f, 0.09f, 0.12f, 1.0f };
    g_Context->ClearRenderTargetView(g_RTV, clear);
    g_Context->ClearDepthStencilView(g_DSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // 드로우
    g_Context->DrawIndexed(36, 0, 0);

    // 표시
    g_SwapChain->Present(1, 0);
}

// --------------------------------- 정리 ---------------------------------
void Cleanup()
{
    if (g_Sampler) { g_Sampler->Release(); g_Sampler = nullptr; }
    if (g_DSS) { g_DSS->Release(); g_DSS = nullptr; }
    if (g_RS) { g_RS->Release(); g_RS = nullptr; }

    if (g_CBuffer) { g_CBuffer->Release(); g_CBuffer = nullptr; }
    if (g_IB) { g_IB->Release(); g_IB = nullptr; }
    if (g_VB) { g_VB->Release(); g_VB = nullptr; }
    if (g_InputLayout) { g_InputLayout->Release(); g_InputLayout = nullptr; }
    if (g_VS) { g_VS->Release(); g_VS = nullptr; }
    if (g_PS) { g_PS->Release(); g_PS = nullptr; }

    DestroyBackbufferAndDSV();

    if (g_Context) { g_Context->Release(); g_Context = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
    if (g_SwapChain) { g_SwapChain->Release(); g_SwapChain = nullptr; }
}
// --------------------------------- 주석 ---------------------------------
/*
[1] 스왑체인(SwapChain) : 백버퍼 / 프론트버퍼 교체 메커니즘.Present()로 교체

[2] 디바이스(Device) : GPU 리소스(버퍼·텍스처·셰이더) 생성 담당

[3] 컨텍스트(Context) : GPU 파이프라인에 명령 제출

[4] RTV(RenderTargetView) : 색을 쓰는 표면으로 백버퍼를 해석하는 뷰

[5] 깊이 텍스처(Depth Texture) : Z값 저장용 2D 텍스처(D24S8 사용)

[6] DSV(DepthStencilView) : 깊이 / 스텐실 텍스처를 파이프라인에 바인딩하는 뷰

[7] VertexPNT : POSITION / NORMAL / COLOR 정점 포맷. 3D 조명용 법선 포함

[8] DXGI_SWAP_CHAIN_DESC : 해상도, 포맷, 플립 모델, 버퍼 수 등을 정의

[9] 입력 레이아웃 : CPU 구조체와 HLSL 시그니처 매칭(SEMANTIC + 오프셋)

[10] 래스터라이저 상태 : 컬링 / 필모드 / DepthClip 등 설정

[11] View 행렬 : 카메라 위치 / 방향으로 월드→뷰 좌표 변환

[12] Projection 행렬 : 원근 투영(FOV / 종횡비 / Near / Far)
*/