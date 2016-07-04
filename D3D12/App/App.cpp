#include "App.h"

#include <DirectXColors.h>
#include <tbb/parallel_for.h>
#include <vector>
#include <WindowsX.h>

#include <Camera/Camera.h>
#include <CommandManager/CommandManager.h>
#include <GlobalData\D3dData.h>
#include <Input/Keyboard.h>
#include <Input/Mouse.h>
#include <MathUtils/MathHelper.h>
#include <PSOManager\PSOManager.h>
#include <ResourceManager\ResourceManager.h>
#include <RootSignatureManager\RootSignatureManager.h>
#include <ShaderManager\ShaderManager.h>
#include <Utils\DebugUtils.h>

namespace {
	const std::uint32_t MAX_NUM_CMD_LISTS{ 3U };
}

App* App::mApp = nullptr;

LRESULT CALLBACK
MainWndProc(HWND hwnd, const std::uint32_t msg, WPARAM wParam, LPARAM lParam) {
	return App::GetApp()->MsgProc(hwnd, msg, wParam, lParam);
}

App::App(HINSTANCE hInstance)
	: mTaskSchedulerInit()
	, mAppInst(hInstance)
{
	ASSERT(mApp == nullptr);
	mApp = this;

	mMasterRenderTaskParent = MasterRenderTask::Create(mMasterRenderTask);
}

App::~App() {
	ASSERT(D3dData::mDevice.Get() != nullptr);
	//FlushCommandQueue();

	mMasterRenderTask->Terminate();
	mTaskSchedulerInit.terminate();

	// We must change swap chain to windowed before release
	//D3dData::mSwapChain->SetFullscreenState(false, nullptr);
}

void App::InitializeTasks() noexcept {
	ASSERT(!mInitTasks.empty());
	ASSERT(mInitTasks.size() == mCmdBuilderTasks.size());

	tbb::concurrent_queue<ID3D12CommandList*>& cmdListQueue{ mCmdListProcessor->CmdListQueue() };
	ASSERT(mCmdListProcessor->IsIdle());

	tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mInitTasks.size()),
		[&](const tbb::blocked_range<size_t>& r) {
		for (size_t i = r.begin(); i != r.end(); ++i)
			mInitTasks[i]->Execute(*D3dData::mDevice.Get(), cmdListQueue, mCmdBuilderTasks[i]->TaskInput());
	}
	);

	while (!mCmdListProcessor->IsIdle()) {
		Sleep(0U);
	}

	FlushCommandQueue();

	const std::uint64_t count{ _countof(mFenceByFrameIndex) };
	for (std::uint64_t i = 0UL; i < count; ++i) {
		mFenceByFrameIndex[i] = mCurrentFence;
	}
}

int32_t App::Run() noexcept {
	ASSERT(Keyboard::gKeyboard.get() != nullptr);
	ASSERT(Mouse::gMouse.get() != nullptr);
	ASSERT(mMasterRenderTaskParent != nullptr);
	ASSERT(mMasterRenderTask != nullptr);
	mMasterRenderTask->Init(mHwnd);
	mMasterRenderTaskParent->spawn(*mMasterRenderTask);

	MSG msg{0U};

	mTimer.Reset();

	while (msg.message != WM_QUIT) 	{
		// If there are Window messages then process them.
		if (PeekMessage(&msg, 0U, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		// Otherwise, do animation/game stuff.
		else {
			mTimer.Tick();

			if (!mAppPaused) {
				const float dt = mTimer.DeltaTime();
				Keyboard::gKeyboard->Update();
				Mouse::gMouse->Update();
				Update(dt);
				//Draw(dt);			
			}
			else {
				Sleep(100U);
			}
		}
	}

	return (int)msg.wParam;
}

void App::Initialize() noexcept {
	InitMainWindow();
	InitDirect3D();	

	Camera::gCamera->UpdateViewMatrix();

	// Create command list processor thread.
	//CommandListProcessor::Create(mCmdListProcessor, mCmdQueue, MAX_NUM_CMD_LISTS)->spawn(*mCmdListProcessor);
	//tbb::empty_task* parent{ MasterRenderTask::Create(mMasterRenderTask) };
}

void App::CreateRtvAndDsvDescriptorHeaps() noexcept {
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = Settings::sSwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ResourceManager::gManager->CreateDescriptorHeap(rtvHeapDesc, mRtvHeap);

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1U;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0U;
	ResourceManager::gManager->CreateDescriptorHeap(dsvHeapDesc, mDsvHeap);
}

void App::Update(const float dt) noexcept {
	static std::int32_t lastXY[]{ 0UL, 0UL };
	static const float sCameraOffset{ 10.0f };
	static const float sCameraMultiplier{ 5.0f };

	ASSERT(Keyboard::gKeyboard.get() != nullptr);
	ASSERT(Mouse::gMouse.get() != nullptr);

	// If we executed command lists for all frames, then we need to wait
	// at least 1 of them to be completed, before continue generating command list for a frame. 
	/*const std::uint32_t currBackBuffer{ D3dData::CurrentBackBufferIndex() };
	const std::uint64_t fence{ mFenceByFrameIndex[currBackBuffer] };
	const std::uint64_t completedFenceValue{ mFence->GetCompletedValue() };
	if (completedFenceValue < fence)
	{
		const HANDLE eventHandle{ CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS) };
		ASSERT(eventHandle);

		// Fire event when GPU hits current fence.  
		CHECK_HR(mFence->SetEventOnCompletion(fence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		const std::uint64_t newCompletedFenceValue{ mFence->GetCompletedValue() };
		CloseHandle(eventHandle);
	}*/

	// Update camera based on keyboard 
	const float offset = sCameraOffset * (Keyboard::gKeyboard->IsKeyDown(DIK_LSHIFT) ? sCameraMultiplier : 1.0f) * dt ;
	if (Keyboard::gKeyboard->IsKeyDown(DIK_W)) {
		Camera::gCamera->Walk(offset);
	}
	if (Keyboard::gKeyboard->IsKeyDown(DIK_S)) {
		Camera::gCamera->Walk(-offset);
	}
	if (Keyboard::gKeyboard->IsKeyDown(DIK_A)) {
		Camera::gCamera->Strafe(-offset);
	}
	if (Keyboard::gKeyboard->IsKeyDown(DIK_D)) {
		Camera::gCamera->Strafe(offset);
	}

	// Update camera based on mouse
	const std::int32_t x{ Mouse::gMouse->X() };
	const std::int32_t y{ Mouse::gMouse->Y() };
	if (Mouse::gMouse->IsButtonDown(Mouse::MouseButtonsLeft)) {
		// Make each pixel correspond to a quarter of a degree.+
		const float dx{ DirectX::XMConvertToRadians(0.25f * (float)(x - lastXY[0])) };
		const float dy{DirectX::XMConvertToRadians(0.25f * (float)(y - lastXY[1])) };

		Camera::gCamera->Pitch(dy);
		Camera::gCamera->RotateY(dx);
	}

	lastXY[0] = x;
	lastXY[1] = y;

	Camera::gCamera->UpdateViewMatrix();
}

void App::Draw(const float) noexcept {
	tbb::concurrent_queue<ID3D12CommandList*>& cmdListQueue{ mCmdListProcessor->CmdListQueue() };
	ASSERT(mCmdListProcessor->IsIdle());

	// Begin Frame task + # cmd build tasks
	const std::uint32_t taskCount{ (std::uint32_t)mCmdBuilderTasks.size() + 1U };
	mCmdListProcessor->resetExecutedTasksCounter();

	const std::uint32_t currBackBuffer{ D3dData::CurrentBackBufferIndex() };
	ID3D12CommandAllocator* cmdAllocFrameBegin{ mCmdAllocFrameBegin[currBackBuffer] };
	ID3D12CommandAllocator* cmdAllocFrameEnd{ mCmdAllocFrameEnd[currBackBuffer] };

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	CHECK_HR(cmdAllocFrameBegin->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	CHECK_HR(mCmdListFrameBegin->Reset(cmdAllocFrameBegin, nullptr));

	// Set the viewport and scissor rect.  This needs to be reset whenever the command list is reset.
	mCmdListFrameBegin->RSSetViewports(1U, &Settings::sScreenViewport);
	mCmdListFrameBegin->RSSetScissorRects(1U, &Settings::sScissorRect);

	// Indicate a state transition on the resource usage.
	CD3DX12_RESOURCE_BARRIER resBarrier{ CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET) };
	mCmdListFrameBegin->ResourceBarrier(1, &resBarrier);

	// Specify the buffers we are going to render to.
	const D3D12_CPU_DESCRIPTOR_HANDLE backBufferHandle = CurrentBackBufferView();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = DepthStencilView();
	mCmdListFrameBegin->OMSetRenderTargets(1U, &backBufferHandle, true, &dsvHandle);

	// Clear the back buffer and depth buffer.
	mCmdListFrameBegin->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0U, nullptr);
	mCmdListFrameBegin->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0U, 0U, nullptr);

	// Done recording commands.
	CHECK_HR(mCmdListFrameBegin->Close());
	cmdListQueue.push(mCmdListFrameBegin);

	tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mCmdBuilderTasks.size()),
		[&](const tbb::blocked_range<size_t>& r) {
		for (size_t i = r.begin(); i != r.end(); ++i)
			mCmdBuilderTasks[i]->Execute(cmdListQueue, currBackBuffer, backBufferHandle, dsvHandle);
	}
	);
	
	CHECK_HR(cmdAllocFrameEnd->Reset());
	
	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	CHECK_HR(mCmdListFrameEnd->Reset(cmdAllocFrameEnd, nullptr));	

	// Indicate a state transition on the resource usage.
	resBarrier = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	mCmdListFrameEnd->ResourceBarrier(1U, &resBarrier);

	// Done recording commands.
	CHECK_HR(mCmdListFrameEnd->Close());

	// Wait until all previous tasks command lists are executed, before
	// executing frame end command list
	while (mCmdListProcessor->ExecutedTasksCounter() < taskCount) {
		Sleep(0U);
	}

	{
		ID3D12CommandList* cmdLists[] = { mCmdListFrameEnd };		
		mCmdQueue->ExecuteCommandLists(1, cmdLists);
	}

	SignalFenceAndPresent();
}

void App::CreateRtvAndDsv() noexcept {
	ASSERT(D3dData::mDevice != nullptr);
	ASSERT(D3dData::mSwapChain != nullptr);

	// Setup RTV descriptor to specify sRGB format.
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = Settings::sRTVFormats[0U];
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	const std::uint32_t rtvDescSize{ D3dData::mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };

	for (std::uint32_t i = 0U; i < Settings::sSwapChainBufferCount; ++i) {
		CHECK_HR(D3dData::mSwapChain->GetBuffer(i, IID_PPV_ARGS(mSwapChainBuffer[i].GetAddressOf())));
		D3dData::mDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), &rtvDesc, rtvHeapHandle);
		rtvHeapHandle.Offset(1U, rtvDescSize);
	}

	// Create the depth/stencil buffer and view.
	D3D12_RESOURCE_DESC depthStencilDesc = {};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0U;
	depthStencilDesc.Width = Settings::sWindowWidth;
	depthStencilDesc.Height = Settings::sWindowHeight;
	depthStencilDesc.DepthOrArraySize = 1U;
	depthStencilDesc.MipLevels = 1U;
	depthStencilDesc.Format = Settings::sDepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = 1U;
	depthStencilDesc.SampleDesc.Quality = 0U;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format = Settings::sDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0U;
	CD3DX12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
	ResourceManager::gManager->CreateCommittedResource(heapProps, D3D12_HEAP_FLAG_NONE, depthStencilDesc, D3D12_RESOURCE_STATE_COMMON, optClear, mDepthStencilBuffer);

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	D3dData::mDevice->CreateDepthStencilView(mDepthStencilBuffer, nullptr, DepthStencilView());
}

LRESULT App::MsgProc(HWND hwnd, const int32_t msg, WPARAM wParam, LPARAM lParam) noexcept {
	switch (msg) {
		// WM_ACTIVATE is sent when the window is activated or deactivated.  
		// We pause the game when the window is deactivated and unpause it 
		// when it becomes active.  
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE) {
			mAppPaused = true;
			mTimer.Stop();
		}
		else {
			mAppPaused = false;
			mTimer.Start();
		}
		return 0;

		// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
	case WM_ENTERSIZEMOVE:
		mAppPaused = true;
		mTimer.Stop();
		return 0;

		// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
		// Here we reset everything based on the new window dimensions.
	case WM_EXITSIZEMOVE:
		mAppPaused = false;
		mTimer.Start();
		return 0;

		// WM_DESTROY is sent when the window is being destroyed.
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

		// The WM_MENUCHAR message is sent when a menu is active and the user presses 
		// a key that does not correspond to any mnemonic or accelerator key. 
	case WM_MENUCHAR:
		// Don't beep when we alt-enter.
		return MAKELRESULT(0, MNC_CLOSE);

		// Catch this message so to prevent the window from becoming too small.
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
		((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
		return 0;
	case WM_KEYUP:
		if (wParam == VK_ESCAPE) {
			PostQuitMessage(0);
		}

		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void App::InitSystems() noexcept {
	ASSERT(Camera::gCamera.get() == nullptr);
	Camera::gCamera = std::make_unique<Camera>();
	Camera::gCamera->SetLens(0.25f * MathHelper::Pi, Settings::AspectRatio(), 1.0f, 1000.0f);

	ASSERT(Keyboard::gKeyboard.get() == nullptr);
	LPDIRECTINPUT8 directInput;
	CHECK_HR(DirectInput8Create(mAppInst, DIRECTINPUT_VERSION, IID_IDirectInput8, (LPVOID*)&directInput, nullptr));
	Keyboard::gKeyboard = std::make_unique<Keyboard>(*directInput, mHwnd);

	ASSERT(Mouse::gMouse.get() == nullptr);
	Mouse::gMouse = std::make_unique<Mouse>(*directInput, mHwnd);
	
	ASSERT(CommandManager::gManager.get() == nullptr);
	CommandManager::gManager = std::make_unique<CommandManager>(*D3dData::mDevice.Get());

	ASSERT(PSOManager::gManager.get() == nullptr);
	PSOManager::gManager = std::make_unique<PSOManager>(*D3dData::mDevice.Get());

	ASSERT(ResourceManager::gManager.get() == nullptr);
	ResourceManager::gManager = std::make_unique<ResourceManager>(*D3dData::mDevice.Get());

	ASSERT(RootSignatureManager::gManager.get() == nullptr);
	RootSignatureManager::gManager = std::make_unique<RootSignatureManager>(*D3dData::mDevice.Get());

	ASSERT(ShaderManager::gManager.get() == nullptr);
	ShaderManager::gManager = std::make_unique<ShaderManager>();
}

void App::InitMainWindow() noexcept {
	WNDCLASS wc = {};
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = MainWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = mAppInst;
	wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";

	ASSERT(RegisterClass(&wc));

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT r = { 0, 0, Settings::sWindowWidth, Settings::sWindowHeight };
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, false);
	const int32_t width{ r.right - r.left };
	const int32_t height{ r.bottom - r.top };

	//const std::uint32_t dwStyle = { WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX };
	const std::uint32_t dwStyle = { WS_POPUP };
	mHwnd = CreateWindowEx(WS_EX_APPWINDOW, L"MainWnd", L"App", dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, mAppInst, 0);
	ASSERT(mHwnd);

	ShowWindow(mHwnd, SW_SHOW);
	UpdateWindow(mHwnd);
}

void App::InitDirect3D() noexcept {
	D3dData::InitDirect3D();

	InitSystems();
	
	/*ResourceManager::gManager->CreateFence(0U, D3D12_FENCE_FLAG_NONE, mFence);
	CreateCommandObjects();
	D3dData::CreateSwapChain(mHwnd, *mCmdQueue);
	CreateRtvAndDsvDescriptorHeaps();
	CreateRtvAndDsv();*/
}

void App::CreateCommandObjects() noexcept {
	ASSERT(Settings::sSwapChainBufferCount > 0U);

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	CommandManager::gManager->CreateCmdQueue(queueDesc, mCmdQueue);

	for (std::uint32_t i = 0U; i < Settings::sSwapChainBufferCount; ++i) {
		CommandManager::gManager->CreateCmdAlloc(D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAllocFrameBegin[i]);
		CommandManager::gManager->CreateCmdAlloc(D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAllocFrameEnd[i]);
	}
	CommandManager::gManager->CreateCmdList(D3D12_COMMAND_LIST_TYPE_DIRECT, *mCmdAllocFrameBegin[0], mCmdListFrameBegin);
	CommandManager::gManager->CreateCmdList(D3D12_COMMAND_LIST_TYPE_DIRECT, *mCmdAllocFrameEnd[0], mCmdListFrameEnd);

	// Start off in a closed state.  This is because the first time we refer 
	// to the command list we will Reset it, and it needs to be closed before
	// calling Reset.
	mCmdListFrameBegin->Close();
	mCmdListFrameEnd->Close();
}

void App::FlushCommandQueue() noexcept {
	++mCurrentFence;

	CHECK_HR(mCmdQueue->Signal(mFence, mCurrentFence));
	
	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence) {
		const HANDLE eventHandle{ CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS) };
		ASSERT(eventHandle);

		// Fire event when GPU hits current fence.  
		CHECK_HR(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void App::SignalFenceAndPresent() noexcept {
	ASSERT(D3dData::mSwapChain.Get());
	CHECK_HR(D3dData::mSwapChain->Present(0U, 0U));

	// Advance the fence value to mark commands up to this fence point.
	const std::uint32_t currBackBuffer{ D3dData::CurrentBackBufferIndex() };
	mFenceByFrameIndex[currBackBuffer] = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	CHECK_HR(mCmdQueue->Signal(mFence, mCurrentFence));
}

ID3D12Resource* App::CurrentBackBuffer() const noexcept {
	const std::uint32_t currBackBuffer{ D3dData::mSwapChain->GetCurrentBackBufferIndex() };
	return mSwapChainBuffer[currBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE App::CurrentBackBufferView() const noexcept {
	const std::uint32_t rtvDescSize{ D3dData::mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
	const std::uint32_t currBackBuffer{ D3dData::CurrentBackBufferIndex() };
	return D3D12_CPU_DESCRIPTOR_HANDLE{ mRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + currBackBuffer * rtvDescSize };
}

D3D12_CPU_DESCRIPTOR_HANDLE App::DepthStencilView() const noexcept {
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}