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

ComPtr<ID3D12DescriptorHeap> rtvHeap; // a heap to store descriptors
UINT rtvDescriptorSize = 0; // size of a single descriptor on GPU
ComPtr<ID3D12Resource> renderTargets[2]; // double buffering
UINT currentBackBuffer = 0;

// synchronization
ComPtr<ID3D12Fence> fence;
UINT64 fenceValue = 0;
HANDLE fenceEvent; // to tell CPU to wait for GPU


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
	CloseHandle(fenceEvent);
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
	g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
	rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

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

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT n = 0; n < 2; n++) 
	{
		g_swapChain->GetBuffer(n, IID_PPV_ARGS(&renderTargets[n]));
		g_device->CreateRenderTargetView(renderTargets[n].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += rtvDescriptorSize;
	}

	// create command allocator and command list
	g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator));
	g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList));

	// command lists are created in the recording state, close it for now and reset later
	g_commandList->Close();

	// create synchronization objects
	g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	fenceValue = 1;
	fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); // create a window event object

	if (fenceEvent == nullptr) 
	{
		// failed to create event
	}
}

void PopulateCommandList()
{
	// reset command allocator and command list
	g_commandAllocator->Reset();
	g_commandList->Reset(g_commandAllocator.Get(), nullptr); // no pso yet so pass null

	// tell gpu that we will draw to it now by transitioning the back buffer from
	// present state to a render target state
	// manual barrier creation
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = renderTargets[currentBackBuffer].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	g_commandList->ResourceBarrier(1, &barrier);

	// get the handle for the current back buffer manually and set it as the render target
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtvHandle.ptr += (SIZE_T)currentBackBuffer * (SIZE_T)rtvDescriptorSize;

	g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// issue commands to clear the render target
	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

	// transition the back buffer back to a present state
	D3D12_RESOURCE_BARRIER barrier2 = {};
	barrier2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier2.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier2.Transition.pResource = renderTargets[currentBackBuffer].Get();
	barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	barrier2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	g_commandList->ResourceBarrier(1, &barrier2);
	g_commandList->Close();
}

void WaitForPreviousFrame() 
{
	// signal the fence with the current value
	const UINT64 fenceUint = fenceValue;
	g_commandQueue->Signal(fence.Get(), fenceUint);
	fenceValue++;

	// wait for the previous frame if gpu is not done
	if(fence->GetCompletedValue() < fenceUint)
	{
		fence->SetEventOnCompletion(fenceUint, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}

	// update the index of the current back buffer
	currentBackBuffer = g_swapChain->GetCurrentBackBufferIndex();
}

