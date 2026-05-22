#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "p2p.h"
#include "p2p_api.h"
#include "gzip/compress.hpp"
#include "gzip/decompress.hpp"
#include "ImGuiFileDialog.h"
#include "json.hpp"
#include "vapoursynth/VSScript4.h"
#include "vapoursynth/VSHelper4.h"
#include "Walnut/Image.h"
#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include <fstream>
#include <string>
#include <stdio.h>
#include <algorithm>
#include <vector>
#include <map>
#include <memory>
#include <GLFW/glfw3.h> // For drag-n-drop files
#include <iostream>

#include "icon.h"
#include "imgui_stdlib.h"

using nlohmann::json;

// --- SAFE LOGGER ---
static void APP_LOG(const std::string& msg) {
	FILE* f = fopen("app_trace_log.txt", "a");
	if (f) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg.c_str());
		fclose(f);
	}
	OutputDebugStringA(msg.c_str());
	OutputDebugStringA("\n");
}
// -------------------

ImFont* g_UbuntuMonoFont = nullptr;

// From vapoursynth sdk's vsscript_example.c
typedef VS_CC const VSSCRIPTAPI *(*getVSScriptAPIType)(int);
typedef VS_CC const char *(*getVSScriptAPILastErrorType)();

getVSScriptAPIType getVSScriptAPIFunc = NULL;
getVSScriptAPILastErrorType getVSScriptAPILastErrorFunc = NULL;

static int loadVSScriptLibrary() {
	APP_LOG("Attempting to load VSScript library...");
#ifdef _WIN32
	const wchar_t *vsscriptPath = _wgetenv(L"VSSCRIPT_PATH");
	HMODULE lib = LoadLibraryExW(vsscriptPath ? vsscriptPath : L"VSScript.dll", NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!lib) {
		APP_LOG("ERROR: Failed to load VSScript library (VSScript.dll)!");
		return 1;
	}
	getVSScriptAPIFunc = (getVSScriptAPIType)GetProcAddress(lib, "getVSScriptAPI");
	getVSScriptAPILastErrorFunc = (getVSScriptAPILastErrorType)GetProcAddress(lib, "getVSScriptAPILastError");
	if (!getVSScriptAPIFunc || !getVSScriptAPILastErrorFunc) {
		APP_LOG("ERROR: Failed to locate entry points in VSScript library!");
		return 1;
	}
	APP_LOG("SUCCESS: VSScript library loaded.");
	return 0;
#else
	APP_LOG("Non-Windows OS detected. Attempting to load dlopen...");
	const char *vsscriptPath = getenv("VSSCRIPT_PATH");
#ifdef __APPLE__
	const char *defaultLibName = "libvsscript.4.dylib";
#else
	const char *defaultLibName = "libvsscript.so.4";
#endif
	void *lib = dlopen(vsscriptPath ? vsscriptPath : defaultLibName, RTLD_LAZY | RTLD_GLOBAL);
	if (!lib) {
		APP_LOG("ERROR: Failed to load VSScript library: " + std::string(dlerror()));
		return 1;
	}
	getVSScriptAPIFunc = (getVSScriptAPIType)dlsym(lib, "getVSScriptAPI");
	getVSScriptAPILastErrorFunc = (getVSScriptAPILastErrorType)dlsym(lib, "getVSScriptAPILastError");
	if (!getVSScriptAPIFunc || !getVSScriptAPILastErrorFunc) {
		APP_LOG("ERROR: Failed to locate entry points in VSScript library: " + std::string(dlerror()));
		return 1;
	}
	return 0;
#endif
}

static void HelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

static ImU32 ColorForAction(const int_fast8_t action) {
	static const std::map<int_fast8_t, ImU32> map = {
		{0, IM_COL32(178, 34, 34, 200)},    
		{1, IM_COL32(178, 34, 34, 200)},    
		{2, IM_COL32(0, 0, 205, 200)},      
		{3, IM_COL32(0, 0, 205, 200)},      
		{4, IM_COL32(0, 128, 0, 200)},      
		{5, IM_COL32(0, 128, 0, 200)},      
		{6, IM_COL32(148, 0, 211, 200)},    
		{7, IM_COL32(148, 0, 211, 200)},    
		{8, IM_COL32(192, 192, 192, 200) }, 
		{9, IM_COL32(255, 128, 0, 255)},    
	};
	return map.at(action);
}

struct TextCallbackData {
	const int activeFrame;
	void* layer;
};

enum NoMatchHandling {
	PREVIOUS,
	NEXT
};

class ExampleLayer : public Walnut::Layer
{
public:
	virtual void OnAttach() override {
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	}

	virtual void OnUIRender() override {
		ImGuiIO& io = ImGui::GetIO();
		static int error = 0;
		static char error_message[1024];

		if (ImGuiFileDialog::Instance()->Display("NewProjectDialog", ImGuiWindowFlags_NoCollapse, ImVec2(500, 400))) {
			if (ImGuiFileDialog::Instance()->IsOk()) {
				std::string scriptPathName = ImGuiFileDialog::Instance()->GetFilePathName();
				StartNewProject(scriptPathName.c_str());
			}
			ImGuiFileDialog::Instance()->Close();
		}

		if (ImGuiFileDialog::Instance()->Display("OpenProjectDialog", ImGuiWindowFlags_NoCollapse, ImVec2(500, 400))) {
			if (ImGuiFileDialog::Instance()->IsOk()) {
				std::string projectPathName = ImGuiFileDialog::Instance()->GetFilePathName();
				OpenProject(projectPathName.c_str());
			}
			ImGuiFileDialog::Instance()->Close();
		}

		if (ImGuiFileDialog::Instance()->Display("SaveProjectAsDialog", ImGuiWindowFlags_NoCollapse, ImVec2(500, 400))) {
			if (ImGuiFileDialog::Instance()->IsOk()) {
				std::string projectPathName = ImGuiFileDialog::Instance()->GetFilePathName();
				m_ProjectFile = projectPathName;
				SaveJson();
			}
			ImGuiFileDialog::Instance()->Close();
		}

		if (ImGuiFileDialog::Instance()->IsOpened()) return;

		// Prevent rendering crash if VSAPI failed to initialize completely
		if (!m_VSAPI) {
			ImGui::Begin("Fields"); ImGui::Text("CRITICAL ERROR: VapourSynth API is not initialized."); ImGui::End();
			return;
		}

		static int last_cycle = -1;
		m_NeedNewFields |= last_cycle != m_ActiveCycle;
		last_cycle = m_ActiveCycle;

		int max_cycle = (m_FieldsFrameCount - 1) / 10;

		if (!io.WantCaptureKeyboard) { 
			if (m_ActiveCycle < max_cycle && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_J))) m_ActiveCycle++;
			if (m_ActiveCycle > 0 && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_K))) m_ActiveCycle--;
			if (ImGui::IsKeyPressed(ImGuiKey_R)) LoadFrames();
			if (ImGui::IsKeyPressed(ImGuiKey_T)) { ApplyCycleToScene(); AutoLoadFrames(); }
		}

		if (m_WantNewFrames) LoadFrames();

		ImGui::Begin("Fields");

		int remaining_fields = m_FieldsFrameCount - (m_ActiveCycle * 10);
		int fields_in_cycle = std::min(remaining_fields, 11);

		for (int i = 0; i < fields_in_cycle; i++) {
			if (m_Fields[i] == nullptr) continue;

			if (m_NeedNewFields && !error) {
				APP_LOG("Fetching Frame " + std::to_string(m_ActiveCycle * 10 + i) + " for Field Node");
				if (!m_FieldsNode) { APP_LOG("ERROR: m_FieldsNode is NULL!"); error = 1; continue; }

				const VSFrame* frame = m_VSAPI->getFrame(m_ActiveCycle * 10 + i, m_FieldsNode, error_message, sizeof(error_message));
				if (!frame) {
					APP_LOG("ERROR: Failed to get frame for Field " + std::to_string(m_ActiveCycle * 10 + i) + ": " + std::string(error_message));
					error = 1; continue;
				}

				APP_LOG("Allocating memory for Field Frame Buffer");
				uint8_t* imageBuffer = (uint8_t*)malloc(m_FieldsWidth * m_FieldsHeight * 4);
				if (!imageBuffer) {
					APP_LOG("CRITICAL: malloc failed for Field imageBuffer!");
					m_VSAPI->freeFrame(frame);
					error = 1; continue;
				}

				p2p_buffer_param p = {};
				p.packing = p2p_rgba32_be;
				p.width = m_FieldsWidth;
				p.height = m_FieldsHeight;
				p.dst[0] = imageBuffer;
				p.dst_stride[0] = m_FieldsWidth * 4;
				for (int plane = 0; plane < 3; plane++) {
					p.src[plane] = m_VSAPI->getReadPtr(frame, plane);
					p.src_stride[plane] = m_VSAPI->getStride(frame, plane);
				}
				p2p_pack_frame(&p, P2P_ALPHA_SET_ONE);

				APP_LOG("Uploading Field Frame Buffer to GPU texture");
				m_Fields[i]->SetData(imageBuffer);
				m_VSAPI->freeFrame(frame);
				free(imageBuffer);
				APP_LOG("Field Frame processing complete.");
			}
		}

		if (ImGui::BeginTable("field table", 6, ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextRow();
			for (int i = 0; i < fields_in_cycle; i += 2) {
				ImGui::TableNextColumn();
				if (m_Fields[i] != nullptr) {
					float fieldDisplayWidth = ImGui::GetContentRegionAvail().x;
					float fieldDisplayHeight = m_FramesWidth ? fieldDisplayWidth * ((float)m_FramesHeight / m_FramesWidth) : 0;
					DrawField(i, fieldDisplayWidth, fieldDisplayHeight);
				}
			}
			ImGui::TableNextRow();
			for (int i = 1; i < std::min(fields_in_cycle, 10); i += 2) {
				ImGui::TableNextColumn();
				if (m_Fields[i] != nullptr) {
					float fieldDisplayWidth = ImGui::GetContentRegionAvail().x;
					float fieldDisplayHeight = m_FramesWidth ? fieldDisplayWidth * ((float)m_FramesHeight / m_FramesWidth) : 0;
					DrawField(i, fieldDisplayWidth, fieldDisplayHeight);
				}
			}
			ImGui::EndTable();
		}
		ImGui::End();

		ImGui::Begin("Output");
		int frames_in_cycle = fields_in_cycle * 4 / 10;
		if (ImGui::BeginTable("frame table", 4, ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextRow();
			for (int i = 0; i < frames_in_cycle; i++) {
				if (m_Frames[i] == nullptr) continue;

				const int activeFrame = m_ActiveCycle * 4 + i;
				if (activeFrame >= m_FramesFrameCount) break;

				if (m_NeedNewFields && !error) {
					APP_LOG("Fetching Frame " + std::to_string(activeFrame) + " for Output Node");
					if (!m_FramesNode) { APP_LOG("ERROR: m_FramesNode is NULL!"); error = 1; continue; }

					const VSFrame* frame = m_VSAPI->getFrame(activeFrame, m_FramesNode, error_message, sizeof(error_message));
					if (!frame) {
						APP_LOG("ERROR: Failed to get frame for Frame Node " + std::to_string(activeFrame) + ": " + std::string(error_message));
						error = 1; continue;
					}

					APP_LOG("Allocating memory for Output Frame Buffer");
					uint8_t* imageBuffer = (uint8_t*)malloc(m_FramesWidth * m_FramesHeight * 4);
					if (!imageBuffer) {
						APP_LOG("CRITICAL: malloc failed for Output imageBuffer!");
						m_VSAPI->freeFrame(frame);
						error = 1; continue;
					}

					p2p_buffer_param p = {};
					p.packing = p2p_rgba32_be;
					p.width = m_FramesWidth;
					p.height = m_FramesHeight;
					p.dst[0] = imageBuffer;
					p.dst_stride[0] = m_FramesWidth * 4;
					for (int plane = 0; plane < 3; plane++) {
						p.src[plane] = m_VSAPI->getReadPtr(frame, plane);
						p.src_stride[plane] = m_VSAPI->getStride(frame, plane);
					}
					p2p_pack_frame(&p, P2P_ALPHA_SET_ONE);
					
					APP_LOG("Getting Frame Properties");
					const VSMap* props = m_VSAPI->getFramePropertiesRO(frame);
					if (!props) {
						APP_LOG("WARNING: Frame Properties is NULL!");
					} else {
						int err = 0;
						m_FieldCount[i] = m_VSAPI->mapGetInt(props, "IVTCDN_Fields", 0, &err);
						const char* freezeFrameProp = m_VSAPI->mapGetData(props, "IVTCDN_FreezeFrame", 0, &err);
						m_FreezeFrames[i] = err ? "" : freezeFrameProp;
						
						if (m_VSAPI->mapNumElements(props, "VMetrics") == 2) {
							const int64_t* vmetrics = m_VSAPI->mapGetIntArray(props, "VMetrics", &err);
							m_CombedMetrics[i] = err ? -1 : vmetrics[1];
						}
					}

					APP_LOG("Uploading Output Frame Buffer to GPU texture");
					m_Frames[i]->SetData(imageBuffer);
					
					m_VSAPI->freeFrame(frame);
					free(imageBuffer);
					APP_LOG("Output Frame processing complete.");
				}
				ImGui::TableNextColumn();
				float frameDisplayWidth = ImGui::GetContentRegionAvail().x;
				float frameDisplayHeight = m_FramesWidth ? frameDisplayWidth * ((float)m_FramesHeight / m_FramesWidth) : 0;
				DrawFrame(i, frameDisplayWidth, frameDisplayHeight);
			}
			ImGui::TableNextRow();
			for (int i = 0; i < frames_in_cycle; i++) {
				ImGui::TableNextColumn();
				if (m_CombedDetection) ImGui::Text("Combed Metric: %d", m_CombedMetrics[i]);
				ImGui::Text("Matched Fields: %d", m_FieldCount[i]);
			}
			ImGui::EndTable();
		}
		ImGui::End();

		ImGui::Begin("Extra Attributes");
		if (ImGui::BeginTable("attribute table", 4, ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextRow();
			static const char* property_labels[11] = { "##properties 0", "##properties 1", "##properties 2", "##properties 3", "##properties 4", "##properties 5", "##properties 6", "##properties 7", "##properties 8", "##properties 9", "##properties 10" };
			const float input_height = std::max(ImGui::GetContentRegionAvail().y - 8, ImGui::GetTextLineHeight() * 4);
			for (int i = 0; i < frames_in_cycle; i++) {
				ImGui::TableNextColumn();
				const int activeFrame = m_ActiveCycle * 4 + i;
				auto& extra_attributes = m_JsonProps["extra_attributes"];
				const std::string activeFrameKey = std::to_string(activeFrame);
				std::string input;
				if (!extra_attributes.contains(activeFrameKey)) input = std::string();
				else input = extra_attributes[activeFrameKey];
				auto textCallbackData = TextCallbackData{ activeFrame, this };
				ImGui::InputTextMultiline(property_labels[i], &input, ImVec2(-FLT_MIN, input_height), ImGuiInputTextFlags_CallbackEdit, AttributeCallback, &textCallbackData);
			}
			ImGui::EndTable();
		}
		ImGui::End();

		ImGui::Begin("Navigation");
		ImGui::SliderInt("Active Cycle", &m_ActiveCycle, 0, max_cycle, nullptr, ImGuiSliderFlags_AlwaysClamp);
		ImGui::SameLine(); HelpMarker("CTRL+click to input value.");
		ImGui::End();

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) SaveJson();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) OpenProjectDialog();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) NewProjectDialog();

		m_NeedNewFields = false;
	}

	ExampleLayer() {
		APP_LOG("Starting ExampleLayer Initialization...");
		int err = loadVSScriptLibrary();
		if (err) {
			APP_LOG("WARNING: loadVSScriptLibrary returned error. Entry points may be missing.");
		}
		
		if (getVSScriptAPIFunc) {
			m_VSSAPI = getVSScriptAPIFunc(VSSCRIPT_API_VERSION);
		}
		
		if (!m_VSSAPI) {
			APP_LOG("CRITICAL ERROR: Failed to get VSSCRIPT_API_VERSION! m_VSSAPI is NULL.");
			return; // Gracefully fail
		}

		APP_LOG("Getting VSAPI...");
		m_VSAPI = m_VSSAPI->getVSAPI(VAPOURSYNTH_API_VERSION);
		if(!m_VSAPI) {
			APP_LOG("CRITICAL ERROR: Failed to get VAPOURSYNTH_API_VERSION! m_VSAPI is NULL.");
			return;
		}
		APP_LOG("VSAPI Successfully Initialized.");
	}

	int static AttributeCallback(ImGuiInputTextCallbackData* data) {
		auto cbData = (TextCallbackData*)data->UserData;
		auto activeFrame = cbData->activeFrame;
		auto* layer = (ExampleLayer*)cbData->layer;
		auto& extra_attributes = layer->m_JsonProps["extra_attributes"];
		const std::string activeFrameKey = std::to_string(activeFrame);
		auto text = std::string(data->Buf);
		if (text.empty() || std::all_of(text.begin(), text.end(), isspace)) extra_attributes.erase(activeFrameKey);
		else extra_attributes[activeFrameKey] = text;
		return 0;
	}

	void OpenProjectDialog() {
		auto path = IGFD::Utils::ParsePathFileName(m_ProjectFile);
		ImGuiFileDialog::Instance()->OpenModal("OpenProjectDialog", "Choose project file", ".ivtc", path.path + "/.");
	}
	void NewProjectDialog() {
		auto path = IGFD::Utils::ParsePathFileName(m_ProjectFile);
		ImGuiFileDialog::Instance()->OpenModal("NewProjectDialog", "Choose script file", ".vpy", path.path + "/.");
	}
	void SaveProjectAsDialog() {
		auto path = IGFD::Utils::ParsePathFileName(m_ProjectFile);
		ImGuiFileDialog::Instance()->OpenModal("SaveProjectAsDialog", "Choose project file", ".ivtc", path.path + "/.");
	}

	template <typename ValueType> static ValueType SetDefault(json& object, std::string attribute, ValueType defaultValue) {
		if (!object.contains(attribute)) object[attribute] = defaultValue;
		return object[attribute];
	}

	void OpenProject(const char* project_path_name) {
		APP_LOG("Opening Project: " + std::string(project_path_name));
		m_ProjectFile = std::string(project_path_name);
		std::ifstream input(m_ProjectFile, std::ios::binary | std::ios::ate);
		int inputSize = input.tellg();
		input.seekg(0, std::ios::beg);

		std::vector<char> compressed; compressed.resize(inputSize);
		input.read(compressed.data(), inputSize);
		std::string decompressed = gzip::decompress(compressed.data(), inputSize);
		m_JsonProps = json::parse(decompressed);
		
		SetDefault(m_JsonProps, "no_match_handling", json::object());
		SetDefault(m_JsonProps, "no_match_handling_default", std::string("Previous"));
		m_NoMatchHandling = (m_JsonProps["no_match_handling_default"] == "Next") ? NoMatchHandling::NEXT : NoMatchHandling::PREVIOUS;
		m_TopFieldFirst = SetDefault(m_JsonProps, "tff", true);
		SetDefault(m_JsonProps, "project_garbage", json::object());
		SetDefault(m_JsonProps, "extra_attributes", json::object());

		auto& projectGarbage = m_JsonProps["project_garbage"];
		m_AutoReload = SetDefault(projectGarbage, "auto_reload", true);
		if (!projectGarbage.contains("version")) {
			SetDefault(projectGarbage, "notes", m_JsonProps["notes"]);
			SetDefault(projectGarbage, "scene_changes", m_JsonProps["scene_changes"]);
			projectGarbage["version"] = 1;
		}
		SetDefault(projectGarbage, "notes", json::array());
		SetDefault(projectGarbage, "scene_changes", json::array());
		m_ActiveCycle = SetDefault(projectGarbage, "active_cycle", 0);
		m_CombedDetection = SetDefault(projectGarbage, "combed_detection", false);
		m_CombedThreshold = SetDefault(projectGarbage, "combed_threshold", 45);

		std::string script_file = projectGarbage["script_file"];
		SetActiveFields(script_file.c_str(), true);
		m_ProjectOpened = true;
	}

	void StartNewProject(const char* script_path_name) {
		APP_LOG("Starting New Project with script: " + std::string(script_path_name));
		static int actions[] = { 0, 1, 2, 3, 8, 5, 4, 8, 6, 7 };
		static std::string notes[] = { "A", "A", "B", "B", "B", "C", "C", "D", "D", "D" };
		m_ProjectFile = "";
		m_JsonProps = R"({
			"ivtc_actions": [],
			"tff": true,
			"no_match_handling": {},
			"no_match_handling_default": "Previous",
			"project_garbage": {
				"version": 1,
				"auto_reload": true,
				"notes": [],
				"scene_changes": [],
				"combed_detection": false,
				"combed_threshold": 45
			},
			"extra_attributes": {}
		})"_json;
		m_JsonProps["project_garbage"]["script_file"] = script_path_name;
		
		SetActiveFields(script_path_name, false);
		
		if (!m_FieldsNode) {
			APP_LOG("ERROR: m_FieldsNode is NULL in StartNewProject. Aborting.");
			return;
		}

		const VSVideoInfo* vi = m_VSAPI->getVideoInfo(m_FieldsNode);
		if (!vi) { APP_LOG("ERROR: vi is NULL in StartNewProject."); return; }

		for (int i = 0; i < vi->numFrames; i++) {
			m_JsonProps["ivtc_actions"][i] = actions[i % 10];
			m_JsonProps["project_garbage"]["notes"][i] = notes[i % 10];
		}
		LoadFrames();
		m_ActiveCycle = 0;
		m_AutoReload = true;
		m_CombedDetection = false;
		m_CombedThreshold = 45;
		m_NoMatchHandling = NoMatchHandling::PREVIOUS;
		m_TopFieldFirst = true;
		m_ProjectOpened = true;
	}

	void SaveJson() {
		if (m_ProjectFile.empty()) SaveProjectAsDialog();
		m_JsonProps["project_garbage"]["active_cycle"] = m_ActiveCycle;
		std::string input = m_JsonProps.dump();
		std::string compressed = gzip::compress(input.c_str(), input.size());
		std::ofstream output(m_ProjectFile, std::ios::binary);
		output << compressed;
	}

	void UpdateAutoReload() { m_JsonProps["project_garbage"]["auto_reload"] = m_AutoReload; }
	void UpdateCombedDetection() { m_JsonProps["project_garbage"]["combed_detection"] = m_CombedDetection; AutoLoadFrames(); }
	void UpdateCombedThreshold() { m_JsonProps["project_garbage"]["combed_threshold"] = m_CombedThreshold; }
	void UpdateNoMatchHandling() {
		std::string newMatchString = (m_NoMatchHandling == NoMatchHandling::PREVIOUS) ? "Previous" : "Next";
		m_JsonProps["no_match_handling_default"] = newMatchString;
		auto newNoMatchHandling = json::object();
		m_JsonProps["no_match_handling"] = newNoMatchHandling;
		AutoLoadFrames();
	}
	void UpdateTopFieldFirst() { m_JsonProps["tff"] = m_TopFieldFirst; AutoLoadFrames(); }

	bool m_AutoReload = true;
	bool m_ProjectOpened = false;
	bool m_CombedDetection = false;
	int m_CombedThreshold = 45;
	int m_NoMatchHandling = NoMatchHandling::PREVIOUS;
	bool m_TopFieldFirst = true;

private:
	const VSAPI* m_VSAPI = nullptr;
	const VSSCRIPTAPI* m_VSSAPI = nullptr;
	std::string m_ProjectFile = "";
	json m_JsonProps;

	int m_ActiveCycle = 0;
	bool m_NeedNewFields = false;
	bool m_WantNewFrames = false;

	VSScript* m_FieldsScriptEnvironment = nullptr;
	VSNode* m_FieldsNode = nullptr;
	int m_FieldsWidth = 0;
	int m_FieldsHeight = 0;
	int m_FieldsFrameCount = 0;
	std::shared_ptr<Walnut::Image> m_Fields[11] = {};

	VSNode* m_FramesNode = nullptr;
	int m_FramesWidth = 0;
	int m_FramesHeight = 0;
	int m_FramesFrameCount = 0;
	std::shared_ptr<Walnut::Image> m_Frames[4] = {};
	int m_FieldCount[4] = {};
	std::string m_FreezeFrames[4] = {};
	int m_CombedMetrics[4] = {};

	VSNode* SeparateFields(VSCore* core, VSNode* node) {
		APP_LOG("Invoking VapourSynth Plugin: SeparateFields");
		if (!node) { APP_LOG("ERROR: Input node is NULL in SeparateFields!"); return nullptr; }
		
		VSPlugin* std_plugin = m_VSAPI->getPluginByID("com.vapoursynth.std", core);
		if(!std_plugin) {
			APP_LOG("ERROR: Plugin com.vapoursynth.std not found!");
			return node; // Return original node safely
		}
		
		VSMap* argument_map = m_VSAPI->createMap();
		m_VSAPI->mapConsumeNode(argument_map, "clip", node, maReplace);
		VSMap* result_map = m_VSAPI->invoke(std_plugin, "SeparateFields", argument_map);

		const char* result_error = m_VSAPI->mapGetError(result_map);
		if (result_error) APP_LOG("VS Error in SeparateFields: " + std::string(result_error));
		
		VSNode* output = m_VSAPI->mapGetNode(result_map, "clip", 0, nullptr);
		if (!output) APP_LOG("ERROR: Output node is NULL after SeparateFields invoke!");
		
		m_VSAPI->freeMap(argument_map);
		m_VSAPI->freeMap(result_map);
		return output;
	}

	VSNode* ConvertToYUV420P8(VSCore* core, VSNode* node) {
		APP_LOG("Invoking VapourSynth Plugin: Resize Spline36 (YUV420P8)");
		if (!node) { APP_LOG("ERROR: Input node is NULL in YUV420P8!"); return nullptr; }

		VSPlugin* resize_plugin = m_VSAPI->getPluginByID("com.vapoursynth.resize", core);
		if(!resize_plugin) { APP_LOG("ERROR: Plugin com.vapoursynth.resize not found!"); return node; }
		
		VSMap* argument_map = m_VSAPI->createMap();
		m_VSAPI->mapConsumeNode(argument_map, "clip", node, maReplace);
		m_VSAPI->mapSetInt(argument_map, "format", pfYUV420P8, maReplace);
		VSMap* result_map = m_VSAPI->invoke(resize_plugin, "Spline36", argument_map);

		const char* result_error = m_VSAPI->mapGetError(result_map);
		if (result_error) APP_LOG("VS Error in Spline36 YUV: " + std::string(result_error));

		VSNode* output = m_VSAPI->mapGetNode(result_map, "clip", 0, nullptr);
		if (!output) APP_LOG("ERROR: Output node is NULL after Spline36 YUV invoke!");

		m_VSAPI->freeMap(argument_map);
		m_VSAPI->freeMap(result_map);
		return output;
	}

	VSNode* ConvertToRGB(VSCore* core, VSNode* node) {
		APP_LOG("Invoking VapourSynth Plugin: Resize Spline36 (RGB24)");
		if (!node) { APP_LOG("ERROR: Input node is NULL in RGB24!"); return nullptr; }

		VSPlugin* resize_plugin = m_VSAPI->getPluginByID("com.vapoursynth.resize", core);
		if(!resize_plugin) { APP_LOG("ERROR: Plugin com.vapoursynth.resize not found!"); return node; }

		VSMap* argument_map = m_VSAPI->createMap();
		m_VSAPI->mapConsumeNode(argument_map, "clip", node, maReplace);
		m_VSAPI->mapSetInt(argument_map, "format", pfRGB24, maReplace);
		m_VSAPI->mapSetInt(argument_map, "matrix_in", 5, maReplace);
		VSMap* result_map = m_VSAPI->invoke(resize_plugin, "Spline36", argument_map);

		const char* result_error = m_VSAPI->mapGetError(result_map);
		if (result_error) APP_LOG("VS Error in Spline36 RGB: " + std::string(result_error));

		VSNode* output = m_VSAPI->mapGetNode(result_map, "clip", 0, nullptr);
		if (!output) APP_LOG("ERROR: Output node is NULL after Spline36 RGB invoke!");

		m_VSAPI->freeMap(argument_map);
		m_VSAPI->freeMap(result_map);
		return output;
	}

	VSNode* IVTCDN(VSCore* core, VSNode* node) {
		APP_LOG("Invoking VapourSynth Plugin: IVTC (tools.mike.ivtc)");
		if (!node) { APP_LOG("ERROR: Input node is NULL in IVTCDN!"); return nullptr; }

		VSPlugin* ivtcdn_plugin = m_VSAPI->getPluginByID("tools.mike.ivtc", core);
		if (!ivtcdn_plugin) {
			APP_LOG("CRITICAL ERROR: Plugin 'tools.mike.ivtc' NOT FOUND!");
			return node; // Safely return without destroying node
		}

		VSMap* argument_map = m_VSAPI->createMap();
		m_VSAPI->mapConsumeNode(argument_map, "clip", node, maReplace);
		std::string rawProps = m_JsonProps.dump();
		m_VSAPI->mapSetData(argument_map, "projectfile", rawProps.c_str(), rawProps.size(), dtUtf8, maReplace);
		m_VSAPI->mapSetInt(argument_map, "rawproject", 1, maReplace);
		VSMap* result_map = m_VSAPI->invoke(ivtcdn_plugin, "IVTC", argument_map);

		const char* result_error = m_VSAPI->mapGetError(result_map);
		if (result_error) APP_LOG("VS Error in IVTC: " + std::string(result_error));

		VSNode* output = m_VSAPI->mapGetNode(result_map, "clip", 0, nullptr);
		if (!output) APP_LOG("ERROR: Output node is NULL after IVTCDN invoke!");

		m_VSAPI->freeMap(argument_map);
		m_VSAPI->freeMap(result_map);
		return output;
	}

	VSNode* DMetrics(VSCore* core, VSNode* node) {
		APP_LOG("Invoking VapourSynth Plugin: DMetrics (com.vapoursynth.dmetrics)");
		if (!node) { APP_LOG("ERROR: Input node is NULL in DMetrics!"); return nullptr; }

		VSPlugin* dmetrics_plugin = m_VSAPI->getPluginByID("com.vapoursynth.dmetrics", core);
		if(!dmetrics_plugin) {
			APP_LOG("CRITICAL ERROR: Plugin 'com.vapoursynth.dmetrics' NOT FOUND!");
			return node; // Safely return
		}

		VSMap* argument_map = m_VSAPI->createMap();
		m_VSAPI->mapConsumeNode(argument_map, "clip", node, maReplace);
		VSMap* result_map = m_VSAPI->invoke(dmetrics_plugin, "DMetrics", argument_map);

		const char* result_error = m_VSAPI->mapGetError(result_map);
		if (result_error) APP_LOG("VS Error in DMetrics: " + std::string(result_error));

		VSNode* output = m_VSAPI->mapGetNode(result_map, "clip", 0, nullptr);
		if (!output) APP_LOG("ERROR: Output node is NULL after DMetrics invoke!");

		m_VSAPI->freeMap(argument_map);
		m_VSAPI->freeMap(result_map);
		return output;
	}

	void DrawField(const int i, const float display_width, const float display_height) {
		int activeField = m_ActiveCycle * 10 + i;
		if (activeField >= m_FieldsFrameCount) return;
        ImGuiIO& io = ImGui::GetIO();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		
		if (m_Fields[i] == nullptr || m_Fields[i]->GetDescriptorSet() == nullptr) {
			APP_LOG("WARNING: m_Fields[" + std::to_string(i) + "] or its descriptor set is null in DrawField.");
			return;
		}

		ImGui::Image(m_Fields[i]->GetDescriptorSet(), { display_width, display_height });
		auto& note = m_JsonProps["project_garbage"]["notes"][activeField];
		auto& action = m_JsonProps["ivtc_actions"][activeField];
		auto& scene_changes = m_JsonProps["project_garbage"]["scene_changes"];
		
		if (ImGui::IsItemHovered()) {
			if (!io.WantCaptureKeyboard) { 
				if (ImGui::IsKeyPressed(ImGuiKey_S) && !io.KeyCtrl) {
					auto it = std::find(scene_changes.begin(), scene_changes.end(), activeField);
					if (it == scene_changes.end()) scene_changes.push_back(activeField);
					else scene_changes.erase(it);
				}
				if (ImGui::IsKeyPressed(ImGuiKey_A)) note = "A";
				else if (ImGui::IsKeyPressed(ImGuiKey_B)) note = "B";
				else if (ImGui::IsKeyPressed(ImGuiKey_C)) note = "C";
				else if (ImGui::IsKeyPressed(ImGuiKey_D)) note = "D";

				static const int drop = 8;
				if (ImGui::IsKeyPressed(ImGuiKey_1) && i < 11) {
					int positiveAction = 0 + i % 2;
					action = action == positiveAction ? drop : positiveAction;
					AutoLoadFrames();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_2) && i < 11) {
					int positiveAction = 2 + i % 2;
					action = action == positiveAction ? drop : positiveAction;
					AutoLoadFrames();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_3) && i < 11) {
					int positiveAction = 4 + i % 2;
					action = action == positiveAction ? drop : positiveAction;
					AutoLoadFrames();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_4)) {
					if (i < 10) {
						int positiveAction = 6 + i % 2;
						action = action == positiveAction ? drop : positiveAction;
					} else {
						int positiveAction = 9;
						action = action == positiveAction ? drop : positiveAction;
					}
					AutoLoadFrames();
				}
			}
			ImGui::BeginTooltip();
			ImGui::Text("In %d", activeField / 2);
			float region_size = 32.0f;
			float region_x = io.MousePos.x - pos.x - region_size * 0.5f;
			float region_y = io.MousePos.y - pos.y - region_size * 0.5f;
			float zoom = 4.0f;
			if (region_x < 0.0f) region_x = 0.0f;
			else if (region_x > display_width - region_size) region_x = display_width - region_size;
			if (region_y < 0.0f) region_y = 0.0f;
			else if (region_y > display_height - region_size) region_y = display_height - region_size;
			ImVec2 uv0 = ImVec2((region_x) / display_width, (region_y) / display_height);
			ImVec2 uv1 = ImVec2((region_x + region_size) / display_width, (region_y + region_size) / display_height);
			ImGui::Image(m_Fields[i]->GetDescriptorSet(), ImVec2(region_size * zoom, region_size * zoom), uv0, uv1);
			ImGui::EndTooltip();
		}
		
		if (std::find(scene_changes.begin(), scene_changes.end(), activeField) != scene_changes.end()) {
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - 5, pos.y), ImVec2(pos.x, pos.y + display_height), IM_COL32(255, 128, 0, 255));
		}
		
		ImFont* font = g_UbuntuMonoFont ? g_UbuntuMonoFont : ImGui::GetFont();
		ImVec2 textSize = font->CalcTextSizeA(64.0f, FLT_MAX, 0.0f, note.get<std::string>().c_str());
		ImVec2 textPos(pos.x + display_width / 2 - textSize.x / 2, pos.y + display_height / 2 - textSize.y / 2);
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(textPos.x - 4, textPos.y + 5), ImVec2(textPos.x + textSize.x + 4, textPos.y + textSize.y), ColorForAction(action.get<int_fast8_t>()));
		ImGui::GetWindowDrawList()->AddText(font, 64.0f, textPos, IM_COL32_WHITE, note.get<std::string>().c_str());
	}

	void DrawFrame(const int i, const float display_width, const float display_height) {
        ImGuiIO& io = ImGui::GetIO();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		
		if (m_Frames[i] == nullptr || m_Frames[i]->GetDescriptorSet() == nullptr) {
			APP_LOG("WARNING: m_Frames[" + std::to_string(i) + "] or its descriptor set is null in DrawFrame.");
			return;
		}

		ImGui::Image(m_Frames[i]->GetDescriptorSet(), { display_width, display_height });

		static const char* context_labels[4] = { "frame 0 context", "frame 1 context", "frame 2 context", "frame 3 context" };
		if (ImGui::BeginPopupContextItem(context_labels[i])) {
			ImGui::Text("TODO menu %d", i);
			ImGui::EndPopup();
		}

		if (ImGui::IsItemHovered()) {
			auto activeFrame = std::to_string(m_ActiveCycle * 4 + i);
			if (!io.WantCaptureKeyboard) {
				if (ImGui::IsKeyPressed(ImGuiKey_F)) {
					auto& noMatchHandling = m_JsonProps["no_match_handling"];
					if (noMatchHandling.contains(activeFrame)) noMatchHandling.erase(activeFrame);
					else {
						if (m_NoMatchHandling == NoMatchHandling::PREVIOUS) noMatchHandling[activeFrame] = "Next";
						else noMatchHandling[activeFrame] = "Previous";
					}
					AutoLoadFrames();
				}
			}

			ImGui::BeginTooltip();
			ImGui::Text("Out %s", activeFrame.c_str());
			float region_size = 32.0f;
			float region_x = io.MousePos.x - pos.x - region_size * 0.5f;
			float region_y = io.MousePos.y - pos.y - region_size * 0.5f;
			float zoom = 4.0f;
			if (region_x < 0.0f) region_x = 0.0f;
			else if (region_x > display_width - region_size) region_x = display_width - region_size;
			if (region_y < 0.0f) region_y = 0.0f;
			else if (region_y > display_height - region_size) region_y = display_height - region_size;
			ImVec2 uv0 = ImVec2((region_x) / display_width, (region_y) / display_height);
			ImVec2 uv1 = ImVec2((region_x + region_size) / display_width, (region_y + region_size) / display_height);
			ImGui::Image(m_Frames[i]->GetDescriptorSet(), ImVec2(region_size * zoom, region_size * zoom), uv0, uv1);
			ImGui::EndTooltip();
		}

		if (m_FieldCount[i] == 1) {
			ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + (display_height / 2)), ImVec2(pos.x + display_width, pos.y + (display_height / 2)), IM_COL32(255, 255, 0, 128), 10);
		}
		if (m_CombedDetection && m_CombedMetrics[i] > m_CombedThreshold) {
			static int padding = 3; 
			ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + padding, pos.y + padding), ImVec2(pos.x + display_width - padding, pos.y + display_height - padding), IM_COL32(255, 0, 0, 128), 10);
			ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + display_width - padding, pos.y + padding), ImVec2(pos.x + padding, pos.y + display_height - padding), IM_COL32(255, 0, 0, 128), 10);
		}

		auto freezeFrame = m_FreezeFrames[i];
		auto action = freezeFrame.empty() ? i * 2 : 8;
		ImGui::GetWindowDrawList()->AddRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + display_width, pos.y + display_height), ColorForAction(action), 0, 0, 4);
		if (!freezeFrame.empty()) {
			ImFont* font = g_UbuntuMonoFont ? g_UbuntuMonoFont : ImGui::GetFont();
			ImVec2 textSize = font->CalcTextSizeA(64.0f, FLT_MAX, 0.0f, freezeFrame.c_str());
			ImVec2 textPos(pos.x + display_width / 2 - textSize.x / 2, pos.y + display_height / 2 - textSize.y / 2);
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(textPos.x - 4, textPos.y + 5), ImVec2(textPos.x + textSize.x, textPos.y + textSize.y), ColorForAction(action));
			ImGui::GetWindowDrawList()->AddText(font, 64.0f, textPos, IM_COL32_WHITE, freezeFrame.c_str());
		}
	}

	void ApplyCycleToScene() {
		auto& notes = m_JsonProps["project_garbage"]["notes"];
		auto& actions = m_JsonProps["ivtc_actions"];
		const auto& scene_changes = m_JsonProps["project_garbage"]["scene_changes"];

		int start_of_cycle = m_ActiveCycle * 10;
		int end_of_cycle = start_of_cycle + 9;

		int start_of_scene = 0;
		int end_of_scene = m_FieldsFrameCount - 1;
		for (auto it = scene_changes.begin(); it != scene_changes.end(); it++) {
			auto val = it.value();
			if (val > start_of_scene && val < start_of_cycle) start_of_scene = val;
			if (val < end_of_scene && val > start_of_cycle) end_of_scene = val;
		}

		int_fast8_t cycle_actions[10];
		std::string cycle_notes[10];
		int position_in_cycle = 0;
		for (int i = start_of_cycle; i <= end_of_cycle; i++) {
			cycle_actions[position_in_cycle] = actions[i];
			cycle_notes[position_in_cycle] = notes[i];
			++position_in_cycle;
		}

		position_in_cycle = start_of_scene % 10;
		for (int i = start_of_scene; i < end_of_scene; i++) {
			actions[i] = cycle_actions[position_in_cycle];
			notes[i] = cycle_notes[position_in_cycle];
			++position_in_cycle %= 10;
		}
	}

	void SetActiveFields(const char* file, bool doLoadFrames=true) {
		APP_LOG("SetActiveFields: evaluating file " + std::string(file));
		if (!m_VSSAPI || !m_VSAPI) { APP_LOG("ERROR: VSSAPI/VSAPI is NULL in SetActiveFields."); return; }

		if (m_FieldsScriptEnvironment != nullptr) {
			m_VSAPI->freeNode(m_FieldsNode);
			m_VSSAPI->freeScript(m_FieldsScriptEnvironment);
		}
		m_FieldsScriptEnvironment = m_VSSAPI->createScript(nullptr);

		m_VSSAPI->evalSetWorkingDir(m_FieldsScriptEnvironment, 1);
		int error = m_VSSAPI->evaluateFile(m_FieldsScriptEnvironment, file);
		if (error != 0) {
			const char* errStr = m_VSSAPI->getError(m_FieldsScriptEnvironment);
			APP_LOG("VS Eval Error: " + std::string(errStr));
		}

		m_FieldsNode = m_VSSAPI->getOutputNode(m_FieldsScriptEnvironment, 0);
		if (!m_FieldsNode) { APP_LOG("ERROR: getOutputNode returned NULL in SetActiveFields."); return; }

		const VSVideoInfo* vi = m_VSAPI->getVideoInfo(m_FieldsNode);
		if (!vi) { APP_LOG("ERROR: vi is NULL in SetActiveFields."); return; }
		
		APP_LOG("SetActiveFields: Evaluating ColorFamily...");
		if (vi->format.colorFamily == cfYUV) {
			VSCore* core = m_VSSAPI->getCore(m_FieldsScriptEnvironment);
			m_FieldsNode = SeparateFields(core, m_FieldsNode);
			m_FieldsNode = ConvertToRGB(core, m_FieldsNode);
		} else if (vi->format.colorFamily == cfRGB) {
			VSCore* core = m_VSSAPI->getCore(m_FieldsScriptEnvironment);
			m_FieldsNode = SeparateFields(core, m_FieldsNode);
		} else {
			APP_LOG("WARNING: Unknown color family detected in Script!");
		}

		if (!m_FieldsNode) { APP_LOG("ERROR: m_FieldsNode became NULL after format conversion pipeline."); return; }

		vi = m_VSAPI->getVideoInfo(m_FieldsNode);
		if (!vi) { APP_LOG("ERROR: vi is NULL after pipeline in SetActiveFields."); return; }

		m_FieldsWidth = vi->width;
		m_FieldsHeight = vi->height;
		m_FieldsFrameCount = vi->numFrames;

		for (int i = 0; i < 11; i++) {
			m_Fields[i] = std::make_shared<Walnut::Image>(
				m_FieldsWidth, m_FieldsHeight, Walnut::ImageFormat::RGBA, nullptr);
		}

		if (doLoadFrames) LoadFrames();
	}

	void AutoLoadFrames() {
		if (m_AutoReload) m_WantNewFrames = true;
	}

	void LoadFrames() {
		APP_LOG("LoadFrames: Starting...");
		if (!m_VSSAPI || !m_VSAPI || !m_FieldsScriptEnvironment) {
			APP_LOG("ERROR: Required VS API pointers are NULL in LoadFrames.");
			return;
		}

		if (m_FramesNode != nullptr) {
			m_VSAPI->freeNode(m_FramesNode);
			m_FramesNode = nullptr;
		}
		
		VSNode* rawFieldsNode = m_VSSAPI->getOutputNode(m_FieldsScriptEnvironment, 0);
		if (!rawFieldsNode) {
			APP_LOG("ERROR: rawFieldsNode is NULL in LoadFrames.");
			return;
		}

		const VSVideoInfo* vi = m_VSAPI->getVideoInfo(rawFieldsNode);
		if (!vi) {
			APP_LOG("ERROR: VideoInfo is NULL for rawFieldsNode.");
			m_VSAPI->freeNode(rawFieldsNode);
			return;
		}

		if (vi->format.colorFamily == cfYUV) {
			VSCore* core = m_VSSAPI->getCore(m_FieldsScriptEnvironment);
			m_FramesNode = SeparateFields(core, rawFieldsNode);
			m_FramesNode = IVTCDN(core, m_FramesNode);
			if (m_CombedDetection) {
				m_FramesNode = ConvertToYUV420P8(core, m_FramesNode);
				m_FramesNode = DMetrics(core, m_FramesNode);
			}
			m_FramesNode = ConvertToRGB(core, m_FramesNode);
		} else if (vi->format.colorFamily == cfRGB) {
			VSCore* core = m_VSSAPI->getCore(m_FieldsScriptEnvironment);
			m_FramesNode = SeparateFields(core, rawFieldsNode);
			m_FramesNode = IVTCDN(core, m_FramesNode);
		}

		if (!m_FramesNode) {
			APP_LOG("ERROR: m_FramesNode is NULL after pipeline processing!");
			m_FramesFrameCount = 0;
			return;
		}

		vi = m_VSAPI->getVideoInfo(m_FramesNode);
		if (!vi) {
			APP_LOG("ERROR: VideoInfo is NULL for final m_FramesNode.");
			m_FramesFrameCount = 0;
			return;
		}

		m_FramesWidth = vi->width;
		m_FramesHeight = vi->height;
		m_FramesFrameCount = vi->numFrames;

		for (int i = 0; i < 4; i++) {
			m_Frames[i] = std::make_shared<Walnut::Image>(
				m_FramesWidth, m_FramesHeight, Walnut::ImageFormat::RGBA, nullptr);
		}

		m_NeedNewFields = true;
		m_WantNewFrames = false;
		APP_LOG("LoadFrames: Completed successfully.");
	}
};

std::shared_ptr<ExampleLayer> g_Layer = nullptr;

void glfw_drop_callback(GLFWwindow* window, int path_count, const char* paths[]) {
	const char* filePathName = paths[0];
	const char* ext = strrchr(filePathName, '.');
	if (!strcmp(ext, ".vpy")) {
		g_Layer->StartNewProject(paths[0]);
	} else if (!strcmp(ext, ".ivtc")) {
		g_Layer->OpenProject(paths[0]);
	}
}

Walnut::Application* Walnut::CreateApplication(int argc, char** argv)
{
	Walnut::ApplicationSpecification spec;
	spec.Name = "IVTC DN";

	Walnut::Application* app = new Walnut::Application(spec);
	g_UbuntuMonoFont = app->m_UbuntuMonoFont;
	g_Layer = std::make_shared<ExampleLayer>();
	app->PushLayer(g_Layer);
	app->SetMenubarCallback([app]()
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New project...", "Ctrl+N")) g_Layer->NewProjectDialog();
			if (ImGui::MenuItem("Open project...", "Ctrl+O")) g_Layer->OpenProjectDialog();
			if (ImGui::MenuItem("Save project", "Ctrl+S")) g_Layer->SaveJson();
			if (ImGui::MenuItem("Save project as...")) g_Layer->SaveProjectAsDialog();
			if (ImGui::MenuItem("Exit")) app->Close();
			ImGui::Separator();

			if (!g_Layer->m_ProjectOpened) ImGui::BeginDisabled();
			
			if (ImGui::Checkbox("Auto-reload", &g_Layer->m_AutoReload)) g_Layer->UpdateAutoReload();
			if (ImGui::Checkbox("Combed Detection", &g_Layer->m_CombedDetection)) g_Layer->UpdateCombedDetection();
			
			if (!g_Layer->m_CombedDetection) ImGui::BeginDisabled();
			ImGui::Indent();
			HelpMarker("CTRL+click to input threshold."); ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::SliderInt("##Threshold", &g_Layer->m_CombedThreshold, 0, 256, nullptr, ImGuiSliderFlags_AlwaysClamp)) {
				g_Layer->UpdateCombedThreshold();
			}
			ImGui::Unindent();
			if (!g_Layer->m_CombedDetection) ImGui::EndDisabled();
			
			ImGui::Text("No Match Default");
			ImGui::Indent();
			if (ImGui::RadioButton("Previous", &g_Layer->m_NoMatchHandling, NoMatchHandling::PREVIOUS)) g_Layer->UpdateNoMatchHandling();
			ImGui::SameLine();
			if (ImGui::RadioButton("Next", &g_Layer->m_NoMatchHandling, NoMatchHandling::NEXT)) g_Layer->UpdateNoMatchHandling();
			ImGui::Unindent();
			if (ImGui::Checkbox("Top Field First", &g_Layer->m_TopFieldFirst)) g_Layer->UpdateTopFieldFirst();
			
			if (!g_Layer->m_ProjectOpened) ImGui::EndDisabled();
			ImGui::EndMenu();
		}
	});
	glfwSetDropCallback(app->GetWindowHandle(), glfw_drop_callback);
	GLFWimage image(32, 32, ICON_DATA);
	glfwSetWindowIcon(app->GetWindowHandle(), 1, &image);
	return app;
}
