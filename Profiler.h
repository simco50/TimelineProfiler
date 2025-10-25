#pragma once

#ifndef WITH_PROFILING
#define WITH_PROFILING 1
#endif

#if !WITH_PROFILING

#define PROFILE_REGISTER_THREAD(...)
#define PROFILE_FRAME()
#define PROFILE_EXECUTE_COMMANDLISTS(...)

#define PROFILE_CPU_SCOPE(...)
#define PROFILE_CPU_BEGIN(...)
#define PROFILE_CPU_END()

#define PROFILE_GPU_SCOPE(...)
#define PROFILE_GPU_BEGIN(...)
#define PROFILE_GPU_END()

#define PROFILE_PRESENT(...)

#else

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
#include <queue>

void DrawProfilerHUD();

void HandleAssertMessage(const char* pMessage);

template<typename... Args>
static void HandleAssertMessage(const char* pExpression, const char* pFileName, int line, const char* pFmt = nullptr, Args... args)
{
	char message[1024]{};
	if (pFmt)
		sprintf_s(message, pFmt, std::forward<Args>(args)...);
	char finalMessage[1024]{};
	sprintf_s(finalMessage, "ASSERT FAILED:\nExpression: %s\nFile: %s:%d\n%s\n", pExpression, __FILE__, __LINE__, message);
	HandleAssertMessage(finalMessage);
}

#define gAssert(op, ...)                                               \
	do                                                                 \
	{                                                                  \
		bool result = (op);                                            \
		if (result == false)                                           \
		{                                                              \
			HandleAssertMessage(#op, __FILE__, __LINE__, __VA_ARGS__); \
			__debugbreak();                                            \
		}                                                              \
	} while (false)

#define gVerify(op, expected, ...) \
	do                        \
	{                         \
		auto r = (op);        \
		gAssert(r expected, __VA_ARGS__);  \
	} while (false)

#define gVerifyHR(op)				 gVerify(op, == S_OK, "HRESULT Failed")
#define gBoundCheck(val, minV, maxV) gAssert(val >= minV && val < maxV, "Value out of bounds")

#define _STRINGIFY(a)	   #a
#define STRINGIFY(a)	   _STRINGIFY(a)
#define CONCAT_IMPL(x, y)  x##y
#define MACRO_CONCAT(x, y) CONCAT_IMPL(x, y)

// Basic types
using uint64 = uint64_t;
using uint32 = uint32_t;
using uint16 = uint16_t;
using uint8	 = uint8_t;
template <typename T>
using Span = std::span<T>;
template <typename T>
using Array = std::vector<T>;
template <typename T, size_t Size>
using StaticArray = std::array<T, Size>;
template <typename K, typename V>
using HashMap = std::unordered_map<K, V>;
using Mutex = std::mutex;

struct URange
{
	uint32 Begin = 0;
	uint32 End	 = 0;
	uint32 GetLength() const { return End - Begin; }
};

// Forward declare D3D12 types
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandAllocator;
struct ID3D12QueryHeap;
struct ID3D12Fence;
struct IDXGISwapChain;
using WinHandle = void*;


/*
	General
*/

// Usage:
//		PROFILE_REGISTER_THREAD(const char* pName)
//		PROFILE_REGISTER_THREAD()
#define PROFILE_REGISTER_THREAD(...) gProfiler.RegisterCurrentThread(__VA_ARGS__)

/// Usage:
//		PROFILE_FRAME()
#define PROFILE_FRAME()  \
	gProfiler.Tick(); \
	gGPUProfiler.Tick()

/// Usage:
///		PROFILE_EXECUTE_COMMANDLISTS(ID3D12CommandQueue* pQueue, Span<ID3D12CommandLists*> commandLists)
#define PROFILE_EXECUTE_COMMANDLISTS(queue, cmdlists) gGPUProfiler.ExecuteCommandLists(queue, cmdlists)

/*
	CPU Profiling
*/

// Usage:
//		PROFILE_CPU_SCOPE(const char* pName)
//		PROFILE_CPU_SCOPE()
#define PROFILE_CPU_SCOPE(...) CPUProfileScope MACRO_CONCAT(profiler, __COUNTER__)(__FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)

// Usage:
//		PROFILE_CPU_BEGIN(const char* pName)
//		PROFILE_CPU_BEGIN()
#define PROFILE_CPU_BEGIN(...) gProfiler.BeginEvent(__VA_ARGS__)
// Usage:
//		PROFILE_CPU_END()
#define PROFILE_CPU_END() gProfiler.EndEvent()


#define PROFILE_PRESENT(swapchain)		 gProfiler.Present(swapchain)


/*
	GPU Profiling
*/

// Usage:
//		PROFILE_GPU_SCOPE(ID3D12GraphicsCommandList* pCommandList, const char* pName)
//		PROFILE_GPU_SCOPE(ID3D12GraphicsCommandList* pCommandList)
#define PROFILE_GPU_SCOPE(cmdlist, ...) GPUProfileScope MACRO_CONCAT(gpu_profiler, __COUNTER__)(__FUNCTION__, __FILE__, __LINE__, cmdlist, __VA_ARGS__)

// Usage:
//		PROFILE_GPU_BEGIN(const char* pName, ID3D12GraphicsCommandList* pCommandList)
#define PROFILE_GPU_BEGIN(cmdlist, name) gGPUProfiler.BeginEvent(cmdlist, name, __FILE__, __LINE__)

// Usage:
//		PROFILE_GPU_END(ID3D12GraphicsCommandList* pCommandList)
#define PROFILE_GPU_END(cmdlist) gGPUProfiler.EndEvent(cmdlist)



template <typename T, uint32 N>
struct FixedArray
{
public:
	T& Pop()
	{
		gAssert(Length > 0);
		--Length;
		return Data[Length];
	}

	T& Push()
	{
		Length++;
		gAssert(Length < N);
		return Data[Length - 1];
	}

	T& Top()
	{
		gAssert(Length > 0);
		return Data[Length - 1];
	}

	uint32 GetSize() const { return Length; }

private:
	uint32			  Length = 0;
	StaticArray<T, N> Data{};
};



// Thread-safe page allocator that recycles pages based on ID
struct ProfilerAllocator
{
public:
	static constexpr uint32 cPageSize = 2 * 1024;

	struct Page
	{
		uint32 ID	= 0;
		uint32 Size = 0;

		void* GetData()
		{
			return static_cast<void*>(this + 1);
		}

		static Page* Create(uint32 size)
		{
			void* pData = new char[sizeof(Page) + size];
			Page* pPage = new (pData) Page;
			pPage->Size = size;
			return pPage;
		}

		static void Release(Page* pPage)
		{
			char* pData = reinterpret_cast<char*>(pPage);
			delete[] pData;
		}
	};
	static_assert(std::is_trivially_destructible_v<Page>);

	void Release()
	{
		while (!AllocatedPages.empty())
		{
			Page* pPage = AllocatedPages.front();
			Page::Release(pPage);
			AllocatedPages.pop();
		}
		while (FreePages.empty())
		{
			Page* pPage = FreePages.back();
			Page::Release(pPage);
			FreePages.pop_back();
		}
	}

	Page* AllocatePage(uint32 id)
	{
		std::lock_guard m(PageLock);

		Page* pPage = nullptr;
		if (FreePages.empty())
		{
			pPage = Page::Create(cPageSize);
			++NumPages;
		}
		else
		{
			pPage = FreePages.back();
			FreePages.pop_back();
		}
		pPage->ID = id;
		AllocatedPages.push(pPage);
		return pPage;
	}

	bool IsValidPage(uint32 id)
	{
		return id >= MinValidID;
	}

	void Evict(uint32 id)
	{
		std::lock_guard m(PageLock);

		gAssert(NumPages == FreePages.size() + AllocatedPages.size());

		while (!AllocatedPages.empty())
		{
			Page* pPage = AllocatedPages.front();
			if (id >= pPage->ID)
			{
				FreePages.push_back(pPage);
				AllocatedPages.pop();
			}
			else
			{
				break;
			}
		}

		MinValidID = id + 1;
	}
	
private:
	std::mutex		  PageLock;
	Array<Page*>	  FreePages;
	std::queue<Page*> AllocatedPages;
	uint32			  MinValidID = 0;
	int				  NumPages	 = 0;
};



// Single event
struct ProfilerEvent
{
	const char* pName			 = nullptr;	 ///< Name of event
	const char* pFilePath		 = nullptr;	 ///< File path of location where event was started
	uint32		Color		: 24 = 0xFFFFFF; ///< Color
	uint32		Depth		: 8	 = 0;		 ///< Stack depth of event
	uint32		LineNumber	: 18 = 0;		 ///< Line number of file where event was started
	uint32		ThreadIndex : 10 = 0;		 ///< Index of thread this event is started on
	uint32		QueueIndex	: 4	 = 0;		 ///< GPU Queue Index (GPU-specific)
	uint64		TicksBegin		 = 0;		 ///< Begin CPU ticks
	uint64		TicksEnd		 = 0;		 ///< End CPU ticks

	bool   IsValid() const { return TicksBegin != 0 && TicksEnd != 0; }
	uint32 GetColor() const { return Color | (0xFF << 24); }
};
static_assert(std::has_unique_object_representations_v<ProfilerEvent>);


// Data for a single frame of profiling events
using ProfilerEventData = Array<ProfilerEvent>;


//-----------------------------------------------------------------------------
// [SECTION] GPU Profiler
//-----------------------------------------------------------------------------

extern class GPUProfiler gGPUProfiler;

struct GPUProfilerCallbacks
{
	using EventBeginFn = void (*)(const char* /*pName*/, ID3D12GraphicsCommandList* /*CommandList*/, void* /*pUserData*/);
	using EventEndFn   = void (*)(ID3D12GraphicsCommandList* /*CommandList*/, void* /*pUserData*/);

	EventBeginFn OnEventBegin = nullptr;
	EventEndFn	 OnEventEnd	  = nullptr;
	void*		 pUserData	  = nullptr;
};

class GPUProfiler
{
public:
	void Initialize(ID3D12Device* pDevice, Span<ID3D12CommandQueue*> queues, uint32 frameLatency);

	void Shutdown();

	// Allocate and record a GPU event on the commandlist
	void BeginEvent(ID3D12GraphicsCommandList* pCmd, const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber);

	// Allocate and record a GPU event on the commandlist
	void BeginEvent(ID3D12GraphicsCommandList* pCmd, const char* pName, uint32 color = 0) { BeginEvent(pCmd, pName, color, "", 0); }

	// Record a GPU event end on the commandlist
	void EndEvent(ID3D12GraphicsCommandList* pCmd);

	// Resolve the last frame and advance to the next frame.
	// Call at the START of the frame.
	void Tick();

	// Notify profiler that these commandlists are executed on a particular queue
	void ExecuteCommandLists(const ID3D12CommandQueue* pQueue, Span<ID3D12CommandList*> commandLists);

	void SetPaused(bool paused) { m_PauseQueued = paused; }

	// Data of a single GPU queue. Allows converting GPU timestamps to CPU timestamps
	struct QueueInfo
	{
		char				Name[128]{};					///< Name of the queue
		ID3D12CommandQueue* pQueue				= nullptr;	///< The D3D queue object
		uint64				GPUCalibrationTicks = 0;		///< The number of GPU ticks when the calibration was done
		uint64				CPUCalibrationTicks = 0;		///< The number of CPU ticks when the calibration was done
		uint64				GPUFrequency		= 0;		///< The GPU tick frequency
		uint32				Index				= 0;		///< Index of queue
		uint32				QueryHeapIndex		= 0;		///< Query Heap index (Copy vs. Other queues)
		uint32				TrackIndex			= 0;		///< The index in the tracks of the profiler
	};

	Span<const QueueInfo> GetQueues() const { return m_Queues; }

	void SetEventCallback(const GPUProfilerCallbacks& inCallbacks) { m_EventCallback = inCallbacks; }

private:
	struct QueryHeap
	{
	public:
		constexpr static uint32 cQueryIndexBits = 16u;
		constexpr static uint32 cMaxNumQueries = (1u << cQueryIndexBits) - 1u;
		
		void Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pResolveQueue, uint32 maxNumQueries, uint32 frameLatency);
		void Shutdown();

		uint32 RecordQuery(ID3D12GraphicsCommandList* pCmd);
		uint32 Resolve(uint32 frameIndex);
		void   Reset(uint32 frameIndex);
		bool   IsFrameComplete(uint64 frameIndex);
		uint32 GetQueryCapacity() const { return m_MaxNumQueries; }

		Span<const uint64> GetQueryData(uint32 frameIndex) const
		{
			if (!IsInitialized())
				return {};

			uint32 frameBit = frameIndex % m_FrameLatency;
			return m_ReadbackData.subspan(frameBit * m_MaxNumQueries, m_MaxNumQueries);
		}

		bool			 IsInitialized() const { return m_pQueryHeap != nullptr; }
		ID3D12QueryHeap* GetHeap() const { return m_pQueryHeap; }

	private:
		Array<ID3D12CommandAllocator*> m_CommandAllocators;			   ///< CommandAlloctors to resolve queries. 1 per frame
		uint32						   m_MaxNumQueries		= 0;	   ///< Max number of event queries
		uint32						   m_FrameLatency		= 0;	   ///< Number of GPU frame latency
		std::atomic<uint32>			   m_QueryIndex			= 0;	   ///< Current index of queries
		ID3D12GraphicsCommandList*	   m_pCommandList		= nullptr; ///< CommandList to resolve queries
		ID3D12QueryHeap*			   m_pQueryHeap			= nullptr; ///< Heap containing MaxNumQueries * FrameLatency queries
		ID3D12Resource*				   m_pReadbackResource	= nullptr; ///< Readback resource storing resolved query dara
		Span<const uint64>			   m_ReadbackData		= {};	   ///< Mapped readback resource pointer
		ID3D12CommandQueue*			   m_pResolveQueue		= nullptr; ///< Queue to resolve queries on
		ID3D12Fence*				   m_pResolveFence		= nullptr; ///< Fence for tracking when queries are finished resolving
		uint64						   m_LastCompletedFence = 0;	   ///< Last finish fence value
	};

	// Data for a single frame of GPU queries. One for each frame latency
	struct QueryData
	{
		struct QueryPair
		{
			uint32 QueryIndexBegin : QueryHeap::cQueryIndexBits = 0xFFFF;
			uint32 QueryIndexEnd   : QueryHeap::cQueryIndexBits = 0xFFFF;

			bool IsValid() const { return QueryIndexBegin != 0xFFFF && QueryIndexEnd != 0xFFFF; }
		};
		static_assert(sizeof(QueryPair) == sizeof(uint32));
		Array<QueryPair>  Pairs;
		ProfilerEventData Events;
		uint32			  NumEvents = 0;
	};
	QueryData& GetQueryData(uint32 frameIndex) { return m_QueryData[frameIndex % m_FrameLatency]; }
	QueryData& GetQueryData() { return GetQueryData(m_FrameIndex); }

	// Contains the state for a tracked commandlist
	struct CommandListState
	{
		CommandListState(GPUProfiler* profiler, ID3D12CommandList* pCmd);
		~CommandListState();

		struct Query
		{
			static constexpr uint32 EndEventFlag	 = 0xFFFE;
			static constexpr uint32 InvalidEventFlag = 0xFFFF;
			
			uint32 QueryIndex : QueryHeap::cQueryIndexBits	= InvalidEventFlag; ///< The index into the query heap
			uint32 EventIndex : 16							= InvalidEventFlag; ///< The ProfilerEvent index. 0xFFFE is it is an "EndEvent"
		};
		static_assert(sizeof(Query) == sizeof(uint32));
		static_assert(std::has_unique_object_representations_v<Query>);

		GPUProfiler*	   pProfiler		  = nullptr;
		ID3D12CommandList* pCommandList = nullptr;
		uint32			   DestructionEventID = 0;
		Array<Query>	   Queries;
	};

	CommandListState* GetState(ID3D12CommandList* pCmd, bool createIfNotFound);

	uint64 ConvertToCPUTicks(const QueueInfo& queue, uint64 gpuTicks) const
	{
		gAssert(gpuTicks >= queue.GPUCalibrationTicks);
		return queue.CPUCalibrationTicks + (gpuTicks - queue.GPUCalibrationTicks) * m_CPUTickFrequency / queue.GPUFrequency;
	}

	QueryHeap& GetHeap(bool isCopyQueue) { return isCopyQueue ? m_QueryHeaps[1] : m_QueryHeaps[0]; }

	bool m_IsInitialized = false;
	bool m_IsPaused		 = false;
	bool m_PauseQueued	 = false;

	Array<QueryData>		  m_QueryData;					///< Data containing all intermediate query event data. 1 per frame latency
	std::atomic<uint32>		  m_EventIndex		 = 0;		///< Current event index
	uint32					  m_FrameLatency	 = 0;		///< Max number of in-flight GPU frames
	uint32					  m_FrameToReadback	 = 0;		///< Next frame to readback from
	uint32					  m_FrameIndex		 = 0;		///< Current frame index
	StaticArray<QueryHeap, 2> m_QueryHeaps;					///< GPU Query Heaps
	uint64					  m_CPUTickFrequency = 0;		///< Tick frequency of CPU for QPC
	Mutex					  m_QueryRangeLock;

	WinHandle									   m_CommandListMapLock{}; ///< Lock for accessing commandlist state hashmap
	HashMap<ID3D12CommandList*, CommandListState*> m_CommandListMap;	   ///< Maps commandlist to index

	static constexpr uint32 MAX_EVENT_DEPTH = 32;
	using ActiveEventStack					= FixedArray<CommandListState::Query, MAX_EVENT_DEPTH>;
	Array<ActiveEventStack>					   m_QueueEventStack; ///< Stack of active events for each command queue
	Array<QueueInfo>						   m_Queues;		  ///< All registered queues
	HashMap<const ID3D12CommandQueue*, uint32> m_QueueIndexMap;	  ///< Map from command queue to index
	GPUProfilerCallbacks					   m_EventCallback;
};

// Helper RAII-style structure to push and pop a GPU sample event
struct GPUProfileScope
{
	GPUProfileScope(const char* pFunction, const char* pFilePath, uint32 lineNumber, ID3D12GraphicsCommandList* pCmd, const char* pName)
		: pCmd(pCmd)
	{
		gGPUProfiler.BeginEvent(pCmd, pName, 0, pFilePath, lineNumber);
	}

	GPUProfileScope(const char* pFunction, const char* pFilePath, uint32 lineNumber, ID3D12GraphicsCommandList* pCmd)
		: pCmd(pCmd)
	{
		gGPUProfiler.BeginEvent(pCmd, pFunction, 0, pFilePath, lineNumber);
	}

	~GPUProfileScope()
	{
		gGPUProfiler.EndEvent(pCmd);
	}

	GPUProfileScope(const GPUProfileScope&)			   = delete;
	GPUProfileScope& operator=(const GPUProfileScope&) = delete;

private:
	ID3D12GraphicsCommandList* pCmd;
};



//-----------------------------------------------------------------------------
// [SECTION] CPU Profiler
//-----------------------------------------------------------------------------

// Global CPU Profiler
extern class Profiler gProfiler;

struct CPUProfilerCallbacks
{
	using EventBeginFn = void (*)(const char* /*pName*/, void* /*pUserData*/);
	using EventEndFn   = void (*)(void* /*pUserData*/);

	EventBeginFn OnEventBegin = nullptr;
	EventEndFn	 OnEventEnd	  = nullptr;
	void*		 pUserData	  = nullptr;
};

// CPU Profiler
// Also responsible for updating GPU profiler
// Also responsible for drawing HUD
class Profiler
{
public:
	void Initialize(uint32 historySize);

	void Shutdown();

	// Start and push an event on the current thread
	void BeginEvent(const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber);

	// Start and push an event on the current thread
	void BeginEvent(const char* pName, uint32 color = 0) { BeginEvent(pName, color, "", 0); }

	// End and pop the last pushed event on the current thread
	void EndEvent();

	void AddEvent(uint32 trackIndex, const ProfilerEvent& event, uint32 frameIndex);

	void Present(IDXGISwapChain* pSwapChain);

	bool IsInitialized() const { return m_IsInitialized; }

	// Resolve the last frame and advance to the next frame.
	// Call at the START of the frame.
	void Tick();

	// Initialize a thread with an optional name
	int RegisterCurrentThread(const char* pName = nullptr);

	struct EventTrack
	{
		enum class EType
		{
			CPU,
			GPU,
			Present,
		};

		ProfilerEventData& GetFrameData(int frameIndex) { return Events[frameIndex % Events.size()]; }
		const ProfilerEventData& GetFrameData(int frameIndex) const { return Events[frameIndex % Events.size()]; }

		char								Name[128]{};			///< Name
		uint32								ID				= 0;	///< ThreadID (or generic identifier)
		uint32								Index			= 0;	///< Index in Tracks Array
		EType								Type			= EType::CPU;
		
		static constexpr int				MAX_STACK_DEPTH = 32;
		FixedArray<uint32, MAX_STACK_DEPTH> EventStack;
		Array<ProfilerEventData>			Events;
	};
	
	// Register a new track
	int RegisterTrack(const char* pName, EventTrack::EType type, uint32 id);

	URange GetFrameRange() const
	{
		uint32 begin = m_FrameIndex - std::min(m_FrameIndex, m_HistorySize) + 1;
		uint32 end	 = m_FrameIndex;
		return { .Begin = begin, .End = end };
	}

	uint64 GetFirstFrameAnchorTicks() const { return m_BeginFrameTicks[(m_FrameIndex + m_BeginFrameTicks.size() + 1) % m_BeginFrameTicks.size()]; }

	Span<const EventTrack> GetTracks() const { return m_Tracks; }

	void SetEventCallback(const CPUProfilerCallbacks& inCallbacks) { m_EventCallback = inCallbacks; }
	void SetPaused(bool paused) { m_QueuedPaused = paused; }
	bool IsPaused() const { return m_Paused; }

private:
	struct PresentEntry
	{
		constexpr static uint64 sQPCDroppedFrame = ~0ull;		///< QPC when frame is dropped (due to tearing)
		constexpr static uint64 sQPCMissedFrame	 = ~0ull - 1;	///< QPC when frame was missed on CPU (rare)

		uint64 PresentQPC = sQPCDroppedFrame;
		uint64 DisplayQPC = sQPCDroppedFrame;
		uint32 PresentID  = ~0u;
		uint32 FrameIndex = 0;
	};

	int& GetCurrentThreadTrackIndex()
	{
		static thread_local int index = -1;
		return index;
	}

	EventTrack& GetCurrentThreadTrack()
	{
		int index = GetCurrentThreadTrackIndex();
		if (index == -1)
			index = RegisterCurrentThread();
		return m_Tracks[index];
	}

	PresentEntry* GetPresentEntry(uint32 presentID, bool isNewEntry)
	{
		PresentEntry& entry = m_PresentQueue[presentID % m_PresentQueue.size()];
		if (entry.PresentID != presentID && !isNewEntry)
			return nullptr;
		return &entry;
	}

	IDXGISwapChain*				  m_pPresentSwapChain			= nullptr; ///< The swapchain that was last presented with
	int							  m_PresentTrackIndex			= -1;	   ///< The track index for the present timeline
	StaticArray<PresentEntry, 32> m_PresentQueue				= {};	   ///< Queue to register information whenever Present is called
	uint32						  m_LastQueuedPresentID			= 0;	   ///< PresentID after the last Present was called
	uint32						  m_LastQueriedPresentID		= 0;	   ///< The last PresentID which GetFrameStatistics has provided data for
	uint32						  m_LastProcessedValidPresentID = 0;	   ///< The last PresentID which GetFrameStatistics has provided data for
	uint32						  m_LastSyncRefreshCount		= 0;	   ///< The SyncRefreshCount of the last queried frame statistics
	uint32						  m_LastProcessedPresentID		= 0;	   ///< The last PresentID which was processed to an even
	uint32						  m_MsToTicks					= 0;	   ///< The amount of ticks in 1 ms

	friend class SubAllocator;
	ProfilerAllocator			 m_Allocator;
	CPUProfilerCallbacks		 m_EventCallback;
	Mutex						 m_ThreadDataLock;			///< Mutex for accessing thread data
	Array<EventTrack>			 m_Tracks;
	Array<uint64>				 m_BeginFrameTicks;
	uint32						 m_HistorySize	 = 0;		///< History size
	uint32						 m_FrameIndex	 = 0;		///< The current frame index
	bool						 m_Paused		 = false;	///< The current pause state
	bool						 m_QueuedPaused	 = false;	///< The queued pause state
	bool						 m_IsInitialized = false;
};

// Helper RAII-style structure to push and pop a CPU sample region
struct CPUProfileScope
{
	CPUProfileScope(const char* pFunctionName, const char* pFilePath, uint32 lineNumber, const char* pName, uint32 color = 0)
	{
		gProfiler.BeginEvent(pName, color, pFilePath, lineNumber);
	}

	CPUProfileScope(const char* pFunctionName, const char* pFilePath, uint32 lineNumber, uint32 color = 0)
	{
		gProfiler.BeginEvent(pFunctionName, color, pFilePath, lineNumber);
	}

	~CPUProfileScope()
	{
		gProfiler.EndEvent();
	}

	CPUProfileScope(const CPUProfileScope&)			   = delete;
	CPUProfileScope& operator=(const CPUProfileScope&) = delete;
};

#endif