/*
 * LeveledListPreviewer — 3D outfit browser for generated leveled list ESPs.
 *
 * Loads an ESP, shows outfit list (filterable by level/name), and renders
 * selected outfit armor pieces in a GLSurface viewport.
 */

#pragma once

#include "../components/LeveledListData.h"
#include "../components/SliderManager.h"
#include "../components/SliderSet.h"
#include "../render/GLSurface.h"
#include "../utils/ConfigurationManager.h"

#include <array>\n#include <memory>
#include <wx/combobox.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/textcompleter.h>
#include <wx/wx.h>

class BodySlideApp;
class LLPreviewCanvas;

extern ConfigurationManager Config;

class LeveledListPreviewer : public wxFrame {
	BodySlideApp* app = nullptr;
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

	// Body part tracking for auto-hiding (0=body, 1=hands, 2=feet)
	enum BodyPartType : uint8_t { BP_BODY = 0, BP_HANDS = 1, BP_FEET = 2 };
	std::unordered_map<std::string, BodyPartType> bodyShapePartMap;

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
	std::vector<const lldata::OutfitEntry*> filteredOutfits;
	wxString lastESPDirectory;
	std::string lastESPFilepath;
	std::unique_ptr<wxFileSystemWatcher> fsWatcher;

	// NPC head
	std::unordered_map<std::string, size_t> npcByDisplay; // displayName → index in data.GetNPCs()
	std::string currentHeadEditorId;

	bool useHighWeight = true; // true = _1 (high), false = _0 (low)

	// Body preset morphing
	struct BodyProject {
		std::string refNifPath;	   // slider set reference NIF (same for both weights)
		std::string sliderSetFile; // .osp file path
		std::string setName;	   // set name within .osp
	};
	std::vector<BodyProject> bodyProjects;

	// Maps game body shape → slider project shape for morphing
	struct BodyShapeMorphInfo {
		size_t projectIdx;		  // index into bodyProjects
		std::string refShapeName; // shape name in the reference NIF / SliderSet
	};
	std::unordered_map<std::string, BodyShapeMorphInfo> bodyShapeMorphMap;

	// Game body verts (from built game body NIF) — used to reset to "(none)"
	std::unordered_map<std::string, std::vector<nifly::Vector3>> bodyGameVerts;
	// Slider reference verts (from ShapeData reference NIF) — base for morph computation
	std::unordered_map<std::string, std::vector<nifly::Vector3>> bodyRefVerts;
	std::unordered_map<std::string, std::vector<nifly::Vector2>> bodyRefUVs;
	std::string currentPresetName;

	// All slider projects indexed by normalized output NIF path
	std::unordered_map<std::string, BodyProject> allSliderProjects;
	bool sliderProjectsCached = false; // true after first FindBodySliderProjects scan

	// NPC skin texture overrides (resolved from WNAM → ARMO → ARMA → TXST)
	std::array<std::string, 8> npcSkinTextures{};
	bool hasNpcSkinTextures = false;

	// Outfit piece preset morphing
	struct OutfitShapeMorphInfo {
		std::string sliderSetFile;
		std::string setName;
		std::string refShapeName; // shape name in reference NIF / SliderSet
	};
	std::unordered_map<std::string, OutfitShapeMorphInfo> outfitShapeMorphMap;	  // displayName → morph info
	std::unordered_map<std::string, std::vector<nifly::Vector3>> outfitRefVerts;  // displayName → ref verts
	std::unordered_map<std::string, std::vector<nifly::Vector3>> outfitGameVerts; // displayName → game verts

	wxDECLARE_EVENT_TABLE();

public:
	LeveledListPreviewer(BodySlideApp* app);
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

	friend class LLPreviewCanvas;

private:
	void LoadESPFile(const std::string& filepath);
	void RefreshOutfitList();
	void LoadOutfitMeshes(const lldata::OutfitEntry& outfit);
	bool AddNifShapeTextures(nifly::NifFile* nif, const std::string& shapeName, const std::vector<lldata::TextureOverride>* overrides = nullptr, const std::string& meshName = {});
	bool LoadNifFromPath(const std::string& relativePath, const std::string& prefix = "");
	void LoadBodyMeshes();
	void FindBodySliderProjects();
	void LoadHeadMesh(const lldata::NPCEntry& npc);
	void ClearHeadMeshes();
	void LoadPresetList();
	void ApplyPresetToBody(const std::string& presetName);
	void ApplyPresetToOutfit(const std::string& presetName);
	static std::string NormalizeNifOutputPath(const std::string& path);

	enum {
		ID_LoadESP = wxID_HIGHEST + 500,
		ID_ReloadESP,
		ID_ToggleUntextured,
		ID_ToggleBody,
		ID_HeadNPC,
		ID_Preset,
		ID_HighWeight,
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
