
#include "Profiler.h"

#if WITH_PROFILING

#define NOMINMAX
#include <d3d12.h>
#include <dxgi.h>

Profiler gProfiler;
GPUProfiler gGPUProfiler;

class SubAllocator
{
public:
	const char* String(const char* pStr, uint32 id)
	{
		uint32 len	 = (uint32)strlen(pStr) + 1;
		char*  pData = (char*)Allocate(len, id);
		strcpy_s(pData, len, pStr);
		return pData;
	}

	void* Allocate(uint32 size, uint32 id)
	{
		if (pPage == nullptr || id > ID || !gProfiler.m_Allocator.IsValidPage(ID) || Offset + size > pPage->Size)
		{
			ID	   = std::max(ID, id);
			pPage  = gProfiler.m_Allocator.AllocatePage(ID);
			Offset = 0;
		}
		void* pData = static_cast<char*>(pPage->GetData()) + Offset;
		Offset += size;
		return pData;
	}

private:
	uint32						 Offset = 0;
	uint32						 ID		= 0;
	ProfilerAllocator::Page*	 pPage	= nullptr;
};
static thread_local SubAllocator gAllocator;

static uint32 HSV2RGB(float hue, float saturation, float value)
{
	float R = std::max(std::min(fabs(hue * 6 - 3) - 1, 1.0f), 0.0f);
	float G = std::max(std::min(2 - fabs(hue * 6 - 2), 1.0f), 0.0f);
	float B = std::max(std::min(2 - fabs(hue * 6 - 4), 1.0f), 0.0f);

	R = ((R - 1) * saturation + 1) * value;
	G = ((G - 1) * saturation + 1) * value;
	B = ((B - 1) * saturation + 1) * value;

	return ((uint8)roundf(R * 255.0f) << 0) |
		   ((uint8)roundf(G * 255.0f) << 8) |
		   ((uint8)roundf(B * 255.0f) << 16) |
		   255 << 24;
}

static uint32 GetFrameColor(uint32 frameIndex)
{
	return HSV2RGB(frameIndex % 10 / 10.0f, 0.5f, 0.5f);
}

static uint32 ColorFromString(const char* pStr)
{
	const float saturation = 0.5f;
	const float value	   = 0.6f;
	float		hue		   = (float)std::hash<std::string>{}(pStr) / std::numeric_limits<size_t>::max();
	return HSV2RGB(hue, saturation, value);
}


//-----------------------------------------------------------------------------
// [SECTION] GPU Profiler
//-----------------------------------------------------------------------------

static constexpr const char* GetCommandQueueName(D3D12_COMMAND_LIST_TYPE type)
{
	switch (type)
	{
	case D3D12_COMMAND_LIST_TYPE_DIRECT:		return "Direct Queue";
	case D3D12_COMMAND_LIST_TYPE_COMPUTE:		return "Compute Queue";
	case D3D12_COMMAND_LIST_TYPE_COPY:			return "Copy Queue";
	case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE:	return "Video Decode Queue";
	case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE:	return "Video Encode Queue";
	case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS:	return "Video Process Queue";
	default:									return "Unknown Queue";
	}
}

void GPUProfiler::Initialize(ID3D12Device* pDevice, Span<ID3D12CommandQueue*> queues, uint32 frameLatency)
{
	gAssert(frameLatency >= 1, "Frame Latency must be at least 1");

	m_FrameLatency = frameLatency;

	InitializeSRWLock((PSRWLOCK)&m_CommandListMapLock);
	QueryPerformanceFrequency((LARGE_INTEGER*)&m_CPUTickFrequency);

	m_QueueEventStack.resize(queues.size());
	for (uint32 queueIndex = 0; queueIndex < queues.size(); ++queueIndex)
	{
		ID3D12CommandQueue*		 pQueue = queues[queueIndex];
		D3D12_COMMAND_QUEUE_DESC desc	= pQueue->GetDesc();

		m_QueueIndexMap[pQueue] = (uint32)m_Queues.size();
		QueueInfo& queueInfo	= m_Queues.emplace_back();
		uint32	   size			= ARRAYSIZE(queueInfo.Name);
		constexpr GUID ID_D3DDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 };
		if (FAILED(pQueue->GetPrivateData(ID_D3DDebugObjectName, &size, queueInfo.Name)))
			strcpy_s(queueInfo.Name, GetCommandQueueName(desc.Type));
		queueInfo.pQueue		 = pQueue;
		queueInfo.Index			 = queueIndex;
		queueInfo.QueryHeapIndex = desc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? 1 : 0;
		gVerifyHR(pQueue->GetClockCalibration(&queueInfo.GPUCalibrationTicks, &queueInfo.CPUCalibrationTicks));
		gVerifyHR(pQueue->GetTimestampFrequency(&queueInfo.GPUFrequency));

		if (!GetHeap(queueInfo.QueryHeapIndex).IsInitialized())
			GetHeap(queueInfo.QueryHeapIndex).Initialize(pDevice, pQueue, QueryData::cMaxNumQueries, frameLatency);

		queueInfo.TrackIndex = gProfiler.RegisterTrack(queueInfo.Name, Profiler::EventTrack::EType::GPU, queueInfo.Index);
	}

	// Maximum number of events is the number of queries is the total capacity of all query heaps divided by 2 (a pair of queries per event)
	int queryCapacity = 0;
	for (QueryHeap& heap : m_QueryHeaps)
		queryCapacity += heap.GetQueryCapacity();


	m_QueryData.resize(frameLatency);
	for (uint32 i = 0; i < frameLatency; ++i)
	{
		m_QueryData[i].Pairs.resize(queryCapacity / 2);
		m_QueryData[i].Events.resize(queryCapacity / 2);
	}

	m_IsInitialized = true;
}

void GPUProfiler::Shutdown()
{
	for (QueryHeap& heap : m_QueryHeaps)
		heap.Shutdown();

	for (auto& commandListState : m_CommandListMap)
		delete commandListState.second;
	m_CommandListMap.clear();

	m_QueryData.clear();

	m_Queues.clear();
	m_QueueIndexMap.clear();
}

void GPUProfiler::BeginEvent(ID3D12GraphicsCommandList* pCmd, const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventBegin)
		m_EventCallback.OnEventBegin(pName, pCmd, m_EventCallback.pUserData);

	if (m_IsPaused)
		return;

	// Register a query on the commandlist
	CommandListState& cmdListState = *GetState(pCmd, true);
	QueryData::Query& cmdListQuery = cmdListState.Queries.emplace_back();

	QueryData& queryData = GetQueryData();

	// Allocate a query range. This stores a begin/end query index pair. (Also event index)
	uint32 eventIndex = m_EventIndex.fetch_add(1);
	if (eventIndex >= queryData.Events.size())
		return;

	// Record a timestamp query and assign to the commandlist
	cmdListQuery.QueryIndex = GetHeap(pCmd->GetType()).RecordQuery(pCmd);
	cmdListQuery.EventIndex = eventIndex;

	// Allocate an event in the sample history
	ProfilerEvent& event = queryData.Events[eventIndex];
	event.pName			 = gAllocator.String(pName, m_FrameIndex);
	event.pFilePath		 = pFilePath;
	event.LineNumber	 = lineNumber;
	event.Color			 = color == 0 ? ColorFromString(pName) : color;
}

void GPUProfiler::EndEvent(ID3D12GraphicsCommandList* pCmd)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventEnd)
		m_EventCallback.OnEventEnd(pCmd, m_EventCallback.pUserData);

	if (m_IsPaused)
		return;

	// Record a timestamp query and assign to the commandlist
	CommandListState& cmdListState = *GetState(pCmd, true);
	QueryData::Query& query		   = cmdListState.Queries.emplace_back();
	query.QueryIndex			   = GetHeap(pCmd->GetType()).RecordQuery(pCmd);
	query.EventIndex			   = QueryData::Query::EndEventFlag; // Range index to indicate this is an 'End' query
}

// Process the last frame and advance to the next
void GPUProfiler::Tick()
{
	if (!m_IsInitialized)
		return;

	for (ActiveEventStack& stack : m_QueueEventStack)
		gAssert(stack.GetSize() == 0, "EventStack for the CommandQueue should be empty. Forgot to `End()` %d Events", stack.GetSize());

	// Poll query heap and populate event timings
	while (m_FrameToReadback < m_FrameIndex)
	{
		// Wait for all query heaps to have finished resolving the queries for the readback frame
		bool allHeapsReady = true;
		for (QueryHeap& heap : m_QueryHeaps)
			allHeapsReady &= heap.IsFrameComplete(m_FrameToReadback);
		if (!allHeapsReady)
			break;

		std::scoped_lock lock(m_QueryRangeLock);

		QueryData& queryData = GetQueryData(m_FrameToReadback);
		for (uint32 i = 0; i < queryData.NumEvents; ++i)
		{
			ProfilerEvent&		  event		 = queryData.Events[i];
			QueryData::QueryPair& queryRange = queryData.Pairs[i];
			gAssert(queryRange.IsValid());

			const QueueInfo&   queue   = m_Queues[event.QueueIndex];
			Span<const uint64> queries = m_QueryHeaps[queue.QueryHeapIndex].GetQueryData(m_FrameToReadback);

			// Convert to CPU ticks and assign to event
			event.TicksBegin = ConvertToCPUTicks(queue, queries[queryRange.QueryIndexBegin]);
			event.TicksEnd	 = ConvertToCPUTicks(queue, queries[queryRange.QueryIndexEnd]);

			// Invalidate
			queryRange = {};

			gProfiler.AddEvent(queue.TrackIndex, event, m_FrameToReadback);
		}
		queryData.NumEvents = 0;

		++m_FrameToReadback;
	}

	m_IsPaused = m_PauseQueued;
	if (m_IsPaused)
		return;

	for (const auto& data: m_CommandListMap)
		gAssert(data.second->Queries.empty(), "The Queries inside the commandlist is not empty. This is because ExecuteCommandLists was not called with this commandlist.");

	for (QueryHeap& heap : m_QueryHeaps)
		heap.Resolve(m_FrameIndex);

	++m_FrameIndex;

	{
		for (QueryHeap& heap : m_QueryHeaps)
			heap.Reset(m_FrameIndex);

		m_EventIndex = 0;
	}
}

void GPUProfiler::ExecuteCommandLists(const ID3D12CommandQueue* pQueue, Span<ID3D12CommandList*> commandLists)
{
	if (!m_IsInitialized)
		return;

	if (m_IsPaused)
		return;

	auto it = m_QueueIndexMap.find(pQueue);
	if (it == m_QueueIndexMap.end())
		return;

	std::scoped_lock lock(m_QueryRangeLock);

	uint32			   queueIndex  = it->second;
	ActiveEventStack&  eventStack  = m_QueueEventStack[queueIndex];
	QueryData&		   queryData   = GetQueryData();
	queryData.NumEvents			   = m_EventIndex;

	for (ID3D12CommandList* pCmd : commandLists)
	{
		CommandListState* pCommandListQueries = GetState(pCmd, false);
		if (pCommandListQueries)
		{
			for (QueryData::Query& query : pCommandListQueries->Queries)
			{
				if (query.EventIndex != QueryData::Query::EndEventFlag)
				{
					// If it's a "BeginEvent", add to the stack
					eventStack.Push() = query;
					if (query.EventIndex == QueryData::Query::InvalidEventFlag)
						continue;

					ProfilerEvent& sampleEvent = queryData.Events[query.EventIndex];
					sampleEvent.QueueIndex	   = queueIndex;
				}
				else
				{
					// If it's an "EndEvent", pop top query from the stack and pair up
					gAssert(eventStack.GetSize() > 0, "Event Begin/End mismatch");
					QueryData::Query beginEventQuery = eventStack.Pop();
					if (beginEventQuery.EventIndex == QueryData::Query::InvalidEventFlag)
						continue;

					// Pair up Begin/End query indices
					QueryData::QueryPair& pair = queryData.Pairs[beginEventQuery.EventIndex];
					pair.QueryIndexBegin	   = beginEventQuery.QueryIndex;
					pair.QueryIndexEnd		   = query.QueryIndex;

					// Compute event depth
					ProfilerEvent& sampleEvent = queryData.Events[beginEventQuery.EventIndex];
					sampleEvent.Depth		   = eventStack.GetSize();
					gAssert(sampleEvent.QueueIndex == queueIndex, "Begin/EndEvent must be recorded on the same queue");
				}
			}
			pCommandListQueries->Queries.clear();
		}
	}
}


GPUProfiler::CommandListState* GPUProfiler::GetState(ID3D12CommandList* pCmd, bool createIfNotFound)
{
	// See if it's already tracked
	AcquireSRWLockShared((PSRWLOCK)&m_CommandListMapLock);
	auto   it	 = m_CommandListMap.find(pCmd);
	CommandListState* pState = it != m_CommandListMap.end() ? it->second : nullptr;
	ReleaseSRWLockShared((PSRWLOCK)&m_CommandListMapLock);

	if (!pState && createIfNotFound)
	{
		// If not, register new commandlist
		// TODO: Pool CommandListStates in case ID3D12CommandLists are often recreated
		pState = new CommandListState(this, pCmd);
	
		AcquireSRWLockExclusive((PSRWLOCK)&m_CommandListMapLock);
		m_CommandListMap[pCmd] = pState;
		ReleaseSRWLockExclusive((PSRWLOCK)&m_CommandListMapLock);
	}
	return pState;
}



GPUProfiler::CommandListState::CommandListState(GPUProfiler* profiler, ID3D12CommandList* pCmd)
	: pProfiler(profiler), pCommandList(pCmd)
{
	ID3DDestructionNotifier* destruction_notifier = nullptr;
	gVerifyHR(pCmd->QueryInterface(&destruction_notifier));
	gVerifyHR(destruction_notifier->RegisterDestructionCallback([](void* pContext) {
		GPUProfiler::CommandListState* pState = (GPUProfiler::CommandListState*)pContext;

		AcquireSRWLockExclusive((PSRWLOCK)&pState->pProfiler->m_CommandListMapLock);
		pState->pProfiler->m_CommandListMap.erase(pState->pCommandList);
		ReleaseSRWLockExclusive((PSRWLOCK)&pState->pProfiler->m_CommandListMapLock);

		delete pState;
	}, this, &DestructionEventID));

	destruction_notifier->Release();
}



GPUProfiler::CommandListState::~CommandListState()
{
	ID3DDestructionNotifier* destruction_notifier = nullptr;
	gVerifyHR(pCommandList->QueryInterface(&destruction_notifier));
	gVerifyHR(destruction_notifier->UnregisterDestructionCallback(DestructionEventID));
	destruction_notifier->Release();
}



void GPUProfiler::QueryHeap::Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pResolveQueue, uint32 maxNumQueries, uint32 frameLatency)
{
	gAssert(gProfiler.IsInitialized());

	m_pResolveQueue = pResolveQueue;
	m_FrameLatency	= frameLatency;
	m_MaxNumQueries = maxNumQueries;

	D3D12_COMMAND_QUEUE_DESC queueDesc = pResolveQueue->GetDesc();

	D3D12_QUERY_HEAP_DESC heapDesc{
		.Type	  = queueDesc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
		.Count	  = maxNumQueries,
		.NodeMask = 0x1,
	};
	gVerifyHR(pDevice->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_pQueryHeap)));

	for (uint32 i = 0; i < frameLatency; ++i)
		gVerifyHR(pDevice->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&m_CommandAllocators.emplace_back())));
	gVerifyHR(pDevice->CreateCommandList(0x1, queueDesc.Type, m_CommandAllocators[0], nullptr, IID_PPV_ARGS(&m_pCommandList)));

	D3D12_RESOURCE_DESC readbackDesc{
		.Dimension		  = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment		  = 0,
		.Width			  = (uint64)maxNumQueries * sizeof(uint64) * frameLatency,
		.Height			  = 1,
		.DepthOrArraySize = 1,
		.MipLevels		  = 1,
		.Format			  = DXGI_FORMAT_UNKNOWN,
		.SampleDesc		  = {
				  .Count   = 1,
				  .Quality = 0,
		  },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags	= D3D12_RESOURCE_FLAG_NONE,
	};

	D3D12_HEAP_PROPERTIES heapProps{
		.Type				  = D3D12_HEAP_TYPE_READBACK,
		.CPUPageProperty	  = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
		.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
		.CreationNodeMask	  = 0,
		.VisibleNodeMask	  = 0,
	};

	gVerifyHR(pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pReadbackResource)));
	void* pReadbackData = nullptr;
	gVerifyHR(m_pReadbackResource->Map(0, nullptr, &pReadbackData));
	m_ReadbackData = Span<const uint64>(static_cast<uint64*>(pReadbackData), maxNumQueries * frameLatency);

	gVerifyHR(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pResolveFence)));
}

void GPUProfiler::QueryHeap::Shutdown()
{
	if (!IsInitialized())
		return;

	for (ID3D12CommandAllocator* pAllocator : m_CommandAllocators)
		pAllocator->Release();
	m_pCommandList->Release();
	m_pQueryHeap->Release();
	m_pReadbackResource->Release();
	m_pResolveFence->Release();
}

uint32 GPUProfiler::QueryHeap::RecordQuery(ID3D12GraphicsCommandList* pCmd)
{
	uint32 index = m_QueryIndex.fetch_add(1);
	if (index >= m_MaxNumQueries)
		return 0xFFFFFFFF;

	pCmd->EndQuery(m_pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, index);
	return index;
}

uint32 GPUProfiler::QueryHeap::Resolve(uint32 frameIndex)
{
	if (!IsInitialized())
		return 0;

	uint32 frameBit	  = frameIndex % m_FrameLatency;
	uint32 queryStart = frameBit * m_MaxNumQueries;
	uint32 numQueries = std::min(m_MaxNumQueries, (uint32)m_QueryIndex);
	m_pCommandList->ResolveQueryData(m_pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, numQueries, m_pReadbackResource, queryStart * sizeof(uint64));
	gVerifyHR(m_pCommandList->Close());
	ID3D12CommandList* pCmdLists[] = { m_pCommandList };
	m_pResolveQueue->ExecuteCommandLists(1, pCmdLists);
	gVerifyHR(m_pResolveQueue->Signal(m_pResolveFence, frameIndex));
	return numQueries;
}

void GPUProfiler::QueryHeap::Reset(uint32 frameIndex)
{
	if (!IsInitialized())
		return;

	// Don't advance to the next frame until the GPU has caught up until at least the frame latency
	if (frameIndex >= m_FrameLatency)
	{
		uint32 wait_frame = frameIndex - m_FrameLatency;
		if (!IsFrameComplete(wait_frame))
			m_pResolveFence->SetEventOnCompletion(wait_frame, nullptr);
	}

	m_QueryIndex					   = 0;
	ID3D12CommandAllocator* pAllocator = m_CommandAllocators[frameIndex % m_FrameLatency];
	gVerifyHR(pAllocator->Reset());
	gVerifyHR(m_pCommandList->Reset(pAllocator, nullptr));
}

bool GPUProfiler::QueryHeap::IsFrameComplete(uint64 frameIndex)
{
	if (!IsInitialized())
		return true;

	uint64 fenceValue = frameIndex;
	if (fenceValue <= m_LastCompletedFence && m_LastCompletedFence > 0)
		return true;
	m_LastCompletedFence = std::max(m_pResolveFence->GetCompletedValue(), m_LastCompletedFence);
	return fenceValue <= m_LastCompletedFence;
}

//-----------------------------------------------------------------------------
// [SECTION] CPU Profiler
//-----------------------------------------------------------------------------

void Profiler::Initialize(uint32 historySize)
{
	m_HistorySize	= historySize;
	m_IsInitialized = true;
	m_BeginFrameTicks.resize(historySize);

	uint64 frequency = 0;
	QueryPerformanceFrequency((LARGE_INTEGER*)&frequency);
	m_MsToTicks = (uint32)(frequency / 1000);
}

void Profiler::Shutdown()
{
	m_Tracks.clear();
	m_BeginFrameTicks.clear();

	m_Allocator.Release();
}

// Begin a new CPU event on the current thread
void Profiler::BeginEvent(const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventBegin)
		m_EventCallback.OnEventBegin(pName, m_EventCallback.pUserData);

	if (m_Paused)
		return;

	// Record new event
	EventTrack&		   track	 = GetCurrentThreadTrack();
	ProfilerEventData& eventData = track.GetFrameData(m_FrameIndex);
	track.EventStack.Push() = (uint32)eventData.size();

	ProfilerEvent& newEvent = eventData.emplace_back();
	newEvent.Depth			= track.EventStack.GetSize() - 1;
	newEvent.ThreadIndex	= track.Index;
	newEvent.pName			= gAllocator.String(pName, m_FrameIndex);
	newEvent.pFilePath		= pFilePath;
	newEvent.LineNumber		= lineNumber;
	newEvent.Color			= color == 0 ? ColorFromString(pName) : color;
	QueryPerformanceCounter((LARGE_INTEGER*)(&newEvent.TicksBegin));
}

// End the last pushed event on the current thread
void Profiler::EndEvent()
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventEnd)
		m_EventCallback.OnEventEnd(m_EventCallback.pUserData);

	if (m_Paused)
		return;

	// End and pop an event of the stack
	EventTrack& track = GetCurrentThreadTrack();

	gAssert(track.EventStack.GetSize() > 0, "Event mismatch. Called EndEvent more than BeginEvent");
	uint32		   eventIndex = track.EventStack.Pop();
	ProfilerEvent& event	  = track.GetFrameData(m_FrameIndex)[eventIndex];
	QueryPerformanceCounter((LARGE_INTEGER*)(&event.TicksEnd));
}


void Profiler::AddEvent(uint32 trackIndex, const ProfilerEvent& event, uint32 frameIndex)
{
	EventTrack&		   track  = m_Tracks[trackIndex];
	ProfilerEventData& events = track.GetFrameData(frameIndex);

	// Name must be copied
	ProfilerEvent newEvent = event;
	newEvent.pName		   = gAllocator.String(newEvent.pName, frameIndex);

	events.push_back(newEvent);
}


void Profiler::Present(IDXGISwapChain* pSwapChain)
{
	if (m_PresentTrackIndex == -1)
		m_PresentTrackIndex = RegisterTrack("Present", EventTrack::EType::Present, 0);

	if (!m_Paused)
	{
		// Add an entry for the current present
		uint32 presentID;
		if (SUCCEEDED(pSwapChain->GetLastPresentCount(&presentID)))
		{
			// If the last queried present is larger than the current present,
			// that means the swapchain may have been recreated and we need to reset.
			if (m_LastQueuedPresentID > presentID || m_pPresentSwapChain != pSwapChain)
			{
				m_LastQueriedPresentID		  = 0;
				m_LastProcessedPresentID	  = 0;
				m_LastProcessedValidPresentID = 0;
				m_PresentQueue				  = {};
				m_LastSyncRefreshCount		  = 0;
				m_pPresentSwapChain			  = pSwapChain;
			}

			PresentEntry* pEntry = GetPresentEntry(presentID, true);
			*pEntry				 = {};
			QueryPerformanceCounter((LARGE_INTEGER*)&pEntry->PresentQPC);
			pEntry->PresentID  = presentID;
			pEntry->FrameIndex = m_FrameIndex;

			m_LastQueuedPresentID = presentID;
		}

		DXGI_FRAME_STATISTICS frameStats{};
		while (SUCCEEDED(pSwapChain->GetFrameStatistics(&frameStats)) && frameStats.PresentCount > m_LastQueriedPresentID)
		{
			gAssert(frameStats.SyncQPCTime.QuadPart != 0);

			// Update the entry that was presented. All the ones before that were not queried are considered dropped.
			PresentEntry* pEntry = GetPresentEntry(frameStats.PresentCount, false);
			if (pEntry)
				pEntry->DisplayQPC = frameStats.SyncQPCTime.QuadPart;

			// It's possible for the CPU to miss a refresh in the time multiple presents have been processed (and not dropped).
			// In this case, there isn't enough information to resolve it. Try to estimate, otherwise ignore the frame.
			uint32 numRefreshes = frameStats.SyncRefreshCount - m_LastSyncRefreshCount;
			if (frameStats.SyncRefreshCount > 0 && numRefreshes > 1)
			{
				// If the previous entry was not processed, it means we missed it in GetFrameStatistics
				PresentEntry* pPreviousEntry = GetPresentEntry(frameStats.PresentCount - 1, false);
				if (pPreviousEntry && pPreviousEntry->DisplayQPC == PresentEntry::sQPCDroppedFrame)
				{
					// If there was a valid present before the previous one, average the time to at least get some estimate
					PresentEntry* pPreviousPreviousEntry = GetPresentEntry(frameStats.PresentCount - 2, false);
					if (pPreviousPreviousEntry && pEntry)
					{
						// printf("MISSED FRAME: %d - ID %d\n", pPreviousEntry->FrameIndex, pPreviousEntry->PresentID);

						// Taking a guess: the sync happened one sync after the one before it (most likely timing to have been missed)
						uint64 duration			 = pEntry->DisplayQPC - pPreviousPreviousEntry->DisplayQPC;
						uint64 estimatedSyncTime = pPreviousPreviousEntry->DisplayQPC + duration / numRefreshes;

						pPreviousEntry->DisplayQPC = estimatedSyncTime;
					}
					else
					{
						// If there's no way to estimate, just drop it
						pPreviousEntry->DisplayQPC = PresentEntry::sQPCMissedFrame;
					}
				}
			}

			m_LastSyncRefreshCount = frameStats.SyncRefreshCount;
			m_LastQueriedPresentID = frameStats.PresentCount;
		}
	}

	uint32 stackSize = 0;

	// Process all entries up until the last present
	for (uint32 presentID = m_LastProcessedPresentID + 1; presentID < m_LastQueriedPresentID; ++presentID)
	{
		const PresentEntry* pToProcessEntry = GetPresentEntry(presentID, false);

		// Search from the last processed valid entry where the next one is
		const PresentEntry* pNextValidPresentEntry = nullptr;
		for (uint32 nextValidPresent = m_LastProcessedValidPresentID; nextValidPresent <= m_LastQueriedPresentID; ++nextValidPresent)
		{
			const PresentEntry* pEntry = GetPresentEntry(nextValidPresent, false);
			if (pEntry && pEntry->DisplayQPC != PresentEntry::sQPCDroppedFrame)
			{
				pNextValidPresentEntry		  = pEntry;
				m_LastProcessedValidPresentID = pEntry->PresentID;
			}
		}

		// No valid present yet, try again next frame
		if (!pNextValidPresentEntry)
			break;

		if (pToProcessEntry)
		{
			if (pToProcessEntry->DisplayQPC == PresentEntry::sQPCMissedFrame)
			{
				// Ignored frame (rare, see above)
			}
			else if (pToProcessEntry->DisplayQPC == PresentEntry::sQPCDroppedFrame)
			{
				ProfilerEvent event{
					.pName		= "Discarded",
					.Color		= GetFrameColor(pToProcessEntry->FrameIndex),
					.Depth		= 1,
					.TicksBegin = pNextValidPresentEntry->DisplayQPC,
					.TicksEnd	= pNextValidPresentEntry->DisplayQPC + m_MsToTicks,
				};
				AddEvent(m_PresentTrackIndex, event, pToProcessEntry->FrameIndex);
			}
			else
			{
				// If the timing of the refresh is the same as the current frame, stack the events. 
				// Wait for the next frame with a different refresh time
				if (pNextValidPresentEntry->DisplayQPC == pToProcessEntry->DisplayQPC)
					break;

				ProfilerEvent event{
					.pName		= "Present",
					.Color		= GetFrameColor(pToProcessEntry->FrameIndex),
					.Depth		= stackSize,
					.TicksBegin = pToProcessEntry->DisplayQPC,
					.TicksEnd	= pNextValidPresentEntry->DisplayQPC,
				};
				++stackSize;
				AddEvent(m_PresentTrackIndex, event, pToProcessEntry->FrameIndex);
			}
		}

		m_LastProcessedPresentID = presentID;
	}
}



// Process the last frame and advance
void Profiler::Tick()
{
	if (!m_IsInitialized)
		return;

	m_Paused = m_QueuedPaused;
	if (m_Paused)
		return;

	// End the "CPU Frame" event (except on frame 0)
	if (m_FrameIndex)
		EndEvent();

	// Advance the frame and reset its data
	++m_FrameIndex;

	std::scoped_lock lock(m_ThreadDataLock);
	for (EventTrack& track : m_Tracks)
	{
		ProfilerEventData& events = track.GetFrameData(m_FrameIndex);
		events.clear();
	}

	QueryPerformanceCounter((LARGE_INTEGER*)&m_BeginFrameTicks[m_FrameIndex % m_BeginFrameTicks.size()]);

	// Begin a "CPU Frame" event
	BeginEvent("CPU Frame", GetFrameColor(m_FrameIndex));

	if (m_FrameIndex >= m_HistorySize)
		m_Allocator.Evict(m_FrameIndex - m_HistorySize);
}



// Register a new thread
int Profiler::RegisterCurrentThread(const char* pName)
{
	int& threadIndex = GetCurrentThreadTrackIndex();

	const char* pLocalName = pName;
	char name[256]{};
	if (!pName)
	{
		// If the name is not provided, retrieve it using GetThreadDescription()
		PWSTR pDescription = nullptr;
		if (SUCCEEDED(::GetThreadDescription(GetCurrentThread(), &pDescription)))
			WideCharToMultiByte(CP_UTF8, 0, pDescription, (int)wcslen(pDescription), name, ARRAYSIZE(name), nullptr, nullptr);
		pLocalName = name;
	}

	if (threadIndex == -1)
	{
		threadIndex = RegisterTrack(pLocalName, EventTrack::EType::CPU, GetCurrentThreadId());
	}
	else
	{
		strcpy_s(m_Tracks[threadIndex].Name, pLocalName);
	}
	return threadIndex;
}


// Register a new track
int Profiler::RegisterTrack(const char* pName, EventTrack::EType type, uint32 id)
{
	std::scoped_lock lock(m_ThreadDataLock);

	EventTrack& data = m_Tracks.emplace_back();
	strcpy_s(data.Name, ARRAYSIZE(data.Name), pName);
	data.ID	   = id;
	data.Index = (uint32)m_Tracks.size() - 1;
	data.Type  = type;
	data.Events.resize(m_HistorySize);

	return data.Index;
}


#endif
