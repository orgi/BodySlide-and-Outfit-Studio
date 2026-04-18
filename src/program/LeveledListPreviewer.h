/*
 * LeveledListPreviewer — 3D outfit browser for generated leveled list ESPs.
 *
 * Loads an ESP, shows outfit list (filterable by level/name), and renders
 * selected outfit armor pieces in a GLSurface viewport.
 */

#pragma once

#include "../components/LeveledListData.h"
#include "../components/SliderPresets.h"
#include "../components/SmpSimulator.h"
#include "../files/TriFile.h"
#include "../render/GLSurface.h"
#include "../utils/ConfigurationManager.h"

#include <array>
#include <memory>
#include <set>
#include <wx/combobox.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/textcompleter.h>
#include <wx/timer.h>
#include <wx/wx.h>

class BodySlideApp;
class LLPreviewCanvas;

wxDECLARE_EVENT(wxEVT_LEVELEDLIST_CLOSED, wxCommandEvent);

extern ConfigurationManager Config;

class LeveledListPreviewer : public wxFrame {
	wxEvtHandler* eventHandler = nullptr;
	LLPreviewCanvas* canvas = nullptr;
	std::unique_ptr<wxGLContext> context;

	GLSurface gls;
	std::unordered_map<std::string, GLMaterial*> shapeMaterials;
	std::vector<std::string> untexturedShapes; // shapes with no diffuse texture
	std::vector<std::string> bodyShapeNames;   // shapes loaded as body base layer
	std::vector<std::string> outfitShapeNames; // shapes loaded as outfit pieces
	std::vector<std::string> headShapeNames;   // shapes loaded as NPC head layer
	bool showUntextured = false;
	bool showBody = true;

	// Mesh list overlay: floating panel over the GL canvas listing loaded shapes
	wxScrolledWindow* meshOverlayPanel = nullptr;
	std::unordered_map<std::string, wxCheckBox*> meshCheckboxes; // shapeName → checkbox
	std::unordered_map<std::string, std::string> shapeNifSource; // shapeName → source NIF path
	std::unordered_map<std::string, std::string> shapeArmoName;	 // shapeName → ARMO display name
	std::set<std::string> expandedNifGroups;					 // NIF paths currently expanded in overlay

	// "Use Any" variant groups: only one variant per group is shown at a time
	struct UseAnyVariant {
		int variantIdx = -1;				 // original variant index from ESP
		std::string label;					 // display name for this variant
		std::vector<std::string> shapeNames; // shapes belonging to this variant
	};
	struct UseAnyGroup {
		int groupId = -1;
		std::vector<UseAnyVariant> variants;
		int activeIndex = 0; // which variant is currently shown
	};
	std::vector<UseAnyGroup> useAnyGroups_;
	// Map shape name → (group index in useAnyGroups_, variant index) for quick lookup
	std::unordered_map<std::string, std::pair<size_t, size_t>> shapeToVariantGroup_;

	// Body part tracking: shape name → set of NIF partition body part IDs
	// (e.g. 32=SBP_32_BODY, 33=SBP_33_HANDS, 37=SBP_37_FEET)
	std::unordered_map<std::string, std::set<uint16_t>> bodyShapePartMap;

	// UI controls
	wxSplitterWindow* splitter = nullptr;
	wxSearchCtrl* searchCtrl = nullptr;
	wxSpinCtrl* levelMinSpin = nullptr;
	wxSpinCtrl* levelMaxSpin = nullptr;
	wxTextCtrl* headCtrl = nullptr;		   // NPC head selector (with autocomplete)
	wxComboBox* presetCombo = nullptr;	   // BodySlide preset selector
	wxCheckBox* highWeightCheck = nullptr; // high (_1) vs low (_0) weight
	wxListCtrl* outfitList = nullptr;

	// Data
	lldata::LeveledListData data;
	const lldata::OutfitEntry* currentOutfit = nullptr;
	std::vector<const lldata::OutfitEntry*> filteredOutfits;
	wxString lastESPDirectory;
	std::string lastESPFilepath;
	std::unique_ptr<wxFileSystemWatcher> fsWatcher;

	// NPC head
	std::unordered_map<std::string, size_t> npcByDisplay; // displayName → index in data.GetNPCs()
	std::string currentHeadEditorId;

	bool useHighWeight = true; // true = _1 (high), false = _0 (low)

	// Preset morphing via .tri files
	// Game verts (from built game NIF) — base for morph computation and reset
	std::unordered_map<std::string, std::vector<nifly::Vector3>> bodyGameVerts;
	std::string currentPresetName;

	// .tri file cache: NIF relative path → loaded TriFile
	std::unordered_map<std::string, TriFile> triFileCache_;

	// Per-slot NPC skin textures (body / hands / feet), resolved via NPC→race chain
	// with ARMA-race filtering. Empty entries mean "no override — use NIF default".
	lldata::LeveledListData::BodyPartTextures npcBodyPartTextures{};
	bool hasNpcSkinTextures = false;

	// QNAM face tint (sRGB). Applied as a per-vertex color multiplier on skin shapes
	// so the body/hands/feet/head get the NPC's complexion without touching shaders.
	bool hasNpcTint = false;
	float npcTintR = 1.0f;
	float npcTintG = 1.0f;
	float npcTintB = 1.0f;

	// Outfit piece preset morphing via .tri files
	std::unordered_map<std::string, std::vector<nifly::Vector3>> outfitGameVerts; // displayName → game verts
	// displayName → original NIF shape name (for .tri morph lookup; strips formId prefix)
	std::unordered_map<std::string, std::string> outfitShapeNifName_;

	// SMP physics simulation
	std::unique_ptr<SmpSimulator> smpSimulator_;
	wxTimer smpTimer_;
	wxCheckBox* smpToggle_ = nullptr;
	bool smpRunning_ = false;
	// SMP XML paths discovered for the current outfit (displayName → xmlPath)
	std::unordered_map<std::string, std::string> smpXmlPaths_;

	// Body slots declared by the current outfit's ARMO records (e.g. 32=body, 33=hands, 37=feet).
	// Used post-load to delete default body shapes whose slots are fully claimed by the outfit.
	std::set<int> outfitDeclaredSlots;

	wxDECLARE_EVENT_TABLE();

public:
	LeveledListPreviewer(wxEvtHandler* handler = nullptr);
	~LeveledListPreviewer();

	void OnShown();
	void OnClose(wxCloseEvent& event);

	// Menu / toolbar actions
	void OnLoadESP(wxCommandEvent& event);
	void OnReloadESP(wxCommandEvent& event);
	void OnFileChanged(wxFileSystemWatcherEvent& event);

	// List and filter events
	void OnSearchChanged(wxCommandEvent& event);
	void OnLevelChanged(wxSpinEvent& event);
	void OnOutfitSelected(wxListEvent& event);

	// Head NPC and preset events
	void OnHeadEntered(wxCommandEvent& event);
	void OnPresetChanged(wxCommandEvent& event);
	void OnHighWeightChanged(wxCommandEvent& event);

	// GL canvas events
	void Render() { gls.RenderOneFrame(); }
	void Resized(uint32_t w, uint32_t h) { gls.SetSize(w, h); }
	void RightDrag(int dX, int dY);
	void LeftDrag(int dX, int dY);
	void MouseWheel(int dW);
	void TrackMouse(int X, int Y);

	void ToggleTextures() {
		gls.ToggleTextures();
		gls.RenderOneFrame();
	}
	void ToggleWireframe() {
		gls.ToggleWireframe();
		gls.RenderOneFrame();
	}
	void OnToggleUntextured(wxCommandEvent& event);
	void OnToggleBody(wxCommandEvent& event);
	void OnToggleSmp(wxCommandEvent& event);
	void OnSmpTimer(wxTimerEvent& event);

	friend class LLPreviewCanvas;

private:
	void LoadESPFile(const std::string& filepath);
	void RefreshOutfitList();
	void LoadOutfitMeshes(const lldata::OutfitEntry& outfit);
	bool AddNifShapeTextures(nifly::NifFile* nif, const std::string& shapeName, const std::vector<lldata::TextureOverride>* overrides = nullptr, const std::string& meshName = {});
	bool LoadNifFromPath(const std::string& relativePath, const std::string& prefix = "");
	void LoadBodyMeshes();
	void LoadHeadMesh(const lldata::NPCEntry& npc);
	void ClearHeadMeshes();
	void UpdateHeadVisibility();
	void UpdateActiveSlotsAndVisibility();
	void LoadPresetList();
	void ApplyPresetToBody(const std::string& presetName, bool deferredRender = false);
	void ApplyPresetToOutfit(const std::string& presetName, bool deferredRender = false);
	TriFile* GetOrLoadTriFile(const std::string& nifRelativePath);

	// Mesh overlay helpers
	void RefreshMeshOverlay();
	void SyncMeshOverlayStates();
	void RepositionMeshOverlay();
	void SwitchUseAnyVariant(size_t groupIdx, int newVariantIdx);

	// SMP physics helpers
	void SetupSmpSimulation();
	void StopSmpSimulation();
	std::string FindSmpXml(nifly::NifFile& nif, const std::string& nifRelativePath);
	std::string FindSkeletonNifPath();

	enum {
		ID_LoadESP = wxID_HIGHEST + 500,
		ID_ReloadESP,
		ID_ToggleUntextured,
		ID_ToggleBody,
		ID_HeadNPC,
		ID_Preset,
		ID_HighWeight,
		ID_ToggleSmp,
		ID_SmpTimer,
	};
};

class LLPreviewCanvas : public wxGLCanvas {
	LeveledListPreviewer* previewer = nullptr;
	bool firstPaint = true;
	wxPoint lastMousePosition;

public:
	LLPreviewCanvas(LeveledListPreviewer* pw, wxWindow* parent, const wxGLAttributes& attribs);

	void OnPaint(wxPaintEvent& event);
	void OnKeyUp(wxKeyEvent& event);
	void OnMotion(wxMouseEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnResized(wxSizeEvent& event);

	wxDECLARE_EVENT_TABLE();
};
