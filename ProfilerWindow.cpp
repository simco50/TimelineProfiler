
#include "Profiler.h"

#if WITH_PROFILING

#include "IconsFontAwesome4.h"
#include "IconsFontAwesome4_data.h"
#include "Roboto_data.h"
#include <fstream>
#include <imgui_internal.h>

#define NOMINMAX
#include <windows.h>

void HandleAssertMessage(const char* pMessage)
{
	printf("%s", pMessage);
	OutputDebugStringA(pMessage);
}

class StringHash
{
private:
	static constexpr uint32 val_const{ 0x811c9dc5 };
	static constexpr uint32 prime_const{ 0x1000193 };

	static inline constexpr uint32 Hash_Internal(const char* const str, const uint32 value) noexcept
	{
		if (!str)
			return 0;
		return (str[0] == '\0') ? value : Hash_Internal(&str[1], (value ^ uint64(str[0])) * prime_const);
	}

public:
	static inline constexpr StringHash Hash(const char* const str) noexcept
	{
		return StringHash(Hash_Internal(str, val_const));
	}

	constexpr StringHash(uint32 value = 0) noexcept
		: m_Hash(value)
	{
	}

	constexpr StringHash(const char* const pText) noexcept
		: m_Hash(Hash_Internal(pText, val_const))
	{
	}

	constexpr void Combine(uint32 other)
	{
		m_Hash ^= other + 0x9e3779b9 + (m_Hash << 6) + (m_Hash >> 2);
	}

	inline constexpr operator uint32() const { return m_Hash; }

	inline bool operator==(const StringHash& rhs) const = default;
	inline bool operator!=(const StringHash& rhs) const = default;
	inline bool operator<(const StringHash& rhs) const	= default;
	inline bool operator>(const StringHash& rhs) const	= default;

	uint32 m_Hash;
};

struct StyleOptions
{
	int	  MaxDepth = 10;
	float MaxTime  = 200;

	float BarHeight		= 1.5f;
	float BarPadding	= 2;
	float ScrollBarSize = 15.0f;

	ImVec4 BarColorMultiplier = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 BGTextColor		  = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
	ImVec4 FGTextColor		  = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
	ImVec4 BarHighlightColor  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

	bool DebugMode = false;

	float GetBarHeight() const
	{
		return BarHeight * ImGui::GetTextLineHeight();
	}
};

struct HUDContext
{
	StyleOptions Style;
	ImFont*		 TextFont = nullptr;
	ImFont*		 IconFont = nullptr;

	float  TimelineScale  = 5.0f;
	ImVec2 TimelineOffset = ImVec2(0.0f, 0.0f);

	bool  IsSelectingRange	  = false;
	float RangeSelectionStart = 0.0f;
	char  SearchString[128]{};
	bool  PauseThreshold	 = false;
	float PauseThresholdTime = 100.0f;
	bool  IsPaused			 = false;

	struct SelectedStatData
	{
		StringHash Hash		  = {};
		uint32	   NumSamples = 0;

		float MovingAverageTime = 0;
		float MinTime			= FLT_MAX;
		float MaxTime			= 0.0f;

		void Set(uint32 hash)
		{
			Hash			  = hash;
			NumSamples		  = 0;
			MovingAverageTime = 0;
			MinTime			  = FLT_MAX;
			MaxTime			  = 0.0f;
		}

		void AddSample(float newSample)
		{
			++NumSamples;
			MinTime			  = ImMin(newSample, MinTime);
			MaxTime			  = ImMax(newSample, MaxTime);
			MovingAverageTime = MovingAverageTime + (newSample - MovingAverageTime) / NumSamples;
			NumSamples %= 4096;
		}

	} SelectedEvent;
};

static HUDContext  gHUDContext;
static HUDContext& Context()
{
	return gHUDContext;
}

static void EditStyle(StyleOptions& style)
{
	ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
	ImGui::SliderInt("Depth", &style.MaxDepth, 1, 12);
	ImGui::SliderFloat("Max Time", &style.MaxTime, 8, 500, "%.1f");
	ImGui::SliderFloat("Bar Height", &style.BarHeight, 1, 4);
	ImGui::SliderFloat("Bar Padding", &style.BarPadding, 0, 5);
	ImGui::SliderFloat("Scroll Bar Size", &style.ScrollBarSize, 1.0f, 40.0f);
	ImGui::ColorEdit4("Bar Color Multiplier", &style.BarColorMultiplier.x);
	ImGui::ColorEdit4("Background Text Color", &style.BGTextColor.x);
	ImGui::ColorEdit4("Foreground Text Color", &style.FGTextColor.x);
	ImGui::ColorEdit4("Bar Highlight Color", &style.BarHighlightColor.x);
	ImGui::Separator();
	ImGui::Checkbox("Debug Mode", &style.DebugMode);
	ImGui::PopItemWidth();
}

static StringHash GetEventHash(const ProfilerEvent& event)
{
	StringHash hash;
	hash.Combine(StringHash(event.pName));
	hash.Combine(StringHash(event.pFilePath));
	hash.Combine(event.LineNumber);
	hash.Combine(event.QueueIndex);
	return hash;
}

template <typename... Args>
static std::string Sprintf(const char* pFormat, Args... args)
{
	static char buffer[1024];
	sprintf_s(buffer, pFormat, std::forward<Args>(args)...);
	return buffer; 
}

struct TraceContext
{
	TraceContext()
	{
		QueryPerformanceCounter((LARGE_INTEGER*)&BaseTime);
	}

	std::ofstream TraceStream;
	uint64		  BaseTime = 0;
};

void BeginTrace(const char* pPath, TraceContext& context)
{
	if (context.TraceStream.is_open())
		return;

	context.TraceStream.open(pPath);
	context.TraceStream << "{\n\"traceEvents\": [\n";

	context.TraceStream << Sprintf("{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":0,\"args\":{\"name\":\"Track\"}},\n");
	for (const Profiler::EventTrack& track : gProfiler.GetTracks())
	{
		context.TraceStream << Sprintf("{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":%d,\"args\":{\"name\":\"%s\"}},\n", track.Index, track.Name);
	}
}

void UpdateTrace(TraceContext& context)
{
	if (!context.TraceStream.is_open())
		return;

	uint64 frequency = 0;
	QueryPerformanceFrequency((LARGE_INTEGER*)&frequency);
	const float TicksToMs = 1000.0f / frequency;

	URange cpuRange = gProfiler.GetFrameRange();
	for (const Profiler::EventTrack& track : gProfiler.GetTracks())
	{
		for (const ProfilerEvent& event : track.GetFrameData(cpuRange.Begin).Events)
			context.TraceStream << Sprintf("{\"pid\":0,\"tid\":%d,\"ts\":%d,\"dur\":%d,\"ph\":\"X\",\"name\":\"%s\"},\n", track.Index, (int)(1000 * TicksToMs * (event.TicksBegin - context.BaseTime)), (int)(1000 * TicksToMs * (event.TicksEnd - event.TicksBegin)), event.pName);
	}
}

void EndTrace(TraceContext& context)
{
	if (!context.TraceStream.is_open())
		return;

	context.TraceStream << "{}]\n}";
	context.TraceStream.close();
}

static void DrawProfilerTimeline(const ImVec2& size = ImVec2(0, 0))
{
	PROFILE_CPU_SCOPE();

	HUDContext&	  context = gHUDContext;
	StyleOptions& style	  = context.Style;

	static TraceContext traceContext;

	UpdateTrace(traceContext);

	ImVec2 sizeActual = ImGui::CalcItemSize(size, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);

	ImRect timelineRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + sizeActual - ImVec2(200, 0));
	ImGui::ItemSize(timelineRect.GetSize());

	// The current (scaled) size of the timeline
	float timelineWidth = timelineRect.GetWidth() * context.TimelineScale;

	ImVec2		cursor		= timelineRect.Min + context.TimelineOffset;
	ImVec2		cursorStart = cursor;
	ImDrawList* pDraw		= ImGui::GetWindowDrawList();

	ImGuiID timelineID = ImGui::GetID("Timeline");
	timelineRect.Max -= ImVec2(style.ScrollBarSize, style.ScrollBarSize);
	if (ImGui::ItemAdd(timelineRect, timelineID))
	{
		ImGui::PushClipRect(timelineRect.Min, timelineRect.Max, true);

		// How many ticks per ms
		uint64 frequency = 0;
		QueryPerformanceFrequency((LARGE_INTEGER*)&frequency);
		const float MsToTicks = (float)frequency / 1000.0f;
		const float TicksToMs = 1000.0f / frequency;

		// How many ticks are in the timeline
		float ticksInTimeline = MsToTicks * style.MaxTime;

		URange cpuRange	   = gProfiler.GetFrameRange();
		uint64 beginAnchor = gProfiler.GetFirstFrameAnchorTicks();

		// How many pixels is one tick
		const float TicksToPixels = timelineWidth / ticksInTimeline;

		// Add vertical bars for each ms interval
		/*
			0	1	2	3
			|	|	|	|
			|	|	|	|
			|	|	|	|
		*/
		pDraw->AddRectFilled(timelineRect.Min, ImVec2(timelineRect.Max.x, timelineRect.Min.y + style.GetBarHeight()), ImColor(0.0f, 0.0f, 0.0f, 0.1f));
		pDraw->AddRect(timelineRect.Min - ImVec2(10, 0), ImVec2(timelineRect.Max.x + 10, timelineRect.Min.y + style.GetBarHeight()), ImColor(1.0f, 1.0f, 1.0f, 0.4f));

		float minIntervalDistance = 80.0f;
		float msWidth			  = 1.0f * MsToTicks * TicksToPixels;
		float intervalSize		  = ceil(minIntervalDistance / msWidth * 2.0f) / 2.0f;

		int markerIdx = 0;
		for (float intervalTime = 0; intervalTime < style.MaxTime; intervalTime += intervalSize, ++markerIdx)
		{
			float  x0	   = intervalTime * MsToTicks * TicksToPixels;
			ImVec2 tickPos = ImVec2(cursor.x + x0, timelineRect.Min.y);
			pDraw->AddLine(tickPos + ImVec2(0, style.GetBarHeight() * 0.5f), tickPos + ImVec2(0, style.GetBarHeight()), ImColor(style.BGTextColor));

			const char* pBarText;
			ImFormatStringToTempBuffer(&pBarText, nullptr, "%.1f ms", intervalTime);
			pDraw->AddText(tickPos + ImVec2(5, 0), ImColor(style.BGTextColor), pBarText);

			if (markerIdx % 2 == 0)
				pDraw->AddRectFilled(tickPos + ImVec2(0, style.GetBarHeight()), tickPos + ImVec2(intervalSize * MsToTicks * TicksToPixels, timelineRect.Max.y), ImColor(1.0f, 1.0f, 1.0f, 0.02f));
		}

		cursor.y += style.GetBarHeight();

		ImGui::PushClipRect(timelineRect.Min + ImVec2(0, style.GetBarHeight()), timelineRect.Max, true);

		ImRect clipRect = ImGui::GetCurrentWindow()->ClipRect;

		// Draw the bars for a list of profiling events
		/*
			[=== SomeFunction (1.2 ms) ===]
		*/
		bool anyHovered = false;
		auto DrawTrack	= [&](Span<const ProfilerEvent> events, uint32 frameIndex, uint32& outTrackDepth) {
			 for (const ProfilerEvent& event : events)
			 {
				 // Skip events above the max depth
				 if (!event.IsValid() || event.Depth >= (uint32)style.MaxDepth)
					 continue;

				 outTrackDepth = ImMax(outTrackDepth, (uint32)event.Depth + 1);

				 bool hovered = false;
				 bool clicked = false;
				 if (event.TicksEnd > beginAnchor)
				 {
					 float	startPos = (event.TicksBegin < beginAnchor ? 0 : event.TicksBegin - beginAnchor) * TicksToPixels;
					 float	endPos	 = (event.TicksEnd - beginAnchor) * TicksToPixels;
					 float	y		 = event.Depth * style.GetBarHeight();
					 ImRect itemRect = ImRect(cursor + ImVec2(startPos, y), cursor + ImVec2(endPos, y + style.GetBarHeight()));

					 // Ensure a bar always has a width
					 itemRect.Max.x = ImMax(itemRect.Max.x, itemRect.Min.x + 1);

					 if (clipRect.Overlaps(itemRect))
					 {
						 float ms = TicksToMs * (float)(event.TicksEnd - event.TicksBegin);

						 ImColor color	   = ImColor(event.GetColor()) * style.BarColorMultiplier;
						 ImColor textColor = style.FGTextColor;
						 // Fade out the bars that don't match the filter
						 if (context.SearchString[0] != 0 && !strstr(event.pName, context.SearchString))
						 {
							 color.Value.w *= 0.3f;
							 textColor.Value.w *= 0.5f;
						 }
						 else if (context.PauseThreshold && ms >= context.PauseThresholdTime)
						 {
							 gProfiler.SetPaused(true);
							 gGPUProfiler.SetPaused(true);
						 }

						 // Darken the bottom
						 ImColor colorBottom = color.Value * ImVec4(0.8f, 0.8f, 0.8f, 1.0f);

						 if (!anyHovered && ImGui::IsMouseHoveringRect(itemRect.Min, itemRect.Max))
						 {
							 hovered	= true;
							 anyHovered = true;

							 if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
							 {
								 clicked = true;
							 }

							 // If the bar is double-clicked, zoom in to make the bar fill the entire window
							 if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
							 {
								 // Zoom ration to make the bar fit the entire window
								 float zoom			   = timelineWidth / itemRect.GetWidth();
								 context.TimelineScale = zoom;

								 // Recompute the timeline size with new zoom
								 float newTimelineWidth = timelineRect.GetWidth() * context.TimelineScale;
								 float newTickScale		= newTimelineWidth / ticksInTimeline;
								 float newStartPos		= newTickScale * (event.TicksBegin - beginAnchor);

								 context.TimelineOffset.x = -newStartPos;
							 }
						 }

						 // Draw the bar rect and outline if hovered

						 // Only pad if bar is large enough
						 float		  maxPaddingX = ImMax(itemRect.GetWidth() * 0.5f - 1.0f, 0.0f);
						 const ImVec2 padding(ImMin(style.BarPadding, maxPaddingX), style.BarPadding);
						 if (hovered)
						 {
							 ImColor highlightColor = color.Value * ImVec4(1.5f, 1.5f, 1.5f, 1.0f);
							 color.Value			= color.Value * ImVec4(1.2f, 1.2f, 1.2f, 1.0f);
							 colorBottom.Value		= colorBottom.Value * ImVec4(1.2f, 1.2f, 1.2f, 1.0f);
							 pDraw->AddRectFilledMultiColor(itemRect.Min + padding, itemRect.Max - padding, color, color, colorBottom, colorBottom);
							 pDraw->AddRect(itemRect.Min, itemRect.Max, highlightColor, 0.0f, ImDrawFlags_None, 3.0f);
						 }
						 else
						 {
							 pDraw->AddRectFilledMultiColor(itemRect.Min + padding, itemRect.Max - padding, color, color, colorBottom, colorBottom);
						 }

						 // If the bar size is large enough, draw the name of the bar on top
						 if (itemRect.GetWidth() > 10.0f)
						 {
							 const char* pBarText;
							 ImFormatStringToTempBuffer(&pBarText, nullptr, "%s (%.2f ms)", event.pName, ms);
							 ImVec2 textSize = ImGui::CalcTextSize(pBarText);

							 itemRect.Expand(ImVec2(-2.0f, 0.0f));

							 ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.5f));
							 itemRect.Translate(ImVec2(2.0f, 2.0f));
							 ImGui::RenderTextEllipsis(pDraw, itemRect.Min + ImMax(ImVec2(), ImVec2(itemRect.GetWidth(), style.GetBarHeight()) - textSize) * 0.5f, itemRect.Max, itemRect.Max.x, pBarText, nullptr, &textSize);
							 ImGui::PopStyleColor();

							 itemRect.Translate(ImVec2(-2.0f, -2.0f));
							 ImGui::RenderTextEllipsis(pDraw, itemRect.Min + ImMax(ImVec2(), ImVec2(itemRect.GetWidth(), style.GetBarHeight()) - textSize) * 0.5f, itemRect.Max, itemRect.Max.x, pBarText, nullptr, &textSize);
						 }
					 }
				 }

				 if (hovered)
				 {
					 if (ImGui::BeginTooltip())
					 {
						 ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "%s | %.3f ms", event.pName, TicksToMs * (float)(event.TicksEnd - event.TicksBegin));
						 ImGui::Text("Frame %d", frameIndex);
						 if (event.pFilePath && *event.pFilePath != 0)
							 ImGui::Text("%s:%d", event.pFilePath, event.LineNumber);
						 ImGui::EndTooltip();
					 }
				 }
				 if (clicked)
				 {
					 StringHash eventHash = GetEventHash(event);
					 context.SelectedEvent.Set(GetEventHash(event));
				 }
			 }
		};

		// Add track name and expander
		/*
			(>) Main Thread [1234]
		*/
		auto TrackHeader = [&](const char* pName, uint32 id) {
			pDraw->AddRectFilled(ImVec2(timelineRect.Min.x, cursor.y), ImVec2(timelineRect.Max.x, cursor.y + style.GetBarHeight()), ImColor(0.0f, 0.0f, 0.0f, 0.3f));

			bool   isOpen		   = ImGui::GetCurrentWindow()->StateStorage.GetBool(id, true);
			ImVec2 trackTextCursor = ImVec2(timelineRect.Min.x, cursor.y);

			float caretSize = ImGui::GetTextLineHeight();
			if (ImGui::ItemAdd(ImRect(trackTextCursor, trackTextCursor + ImVec2(caretSize, caretSize)), id))
			{
				if (ImGui::IsItemHovered())
					pDraw->AddRect(ImGui::GetItemRectMin() + ImVec2(2, 2), ImGui::GetItemRectMax() - ImVec2(2, 2), ImColor(style.BGTextColor), 3.0f);
				pDraw->AddText(ImGui::GetItemRectMin() + ImVec2(2, 2), ImColor(style.BGTextColor), isOpen ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
				if (ImGui::ButtonBehavior(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), id, nullptr, nullptr, ImGuiButtonFlags_MouseButtonLeft))
				{
					isOpen = !isOpen;
					ImGui::GetCurrentWindow()->StateStorage.SetBool(id, isOpen);
				}
			}

			trackTextCursor.x += caretSize;
			pDraw->AddText(trackTextCursor, ImColor(style.BGTextColor), pName);
			cursor.y += style.GetBarHeight();
			return isOpen;
		};

		// Draw each track
		Span<const Profiler::EventTrack> tracks = gProfiler.GetTracks();

		// Sort by track type
		Array<const Profiler::EventTrack*> sorted_tracks;
		for (const Profiler::EventTrack& track : tracks)
			sorted_tracks.push_back(&track);
		std::sort(sorted_tracks.begin(), sorted_tracks.end(), [](const Profiler::EventTrack* a, const Profiler::EventTrack* b) {
			return (int)a->Type > (int)b->Type;
		});

		for (const Profiler::EventTrack* pTrack : sorted_tracks)
		{
			PROFILE_CPU_SCOPE("Timeline Track");

			// Add thread name for track
			const char* pHeaderText;
			ImFormatStringToTempBuffer(&pHeaderText, nullptr, "%s [%d]", pTrack->Name, pTrack->ID);

			if (TrackHeader(pHeaderText, ImGui::GetID(pTrack)))
			{
				uint32 trackDepth = 0;

				// Add a bar in the right place for each event
				/*
					|[=============]			|
					|	[======]				|
				*/
				for (uint32 frameIndex = cpuRange.Begin; frameIndex < cpuRange.End; ++frameIndex)
				{
					DrawTrack(pTrack->GetFrameData(frameIndex).Events, frameIndex, trackDepth);
				}
				cursor.y += trackDepth * style.GetBarHeight();
			}

			// Add vertical line to end track
			pDraw->AddLine(ImVec2(timelineRect.Min.x, cursor.y), ImVec2(timelineRect.Max.x, cursor.y), ImColor(style.BGTextColor));
		}

		// The final height of the timeline
		float timelineHeight = cursor.y - cursorStart.y;

		if (ImGui::IsWindowFocused())
		{
			// Profile range
			// If not currently in selection, start selection when left mouse button is pressed
			if (!context.IsSelectingRange && ImGui::IsMouseHoveringRect(timelineRect.Min, timelineRect.Max))
			{
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					context.RangeSelectionStart = ImGui::GetMousePos().x;
					context.IsSelectingRange	= true;
				}
			}
			else if (context.IsSelectingRange)
			{
				// If mouse button is released, exit measuring mode
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					context.IsSelectingRange = false;
				}
				else
				{
					// Distance between mouse cursor and measuring start
					float distance = fabs(ImGui::GetMousePos().x - context.RangeSelectionStart);

					// Fade in based on distance
					float opacity = ImClamp(distance / 30.0f, 0.0f, 1.0f);
					if (opacity > 0.0f)
					{
						float time = (distance / TicksToPixels) * TicksToMs;

						// Draw measure region
						pDraw->AddRectFilled(ImVec2(context.RangeSelectionStart, timelineRect.Min.y), ImVec2(ImGui::GetMousePos().x, timelineRect.Max.y), ImColor(1.0f, 1.0f, 1.0f, 0.1f));
						pDraw->AddLine(ImVec2(context.RangeSelectionStart, timelineRect.Min.y), ImVec2(context.RangeSelectionStart, timelineRect.Max.y), ImColor(1.0f, 1.0f, 1.0f, 0.3f), 3.0f);
						pDraw->AddLine(ImVec2(ImGui::GetMousePos().x, timelineRect.Min.y), ImVec2(ImGui::GetMousePos().x, timelineRect.Max.y), ImColor(1.0f, 1.0f, 1.0f, 0.3f), 3.0f);

						// Add line and arrows
						ImColor measureColor = style.FGTextColor;
						measureColor.Value.w *= opacity;
						ImVec2 lineStart = ImVec2(context.RangeSelectionStart, ImGui::GetMousePos().y);
						ImVec2 lineEnd	 = ImGui::GetMousePos();
						if (lineStart.x > lineEnd.x)
							std::swap(lineStart.x, lineEnd.x);
						pDraw->AddLine(lineStart, lineEnd, measureColor);
						pDraw->AddLine(lineStart, lineStart + ImVec2(5, 5), measureColor);
						pDraw->AddLine(lineStart, lineStart + ImVec2(5, -5), measureColor);
						pDraw->AddLine(lineEnd, lineEnd + ImVec2(-5, 5), measureColor);
						pDraw->AddLine(lineEnd, lineEnd + ImVec2(-5, -5), measureColor);

						// Add text in the middle
						const char* pTimeText;
						ImFormatStringToTempBuffer(&pTimeText, nullptr, "Time: %.3f ms", time);
						ImVec2 textSize = ImGui::CalcTextSize(pTimeText);
						pDraw->AddText((lineEnd + lineStart) / 2 - ImVec2(textSize.x * 0.5f, textSize.y), measureColor, pTimeText);
					}
				}
			}

			// Zoom behavior
			float zoomDelta = 0.0f;
			if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))
				zoomDelta += ImGui::GetIO().MouseWheel / 5.0f;

			if (zoomDelta != 0)
			{
				// Logarithmic scale
				float logScale = logf(context.TimelineScale);
				logScale += zoomDelta;
				float newScale = ImClamp(expf(logScale), 1.0f, 100.0f);

				float scaleFactor = newScale / context.TimelineScale;
				context.TimelineScale *= scaleFactor;
				ImVec2 mousePos			 = ImGui::GetMousePos() - timelineRect.Min;
				context.TimelineOffset.x = mousePos.x - (mousePos.x - context.TimelineOffset.x) * scaleFactor;
			}
		}

		// Panning behavior
		bool held;
		ImGui::ButtonBehavior(timelineRect, timelineID, nullptr, &held, ImGuiButtonFlags_MouseButtonRight);
		if (held)
			context.TimelineOffset += ImGui::GetIO().MouseDelta;

		// Compute the new timeline size to correctly clamp the offset
		timelineWidth		   = timelineRect.GetWidth() * context.TimelineScale;
		context.TimelineOffset = ImClamp(context.TimelineOffset, ImMin(ImVec2(0.0f, 0.0f), timelineRect.GetSize() - ImVec2(timelineWidth, timelineHeight)), ImVec2(0.0f, 0.0f));

		ImGui::PopClipRect();
		ImGui::PopClipRect();

		// Draw a debug rect around the timeline item and the whole (unclipped) timeline rect
		if (style.DebugMode)
		{
			pDraw->PushClipRectFullScreen();
			pDraw->AddRect(cursorStart, cursorStart + ImVec2(timelineWidth, timelineHeight), ImColor(1.0f, 0.0f, 0.0f), 0.0f, ImDrawFlags_None, 3.0f);
			pDraw->AddRect(timelineRect.Min, timelineRect.Max, ImColor(0.0f, 1.0f, 0.0f), 0.0f, ImDrawFlags_None, 2.0f);
			pDraw->PopClipRect();
		}

		// Add extra data to tooltip
		ImGui::SameLine();

		ImGui::BeginGroup();

		const char* pTracePath = "trace.json";
		if (!traceContext.TraceStream.is_open())
		{
			if (ImGui::Button("Begin Trace", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			{
				BeginTrace(pTracePath, traceContext);
			}
		}
		else
		{
			if (ImGui::Button("End Trace", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			{
				EndTrace(traceContext);
			}
		}

		HUDContext::SelectedStatData& selectedEvent = context.SelectedEvent;
		if ((uint32)selectedEvent.Hash != 0)
		{
			const char* pName	  = "";
			float		eventTime = 0;
			uint32		n		  = 0;
			for (uint32 i = cpuRange.Begin; i < cpuRange.End; ++i)
			{
				for (const Profiler::EventTrack& track : gProfiler.GetTracks())
				{
					const ProfilerEventData& eventData = track.GetFrameData(i);
					for (const ProfilerEvent& event : eventData.Events)
					{
						if (GetEventHash(event) == selectedEvent.Hash)
						{
							float time = TicksToMs * (float)(event.TicksEnd - event.TicksBegin);
							selectedEvent.AddSample(time);
							pName	  = event.pName;
							eventTime = time;
							++n;
						}
					}
				}
			}
			
			if (eventTime)
			{
				ImGui::Text(pName);
				if (ImGui::BeginTable("TooltipTable", 2))
				{
					ImGui::TableNextColumn();
					ImGui::Text("Time:");
					ImGui::TableNextColumn();
					ImGui::Text("%.2f ms", eventTime);

					ImGui::TableNextColumn();
					ImGui::Text("Occurances:");
					ImGui::TableNextColumn();
					ImGui::Text("%d", n);

					ImGui::TableNextColumn();
					ImGui::Text("Moving Average:");
					ImGui::TableNextColumn();
					ImGui::Text("%.2f ms", selectedEvent.MovingAverageTime);

					ImGui::TableNextColumn();
					ImGui::Text("Min/Max:");
					ImGui::TableNextColumn();
					ImGui::Text("%.2f/%.2f ms", selectedEvent.MinTime, selectedEvent.MaxTime);

					ImGui::EndTable();
				}
			}
		}
		ImGui::EndGroup();

		// Horizontal scroll bar
		ImS64 scrollH = -(ImS64)context.TimelineOffset.x;
		ImGui::ScrollbarEx(ImRect(ImVec2(timelineRect.Min.x, timelineRect.Max.y), ImVec2(timelineRect.Max.x + style.ScrollBarSize, timelineRect.Max.y + style.ScrollBarSize)), ImGui::GetID("ScrollH"), ImGuiAxis_X, &scrollH, (ImS64)timelineRect.GetSize().x, (ImS64)timelineWidth, ImDrawFlags_None);
		context.TimelineOffset.x = -(float)scrollH;

		// Vertical scroll bar
		ImS64 scrollV = -(ImS64)context.TimelineOffset.y;
		ImGui::ScrollbarEx(ImRect(ImVec2(timelineRect.Max.x, timelineRect.Min.y), ImVec2(timelineRect.Max.x + style.ScrollBarSize, timelineRect.Max.y)), ImGui::GetID("ScrollV"), ImGuiAxis_Y, &scrollV, (ImS64)timelineRect.GetSize().y, (ImS64)timelineHeight, ImDrawFlags_None);
		context.TimelineOffset.y = -(float)scrollV;
	}
}


void DrawProfilerHUD()
{
	HUDContext&	  context = Context();
	StyleOptions& style	  = context.Style;

	if (!context.IconFont)
	{
		{
			ImFontConfig fontConfig;
			fontConfig.MergeMode = false;
			strcpy_s(fontConfig.Name, "Roboto-Regular");
			context.TextFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(roboto_regular_compressed_data, roboto_regular_compressed_size, 0.0f, &fontConfig);
		}

		{
			ImFontConfig fontConfig;
			fontConfig.MergeMode = true;
			strcpy_s(fontConfig.Name, "FontAwesome");
			static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
			context.IconFont				   = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(font_awesome_compressed_data, font_awesome_compressed_size, 0.0f, &fontConfig, icon_ranges);
		}
	}

	ImGui::PushFont(context.TextFont);
	ImGui::PushStyleColor(ImGuiCol_Text, style.FGTextColor);

	if (gProfiler.IsPaused())
		ImGui::Text("Paused");
	else
		ImGui::Text("Press Space to pause");

	ImGui::SameLine();

	ImGui::Checkbox("Pause threshold", &Context().PauseThreshold);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150);
	ImGui::SliderFloat("##Treshold", &context.PauseThresholdTime, 0.0f, 16.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
	ImGui::SameLine();

	ImGui::Dummy(ImVec2(30, 0));
	ImGui::SameLine();

	ImGui::Text("Filter");
	ImGui::SetNextItemWidth(150);
	ImGui::SameLine();
	ImGui::InputText("##Search", context.SearchString, ARRAYSIZE(context.SearchString));
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_TIMES "##clearfilter"))
		context.SearchString[0] = 0;
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_PAINT_BRUSH "##styleeditor"))
		ImGui::OpenPopup("Style Editor");

	if (ImGui::BeginPopup("Style Editor"))
	{
		EditStyle(style);
		ImGui::EndPopup();
	}

	if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Space))
		context.IsPaused = !context.IsPaused;

	if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		ImGui::SetWindowFocus();

	gProfiler.SetPaused(context.IsPaused);
	gGPUProfiler.SetPaused(context.IsPaused);

	DrawProfilerTimeline(ImVec2(0, 0));

	ImGui::PopStyleColor();
	ImGui::PopFont();
}

#endif
