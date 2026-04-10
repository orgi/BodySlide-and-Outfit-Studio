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

#include <memory>
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/srchctrl.h>
#include <wx/splitter.h>
#include <wx/textcompleter.h>

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
	std::vector<std::string> headShapeNames;   // shapes loaded as NPC head layer
	bool showUntextured = false;
	bool showBody = true;

	// UI controls
	wxSplitterWindow* splitter = nullptr;
	wxSearchCtrl* searchCtrl = nullptr;
	wxSpinCtrl* levelMinSpin = nullptr;
	wxSpinCtrl* levelMaxSpin = nullptr;
	wxTextCtrl* headCtrl = nullptr;     // NPC head selector (with autocomplete)
	wxComboBox* presetCombo = nullptr;  // BodySlide preset selector
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

	// Body preset morphing
	struct BodyProject {
		std::string refNifPath;    // absolute path to reference (unmorphed) NIF
		std::string sliderSetFile; // .osp file path
		std::string setName;       // set name within .osp
	};
	std::vector<BodyProject> bodyProjects;
	// Original (pre-morph) verts per body shape, stored when body is loaded
	std::unordered_map<std::string, std::vector<nifly::Vector3>> bodyRefVerts;
	std::unordered_map<std::string, std::vector<nifly::Vector2>> bodyRefUVs;
	std::string currentPresetName;

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

	// GL canvas events
	void Render() { gls.RenderOneFrame(); }
	void Resized(uint32_t w, uint32_t h) { gls.SetSize(w, h); }
	void RightDrag(int dX, int dY);
	void LeftDrag(int dX, int dY);
	void MouseWheel(int dW);
	void TrackMouse(int X, int Y);

	void ToggleTextures() { gls.ToggleTextures(); gls.RenderOneFrame(); }
	void ToggleWireframe() { gls.ToggleWireframe(); gls.RenderOneFrame(); }
	void OnToggleUntextured(wxCommandEvent& event);
	void OnToggleBody(wxCommandEvent& event);

	friend class LLPreviewCanvas;

private:
	void LoadESPFile(const std::string& filepath);
	void RefreshOutfitList();
	void LoadOutfitMeshes(const lldata::OutfitEntry& outfit);
	bool AddNifShapeTextures(nifly::NifFile* nif, const std::string& shapeName,
						  const std::vector<lldata::TextureOverride>* overrides = nullptr,
						  const std::string& meshName = {});
	bool LoadNifFromPath(const std::string& relativePath, const std::string& prefix = "");
	void LoadBodyMeshes();
	void FindBodySliderProjects();
	void LoadHeadMesh(const lldata::NPCEntry& npc);
	void ClearHeadMeshes();
	void LoadPresetList();
	void ApplyPresetToBody(const std::string& presetName);

	enum {
		ID_LoadESP = wxID_HIGHEST + 500,
		ID_ReloadESP,
		ID_ToggleUntextured,
		ID_ToggleBody,
		ID_HeadNPC,
		ID_Preset,
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
