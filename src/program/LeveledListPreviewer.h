/*
 * LeveledListPreviewer — 3D outfit browser for generated leveled list ESPs.
 *
 * Loads an ESP, shows outfit list (filterable by level/name), and renders
 * selected outfit armor pieces in a GLSurface viewport.
 */

#pragma once

#include "../components/LeveledListData.h"
#include "../render/GLSurface.h"
#include "../utils/ConfigurationManager.h"

#include <memory>
#include <wx/wx.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/srchctrl.h>
#include <wx/splitter.h>

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
	bool showUntextured = false;
	bool showBody = true;

	// UI controls
	wxSplitterWindow* splitter = nullptr;
	wxSearchCtrl* searchCtrl = nullptr;
	wxSpinCtrl* levelMinSpin = nullptr;
	wxSpinCtrl* levelMaxSpin = nullptr;
	wxListCtrl* outfitList = nullptr;

	// Data
	lldata::LeveledListData data;
	std::vector<const lldata::OutfitEntry*> filteredOutfits;
	wxString lastESPDirectory;
	std::string lastESPFilepath;
	std::unique_ptr<wxFileSystemWatcher> fsWatcher;

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
						  const std::vector<lldata::TextureOverride>* overrides = nullptr);
	bool LoadNifFromPath(const std::string& relativePath, const std::string& prefix = "");
	void LoadBodyMeshes();

	enum {
		ID_LoadESP = wxID_HIGHEST + 500,
		ID_ReloadESP,
		ID_ToggleUntextured,
		ID_ToggleBody,
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
