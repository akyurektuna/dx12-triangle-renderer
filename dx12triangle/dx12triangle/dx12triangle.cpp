#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

const UINT WindowWidth = 1280;
const UINT WindowHeight = 720;

HWND hWnd;

ComPtr<ID3D12Device> g_device; // gpu
ComPtr<IDXGISwapChain3> g_swapChain; // back buffering
ComPtr<ID3D12CommandQueue> g_commandQueue; // submit commands for the GPU to execute
ComPtr<ID3D12CommandAllocator> g_commandAllocator; // memory for a batch of commands
ComPtr<ID3D12GraphicsCommandList> g_commandList;

ComPtr<ID3D12DescriptorHeap> g_rtvHeap; // a heap to store descriptors
UINT g_rtvDescriptorSize = 0; // size of a single descriptor on GPU
ComPtr<ID3D12Resource> g_renderTargets[2]; // double buffering
UINT g_currentBackBuffer = 0;

// synchronization
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue = 0;
HANDLE g_fenceEvent; // to tell CPU to wait for GPU

ComPtr<ID3D12RootSignature> g_rootSignature; // defines resources shaders need
ComPtr<ID3D12PipelineState> g_pipelineState;

// simple shaders
const char* g_VertexShader = R"(
    struct VS_INPUT
    {
        float3 pos : POSITION;
        float4 col : COLOR;
    };
    struct PS_INPUT
    {
        float4 pos : SV_POSITION;
        float4 col : COLOR;
    };
    PS_INPUT main(VS_INPUT input)
    {
        PS_INPUT output;
        output.pos = float4(input.pos, 1.0f); // transform by identity 
        output.col = input.col;
        return output;
    }
)";

const char* g_PixelShader = R"(
    struct PS_INPUT
    {
        float4 pos : SV_POSITION;
        float4 col : COLOR;
    };
    float4 main(PS_INPUT input) : SV_Target
    {
        return input.col;
    }
)";

ComPtr<ID3D12Resource> g_vertexBuffer;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView; 

struct Vertex {
	float position[3];
	float color[4];
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

void InitD3D();
void PopulateCommandList();
void WaitForPreviousFrame();

// main entry point for windows applications
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) 
{
	// register
	const wchar_t CLASS_NAME[] = L"dx12 window class";

	WNDCLASS wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;

	RegisterClass(&wc);

	// create
	hWnd = CreateWindowEx(
		0,
		CLASS_NAME,
		L"dx12 hello world",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, WindowWidth, WindowHeight,
		NULL, NULL, hInstance, NULL
	);

	if (hWnd == NULL) 
	{
		return 0;
	}

	// initialize direct3d
	InitD3D();

	ShowWindow(hWnd, nCmdShow);

	// main loop
	MSG msg = {};
	while (msg.message != WM_QUIT) 
	{
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) 
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else 
		{
			PopulateCommandList();
			ID3D12CommandList* commandLists[] = { g_commandList.Get() };
			g_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
			g_swapChain->Present(1, 0);
			WaitForPreviousFrame();
		}
	}

	// cleanup done by comptr
	CloseHandle(g_fenceEvent);
	return 0;
}

// handles messages from the OS
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch(message)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}


void CreatePipelineStateObject()
{
	HRESULT hr;

	// compile shaders
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	ComPtr<ID3DBlob> errorBuffer;

	hr = D3DCompile(g_VertexShader, strlen(g_VertexShader), nullptr, nullptr,
		nullptr, "main", "vs_5_0", 0, 0, &vertexShader, &errorBuffer);

	if (FAILED(hr))
	{
		MessageBoxA(0, (char*)errorBuffer->GetBufferPointer(), "Vertex Shader Compile Error", MB_OK);
		exit(1);
	}

	hr = D3DCompile(g_PixelShader, strlen(g_PixelShader), nullptr, nullptr,
		nullptr, "main", "ps_5_0", 0, 0, &pixelShader, &errorBuffer);

	if (FAILED(hr))
	{
		MessageBoxA(0, (char*)errorBuffer->GetBufferPointer(), "Pixel Shader Compile Error", MB_OK);
		exit(1);
	}

	// create a root signature
	// shaders do not use any resources yet

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
	rootSignatureDesc.NumParameters = 0;
	rootSignatureDesc.pParameters = nullptr;
	rootSignatureDesc.NumStaticSamplers = 0;
	rootSignatureDesc.pStaticSamplers = nullptr;
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> signature;
	D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr);
	g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

	// define vertex input layout
	D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	// create pso
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.pRootSignature = g_rootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };

	// manual Rasterizer State
	D3D12_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
	rasterizerDesc.FrontCounterClockwise = FALSE;
	rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	rasterizerDesc.DepthClipEnable = TRUE;
	rasterizerDesc.MultisampleEnable = FALSE;
	rasterizerDesc.AntialiasedLineEnable = FALSE;
	rasterizerDesc.ForcedSampleCount = 0;
	psoDesc.RasterizerState = rasterizerDesc;

	// manual Blend State
	D3D12_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.IndependentBlendEnable = FALSE;
	const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
		FALSE, FALSE,
		D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
		D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
		D3D12_LOGIC_OP_NOOP,
		D3D12_COLOR_WRITE_ENABLE_ALL,
	};
	for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;
	}
	psoDesc.BlendState = blendDesc;

	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // Must match swap chain format
	psoDesc.SampleDesc.Count = 1;

	hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState));
	if (FAILED(hr))
	{
		MessageBox(nullptr, L"Failed to create Pipeline State Object!", L"Error", MB_OK);
		exit(1);
	}

}

void CreateAssets()
{
	HRESULT hr;

	Vertex triangleVertices[] = {
		// bottom-left vertex - red
		{ { -0.5f, -0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
		// bottom-right vertex - green
		{ { 0.0f, 0.5f, 0.0f },  { 0.0f, 1.0f, 0.0f, 1.0f } },
		// top vertex - blue
		{ { 0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
	};

	const UINT vertexBufferSize = sizeof(triangleVertices);

	// create vertex buffer resource on te gpu (default heap)
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Alignment = 0;
	resDesc.Width = vertexBufferSize;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	g_device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&g_vertexBuffer)
	);

	// temporary upload heap to get data to the gpu
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	ComPtr<ID3D12Resource> vertexBufferUpload;
	g_device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertexBufferUpload)
	);

	// copy data to the upload heap, then schedule a copy to the default heap
	void* data;
	vertexBufferUpload->Map(0, nullptr, &data);
	memcpy(data, triangleVertices, vertexBufferSize);
	vertexBufferUpload->Unmap(0, nullptr);

	g_commandList->Reset(g_commandAllocator.Get(), nullptr);
	g_commandList->CopyResource(g_vertexBuffer.Get(), vertexBufferUpload.Get());
	g_commandList->Close();

	ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
	g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	g_commandList->Reset(g_commandAllocator.Get(), nullptr); // reset the command list for the barrier

	// barrier to transition the vertex buffer from COPY_DEST to VERTEX_AND_CONSTANT_BUFFER
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_vertexBuffer.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER; // State needed for Draw()
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	g_commandList->ResourceBarrier(1, &barrier);
	g_commandList->Close();

	// execute the command list to transition the resource state
	ID3D12CommandList* ppCommandListsTransition[] = { g_commandList.Get() };
	g_commandQueue->ExecuteCommandLists(_countof(ppCommandListsTransition), ppCommandListsTransition);

	WaitForPreviousFrame();
	g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();

	g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();
	g_vertexBufferView.StrideInBytes = sizeof(Vertex);
	g_vertexBufferView.SizeInBytes = vertexBufferSize;
}

// setup directx objects
void InitD3D()
{
	UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	// create the device
	ComPtr<IDXGIFactory4> factory;
	CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));

	ComPtr<IDXGIAdapter1> hwAdapter;
	factory->EnumAdapters1(0, &hwAdapter); // get the first default adapter
	
	D3D12CreateDevice(hwAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));

	// create command queue
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

	// AFTER the queue create the swap chain

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = 2;
	swapChainDesc.Width = WindowWidth;
	swapChainDesc.Height = WindowHeight;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChainLocal;
	factory->CreateSwapChainForHwnd(
		g_commandQueue.Get(),
		hWnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChainLocal
	);

	swapChainLocal.As(&g_swapChain);

	// create a descriptor heap for RTVs
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 2;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
	g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// create RTVs for the back buffers
	// 
	/*
		instead of using d3dx12.h doing the calculations with manual pointer arithmetic
		
		>> d3d12_cpu_descriptor_Handle is a pointer to a location in cpu that describes
		a resource to the gpu
		>> ptr is the actual memory address
		>> rtv descriptor size tells us how many bytes a single descriptor takes up in
		the heap
		>> to get the descriptor for the nth buffer we start at the beginning of the heap
		and add n * rtv descriptor size bytes to the pointer
	*/

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT n = 0; n < 2; n++) 
	{
		g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
		g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += g_rtvDescriptorSize;
	}

	// create command allocator and command list
	g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator));
	g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList));

	// command lists are created in the recording state, close it for now and reset later
	g_commandList->Close();

	// create synchronization objects
	g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
	g_fenceValue = 1;
	g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); // create a window event object

	if (g_fenceEvent == nullptr)
	{
		// failed to create event
	}

	CreatePipelineStateObject();
	CreateAssets();
}

void PopulateCommandList()
{
	// reset command allocator and command list
	g_commandAllocator->Reset();
	g_commandList->Reset(g_commandAllocator.Get(), g_pipelineState.Get()); // no pso yet so pass null

	// tell gpu that we will draw to it now by transitioning the back buffer from
	// present state to a render target state
	// manual barrier creation
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_renderTargets[g_currentBackBuffer].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	g_commandList->ResourceBarrier(1, &barrier);

	// get the handle for the current back buffer manually and set it as the render target
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtvHandle.ptr += (SIZE_T)g_currentBackBuffer * (SIZE_T)g_rtvDescriptorSize;

	g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	D3D12_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(WindowWidth);
	viewport.Height = static_cast<float>(WindowHeight);
	viewport.MaxDepth = 1.0f;
	g_commandList->RSSetViewports(1, &viewport);

	D3D12_RECT scissorRect = {};
	scissorRect.right = WindowWidth;
	scissorRect.bottom = WindowHeight;
	g_commandList->RSSetScissorRects(1, &scissorRect);

	// issue commands to clear the render target
	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

	g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
	g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_commandList->IASetVertexBuffers(0, 1, &g_vertexBufferView);
	g_commandList->DrawInstanced(3, 1, 0, 0);

	// transition the back buffer back to a present state
	D3D12_RESOURCE_BARRIER barrier2 = {};
	barrier2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier2.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier2.Transition.pResource = g_renderTargets[g_currentBackBuffer].Get();
	barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	barrier2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	g_commandList->ResourceBarrier(1, &barrier2);
	g_commandList->Close();
}

void WaitForPreviousFrame() 
{
	// signal the fence with the current value
	const UINT64 fence = g_fenceValue;
	g_commandQueue->Signal(g_fence.Get(), fence);
	g_fenceValue++;

	// wait for the previous frame if gpu is not done
	if(g_fence->GetCompletedValue() < fence)
	{
		g_fence->SetEventOnCompletion(fence, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, INFINITE);
	}

	// update the index of the current back buffer
	g_currentBackBuffer = g_swapChain->GetCurrentBackBufferIndex();
}

