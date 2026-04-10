/*
 * LeveledListPreviewer — outfit browser and 3D preview for leveled list ESPs.
 */

#include "LeveledListPreviewer.h"
#include "BodySlideApp.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include <wx/dir.h>

using namespace nifly;

extern ConfigurationManager Config;

// ---------------------------------------------------------------------------
// Event tables
// ---------------------------------------------------------------------------

wxBEGIN_EVENT_TABLE(LeveledListPreviewer, wxFrame)
	EVT_CLOSE(LeveledListPreviewer::OnClose)
	EVT_MENU(LeveledListPreviewer::ID_LoadESP, LeveledListPreviewer::OnLoadESP)
	EVT_MENU(LeveledListPreviewer::ID_ReloadESP, LeveledListPreviewer::OnReloadESP)
	EVT_MENU(LeveledListPreviewer::ID_ToggleUntextured, LeveledListPreviewer::OnToggleUntextured)
	EVT_MENU(LeveledListPreviewer::ID_ToggleBody, LeveledListPreviewer::OnToggleBody)
	EVT_TEXT_ENTER(LeveledListPreviewer::ID_HeadNPC, LeveledListPreviewer::OnHeadEntered)
	EVT_COMBOBOX(LeveledListPreviewer::ID_Preset, LeveledListPreviewer::OnPresetChanged)
	EVT_FSWATCHER(wxID_ANY, LeveledListPreviewer::OnFileChanged)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LeveledListPreviewer::LeveledListPreviewer(BodySlideApp* app)
	: wxFrame(nullptr, wxID_ANY, _("Leveled List Previewer"), wxDefaultPosition, wxDefaultSize)
	, app(app) {
	SetIcon(wxIcon(wxString::FromUTF8(Config["AppDir"]) + "/res/images/BodySlide.png", wxBITMAP_TYPE_PNG));

	// Menu bar
	wxMenuBar* menuBar = new wxMenuBar();
	wxMenu* fileMenu = new wxMenu();
	fileMenu->Append(ID_LoadESP, _("&Open ESP...\tCtrl+O"), _("Load a generated leveled list ESP"));
	fileMenu->Append(ID_ReloadESP, _("&Reload ESP\tCtrl+R"), _("Reload the current ESP file"));
	menuBar->Append(fileMenu, _("&File"));

	wxMenu* viewMenu = new wxMenu();
	viewMenu->AppendCheckItem(ID_ToggleUntextured, _("Show &Untextured Meshes\tU"), _("Show collision and other meshes without textures"));
	viewMenu->Check(ID_ToggleUntextured, false);
	viewMenu->AppendCheckItem(ID_ToggleBody, _("Show &Body\tB"), _("Show default body, hands and feet underneath outfit"));
	viewMenu->Check(ID_ToggleBody, true);
	menuBar->Append(viewMenu, _("&View"));

	SetMenuBar(menuBar);

	// Main splitter: left panel (controls + list) | right panel (GL canvas)
	splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_3D | wxSP_LIVE_UPDATE);

	// --- Left panel ---
	wxPanel* leftPanel = new wxPanel(splitter);
	wxBoxSizer* leftSizer = new wxBoxSizer(wxVERTICAL);

	// Search bar
	searchCtrl = new wxSearchCtrl(leftPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	searchCtrl->SetDescriptiveText(_("Search outfits..."));
	searchCtrl->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, &LeveledListPreviewer::OnSearchChanged, this);
	searchCtrl->Bind(wxEVT_TEXT, &LeveledListPreviewer::OnSearchChanged, this);
	leftSizer->Add(searchCtrl, 0, wxEXPAND | wxALL, 5);

	// Level range controls
	wxBoxSizer* levelSizer = new wxBoxSizer(wxHORIZONTAL);
	levelSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Level:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	levelMinSpin = new wxSpinCtrl(leftPanel, wxID_ANY, "1", wxDefaultPosition, wxSize(60, -1), wxSP_ARROW_KEYS, 1, 100, 1);
	levelMaxSpin = new wxSpinCtrl(leftPanel, wxID_ANY, "100", wxDefaultPosition, wxSize(60, -1), wxSP_ARROW_KEYS, 1, 100, 100);
	levelSizer->Add(levelMinSpin, 0, wxRIGHT, 5);
	levelSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("-")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	levelSizer->Add(levelMaxSpin, 0);

	levelMinSpin->Bind(wxEVT_SPINCTRL, &LeveledListPreviewer::OnLevelChanged, this);
	levelMaxSpin->Bind(wxEVT_SPINCTRL, &LeveledListPreviewer::OnLevelChanged, this);

	leftSizer->Add(levelSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// NPC head selector
	wxBoxSizer* headSizer = new wxBoxSizer(wxHORIZONTAL);
	headSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Head:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	headCtrl = new wxTextCtrl(leftPanel, ID_HeadNPC, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	headCtrl->SetToolTip(_("Type an NPC editor ID or name and press Enter"));
	headSizer->Add(headCtrl, 1);
	leftSizer->Add(headSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// Preset selector
	wxBoxSizer* presetSizer = new wxBoxSizer(wxHORIZONTAL);
	presetSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Preset:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	presetCombo = new wxComboBox(leftPanel, ID_Preset, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxArrayString{}, wxCB_READONLY | wxCB_DROPDOWN);
	presetCombo->SetToolTip(_("Choose a BodySlide preset to morph the preview body"));
	presetSizer->Add(presetCombo, 1);
	leftSizer->Add(presetSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// Outfit list
	outfitList = new wxListCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	{
		int w0 = Config.GetIntValue("LLPreviewer/ColW0");
		int w1 = Config.GetIntValue("LLPreviewer/ColW1");
		int w2 = Config.GetIntValue("LLPreviewer/ColW2");
		outfitList->AppendColumn(_("Name"),   wxLIST_FORMAT_LEFT,  w0 > 0 ? w0 : 200);
		outfitList->AppendColumn(_("Level"),  wxLIST_FORMAT_RIGHT, w1 > 0 ? w1 : 50);
		outfitList->AppendColumn(_("Pieces"), wxLIST_FORMAT_RIGHT, w2 > 0 ? w2 : 50);
	}
	outfitList->Bind(wxEVT_LIST_ITEM_SELECTED, &LeveledListPreviewer::OnOutfitSelected, this);
	leftSizer->Add(outfitList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	leftPanel->SetSizer(leftSizer);

	// --- Right panel: GL canvas ---
	wxPanel* rightPanel = new wxPanel(splitter);
	wxBoxSizer* rightSizer = new wxBoxSizer(wxVERTICAL);

	canvas = new LLPreviewCanvas(this, rightPanel, GLSurface::GetGLAttribs());
	context = std::make_unique<wxGLContext>(canvas, nullptr, &GLSurface::GetGLContextAttribs());

	rightSizer->Add(canvas, 1, wxEXPAND);
	rightPanel->SetSizer(rightSizer);

	// Split
	splitter->SplitVertically(leftPanel, rightPanel, 320);
	splitter->SetMinimumPaneSize(200);

	// Status bar
	CreateStatusBar();
	SetStatusText(_("Use File > Open ESP to load a leveled list plugin."));

	// Restore last directory from config
	lastESPDirectory = wxString::FromUTF8(Config["LLPreviewer/LastESPDir"]);
	std::string lastFile = Config["LLPreviewer/LastESPFile"];

	// Restore window size and position
	{
		int x = Config.GetIntValue("LLPreviewer/WindowX");
		int y = Config.GetIntValue("LLPreviewer/WindowY");
		int w = Config.GetIntValue("LLPreviewer/WindowW");
		int h = Config.GetIntValue("LLPreviewer/WindowH");
		if (w > 0 && h > 0)
			SetSize(x, y, w, h);
		else
			SetSize(wxDefaultCoord, wxDefaultCoord, 1200, 800);
		if (Config["LLPreviewer/Maximized"] == "true")
			Maximize();
	}
	{
		int sash = Config.GetIntValue("LLPreviewer/SashPos");
		splitter->SetSashPosition(sash > 0 ? sash : 320);
	}

	// Restore persisted head / preset / outfit selections
	currentHeadEditorId = Config["LLPreviewer/HeadEditorId"];
	currentPresetName   = Config["LLPreviewer/PresetName"];
	if (headCtrl && !currentHeadEditorId.empty())
		headCtrl->SetValue(wxString::FromUTF8(currentHeadEditorId));

	Show();

	// Offer to reload last ESP after window is shown
	if (!lastFile.empty() && wxFileName::FileExists(lastFile)) {
		CallAfter([this, lastFile]() {
			if (wxMessageBox(
					wxString::Format(_("Reload last ESP?\n%s"), lastFile),
					_("Leveled List Previewer"),
					wxYES_NO | wxICON_QUESTION, this) == wxYES) {
				LoadESPFile(lastFile);
			}
		});
	}
}

LeveledListPreviewer::~LeveledListPreviewer() {}

// ---------------------------------------------------------------------------
// GL initialization (called on first paint)
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnShown() {
	if (!context->IsOK()) {
		Destroy();
		canvas = nullptr;
		wxLogError("Leveled List Previewer: OpenGL context is not OK.");
		wxMessageBox(_("Preview failed: OpenGL context is not OK."), _("OpenGL Error"), wxICON_ERROR);
		return;
	}

	gls.Initialize(canvas, context.get());
	auto size = canvas->GetSize();
	gls.SetStartingView(Vector3(0.0f, -5.0f, -15.0f), Vector3(15.0f, 0.0f, 0.0f), size.GetWidth(), size.GetHeight(), 65.0);

	int ambient = Config.GetIntValue("Lights/Ambient");
	int frontal = Config.GetIntValue("Lights/Frontal");
	int d0 = Config.GetIntValue("Lights/Directional0");
	int d0x = Config.GetIntValue("Lights/Directional0.x");
	int d0y = Config.GetIntValue("Lights/Directional0.y");
	int d0z = Config.GetIntValue("Lights/Directional0.z");
	int d1 = Config.GetIntValue("Lights/Directional1");
	int d1x = Config.GetIntValue("Lights/Directional1.x");
	int d1y = Config.GetIntValue("Lights/Directional1.y");
	int d1z = Config.GetIntValue("Lights/Directional1.z");
	int d2 = Config.GetIntValue("Lights/Directional2");
	int d2x = Config.GetIntValue("Lights/Directional2.x");
	int d2y = Config.GetIntValue("Lights/Directional2.y");
	int d2z = Config.GetIntValue("Lights/Directional2.z");

	gls.UpdateLights(ambient, frontal, d0, d1, d2,
					 Vector3(d0x / 100.0f, d0y / 100.0f, d0z / 100.0f),
					 Vector3(d1x / 100.0f, d1y / 100.0f, d1z / 100.0f),
					 Vector3(d2x / 100.0f, d2y / 100.0f, d2z / 100.0f));

	if (Config.Exists("Rendering/ColorBackground")) {
		int r = Config.GetIntValue("Rendering/ColorBackground.r");
		int g = Config.GetIntValue("Rendering/ColorBackground.g");
		int b = Config.GetIntValue("Rendering/ColorBackground.b");
		gls.SetBackgroundColor(Vector3(r / 255.0f, g / 255.0f, b / 255.0f));
	}

	gls.SetPerspective(true);
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnLoadESP(wxCommandEvent& WXUNUSED(event)) {
	wxFileDialog dlg(this, _("Open ESP/ESM File"), lastESPDirectory, "",
					 "Elder Scrolls Plugin (*.esp;*.esm;*.esl)|*.esp;*.esm;*.esl",
					 wxFD_OPEN | wxFD_FILE_MUST_EXIST);

	if (dlg.ShowModal() != wxID_OK)
		return;

	lastESPDirectory = dlg.GetDirectory();
	LoadESPFile(dlg.GetPath().ToStdString());
}

void LeveledListPreviewer::LoadESPFile(const std::string& filepath) {
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';
	data.SetBaseDataPath(baseGamePath);

	wxBusyCursor wait;
	if (!data.LoadESP(filepath)) {
		wxMessageBox(_("Failed to load ESP file."), _("Error"), wxICON_ERROR);
		return;
	}

	lastESPFilepath = filepath;

	// Watch the directory containing the ESP so we catch both in-place writes
	// (MODIFY) and rename-based writes (CREATE/RENAME), which is what most
	// build tools use. File-level watches miss rename-based overwrites on Linux.
	wxFileName espFileName = wxFileName::FileName(wxString::FromUTF8(filepath));
	fsWatcher = std::make_unique<wxFileSystemWatcher>();
	fsWatcher->SetOwner(this);
	fsWatcher->Add(wxFileName::DirName(espFileName.GetPath()),
				   wxFSW_EVENT_MODIFY | wxFSW_EVENT_CREATE | wxFSW_EVENT_RENAME);

	// Persist for next session
	Config.SetValue("LLPreviewer/LastESPFile", filepath);
	Config.SetValue("LLPreviewer/LastESPDir", lastESPDirectory.ToStdString());

	// Load NPCs from vanilla ESMs for the head autocomplete (once per session is fine;
	// only reload if the list is empty so repeated ESP reloads don't re-parse large ESMs)
	if (data.GetNPCs().empty()) {
		data.LoadNPCs();

		// Build display→index map and populate autocomplete
		npcByDisplay.clear();
		wxArrayString npcNames;
		auto& npcs = data.GetNPCs();
		npcNames.reserve(npcs.size());
		for (size_t i = 0; i < npcs.size(); ++i) {
			npcByDisplay[npcs[i].displayName] = i;
			npcNames.push_back(wxString::FromUTF8(npcs[i].displayName));
		}
		if (headCtrl)
			headCtrl->AutoComplete(npcNames);
	}

	LoadPresetList();

	SetStatusText(wxString::Format(_("Loaded %s — %s"),
									data.GetFilename(), data.GetLoadInfo()));
	RefreshOutfitList();

	// Re-select the last outfit if one was persisted
	{
		std::string lastOutfit = Config["LLPreviewer/LastOutfit"];
		if (!lastOutfit.empty()) {
			for (long i = 0; i < outfitList->GetItemCount(); ++i) {
				if (outfitList->GetItemText(i).ToStdString() == lastOutfit) {
					outfitList->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
					outfitList->EnsureVisible(i);
					break;
				}
			}
		}
	}
}

void LeveledListPreviewer::OnReloadESP(wxCommandEvent&) {
	if (lastESPFilepath.empty())
		return;
	LoadESPFile(lastESPFilepath);
}

void LeveledListPreviewer::OnFileChanged(wxFileSystemWatcherEvent& event) {
	if (lastESPFilepath.empty())
		return;

	int type = event.GetChangeType();
	if (!((type & wxFSW_EVENT_MODIFY) || (type & wxFSW_EVENT_CREATE) || (type & wxFSW_EVENT_RENAME)))
		return;

	// Filter: only reload when our specific ESP file changed.
	// GetPath() returns the full path of the changed item when watching a directory.
	wxFileName eventFile = event.GetPath();
	wxFileName espFile = wxFileName::FileName(wxString::FromUTF8(lastESPFilepath));
	if (!eventFile.GetFullName().IsSameAs(espFile.GetFullName(), false))
		return;

	// Use CallAfter so the event handler returns quickly and the file has
	// finished being written before we reload.
	CallAfter([this]() {
		if (!lastESPFilepath.empty())
			LoadESPFile(lastESPFilepath);
	});
}

// ---------------------------------------------------------------------------
// Search / filter
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnSearchChanged(wxCommandEvent& WXUNUSED(event)) {
	RefreshOutfitList();
}

void LeveledListPreviewer::OnLevelChanged(wxSpinEvent& WXUNUSED(event)) {
	RefreshOutfitList();
}

void LeveledListPreviewer::RefreshOutfitList() {
	outfitList->DeleteAllItems();
	filteredOutfits.clear();

	std::string query = searchCtrl->GetValue().ToStdString();
	uint16_t minLv = static_cast<uint16_t>(levelMinSpin->GetValue());
	uint16_t maxLv = static_cast<uint16_t>(levelMaxSpin->GetValue());

	// Apply search filter
	auto searchResults = data.SearchByName(query);

	// Apply level filter
	for (auto* entry : searchResults) {
		if (entry->minLevel >= minLv && entry->minLevel <= maxLv)
			filteredOutfits.push_back(entry);
	}

	// Populate list control
	for (size_t i = 0; i < filteredOutfits.size(); ++i) {
		auto* entry = filteredOutfits[i];
		long idx = outfitList->InsertItem(static_cast<long>(i), wxString::FromUTF8(entry->name));
		outfitList->SetItem(idx, 1, wxString::Format("%d", entry->minLevel));
		outfitList->SetItem(idx, 2, wxString::Format("%zu", entry->pieces.size()));
	}
}

// ---------------------------------------------------------------------------
// Body mesh loading (base layer: body, hands, feet)
// ---------------------------------------------------------------------------

bool LeveledListPreviewer::LoadNifFromPath(const std::string& relativePath, const std::string& prefix) {
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	NifFile nif;
	bool loaded = false;

	// Try loose file (with case-insensitive fallback)
	std::string fullPath = baseGamePath + relativePath;
	if (wxFileName::FileExists(fullPath)) {
		loaded = (nif.Load(fullPath) == 0);
	}
	if (!loaded) {
		std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, relativePath);
		if (!resolved.empty())
			loaded = (nif.Load(resolved) == 0);
	}

	// Try BSA/BA2
	if (!loaded) {
		for (FSArchiveFile* archive : FSManager::archiveList()) {
			if (archive && archive->hasFile(relativePath)) {
				wxMemoryBuffer outData;
				archive->fileContents(relativePath, outData);
				if (!outData.IsEmpty()) {
					std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
					std::istringstream stream(content, std::istringstream::binary);
					loaded = (nif.Load(stream) == 0);
					break;
				}
			}
		}
	}

	if (!loaded)
		return false;

	for (auto& shapeName : nif.GetShapeNames()) {
		// Use prefix to avoid name collisions with outfit shapes
		std::string displayName = prefix.empty() ? shapeName : (prefix + shapeName);
		Mesh* m = gls.AddMeshFromNif(&nif, shapeName, nullptr, false);
		if (!m)
			continue;

		const std::vector<Color4>* vcolors = nif.GetColorsForShape(shapeName);
		if (vcolors) {
			for (size_t v = 0; v < vcolors->size(); v++) {
				m->vcolors[v].x = vcolors->at(v).r;
				m->vcolors[v].y = vcolors->at(v).g;
				m->vcolors[v].z = vcolors->at(v).b;
				m->valpha[v] = vcolors->at(v).a;
			}
		}

		m->CreateBuffers();
		AddNifShapeTextures(&nif, m->shapeName);
		bodyShapeNames.push_back(m->shapeName);
	}

	return true;
}

void LeveledListPreviewer::LoadBodyMeshes() {
	bodyRefVerts.clear();
	bodyRefUVs.clear();

	// Find BodySlide projects that output to the standard body NIF paths.
	FindBodySliderProjects();

	if (!bodyProjects.empty()) {
		// Load body from BodySlide reference NIFs so preset morphs can be applied
		// correctly — the morph diffs are relative to these files.
		for (auto& proj : bodyProjects) {
			NifFile refNif;
			if (refNif.Load(proj.refNifPath) != 0) {
				wxLogMessage("LeveledListPreviewer: Body ref NIF not found: %s", proj.refNifPath);
				continue;
			}

			for (auto& shapeName : refNif.GetShapeNames()) {
				Mesh* m = gls.AddMeshFromNif(&refNif, shapeName, nullptr, false);
				if (!m) continue;

				const std::vector<Color4>* vcolors = refNif.GetColorsForShape(shapeName);
				if (vcolors) {
					for (size_t v = 0; v < vcolors->size(); v++) {
						m->vcolors[v].x = vcolors->at(v).r;
						m->vcolors[v].y = vcolors->at(v).g;
						m->vcolors[v].z = vcolors->at(v).b;
						m->valpha[v]    = vcolors->at(v).a;
					}
				}
				m->CreateBuffers();
				AddNifShapeTextures(&refNif, shapeName);
				bodyShapeNames.push_back(m->shapeName);

				// Cache reference verts in NIF space — gls.Update expects this.
				auto* shape = refNif.FindBlockByName<NiShape>(shapeName);
				if (shape) {
					std::vector<Vector3> verts;
					refNif.GetVertsForShape(shape, verts);
					bodyRefVerts[shapeName] = std::move(verts);

					std::vector<Vector2> uvs;
					refNif.GetUvsForShape(shape, uvs);
					if (!uvs.empty())
						bodyRefUVs[shapeName] = std::move(uvs);
				}
			}
		}
	}

	if (bodyShapeNames.empty()) {
		// No slider projects found — fall back to plain game NIFs (no morphing)
		static const std::vector<std::string> bodyParts = {
			"meshes/actors/character/character assets/femalebody_1.nif",
			"meshes/actors/character/character assets/femalehands_1.nif",
			"meshes/actors/character/character assets/femalefeet_1.nif",
		};
		for (auto& part : bodyParts) {
			if (!LoadNifFromPath(part))
				wxLogMessage("LeveledListPreviewer: Body part not found: %s", part);
		}
	}

	// Apply current preset if one was chosen
	if (!currentPresetName.empty() && currentPresetName != "(none)")
		ApplyPresetToBody(currentPresetName);
}

// ---------------------------------------------------------------------------
// NPC head mesh loading
// ---------------------------------------------------------------------------

void LeveledListPreviewer::ClearHeadMeshes() {
	if (canvas && context)
		canvas->SetCurrent(*context);
	for (auto& name : headShapeNames)
		gls.DeleteMesh(name);
	headShapeNames.clear();
}

void LeveledListPreviewer::LoadHeadMesh(const lldata::NPCEntry& npc) {
	ClearHeadMeshes();

	// FaceGen NIF path: meshes/actors/character/FaceGenData/FaceGeom/{plugin}/{formid:08X}.nif
	char formIdBuf[16];
	std::snprintf(formIdBuf, sizeof(formIdBuf), "%08X", npc.formId);
	std::string relativePath = "meshes/actors/character/FaceGenData/FaceGeom/"
							   + npc.plugin + "/" + formIdBuf + ".nif";

	std::string resolvedPath = data.ResolveNifPath(relativePath);
	if (resolvedPath.empty()) {
		wxLogWarning("LeveledListPreviewer: FaceGen NIF not found: %s", relativePath);
		return;
	}

	NifFile nif;
	if (nif.Load(resolvedPath) != 0) {
		wxLogWarning("LeveledListPreviewer: Failed to load FaceGen NIF: %s", resolvedPath);
		return;
	}

	for (auto& shapeName : nif.GetShapeNames()) {
		std::string meshName = "_head_" + shapeName;
		Mesh* m = gls.AddMeshFromNif(&nif, shapeName, nullptr, false);
		if (!m)
			continue;
		m->shapeName = meshName;

		const std::vector<Color4>* vcolors = nif.GetColorsForShape(shapeName);
		if (vcolors) {
			for (size_t v = 0; v < vcolors->size(); v++) {
				m->vcolors[v].x = vcolors->at(v).r;
				m->vcolors[v].y = vcolors->at(v).g;
				m->vcolors[v].z = vcolors->at(v).b;
				m->valpha[v]    = vcolors->at(v).a;
			}
		}

		m->CreateBuffers();
		AddNifShapeTextures(&nif, shapeName, nullptr, meshName);
		headShapeNames.push_back(meshName);
	}

	wxLogMessage("LeveledListPreviewer: Loaded head NIF for NPC '%s' (%s): %zu shapes",
				 npc.editorId, formIdBuf, headShapeNames.size());
	gls.RenderOneFrame();
}

void LeveledListPreviewer::OnHeadEntered(wxCommandEvent& WXUNUSED(event)) {
	if (!headCtrl) return;

	std::string text = headCtrl->GetValue().ToStdString();
	if (text.empty()) {
		ClearHeadMeshes();
		currentHeadEditorId.clear();
		gls.RenderOneFrame();
		return;
	}

	// Check for exact match in display-name map
	auto it = npcByDisplay.find(text);
	if (it == npcByDisplay.end()) {
		// Try matching by editorId directly
		auto& npcs = data.GetNPCs();
		for (size_t i = 0; i < npcs.size(); ++i) {
			if (npcs[i].editorId == text) {
				it = npcByDisplay.find(npcs[i].displayName);
				break;
			}
		}
	}

	if (it == npcByDisplay.end()) {
		SetStatusText(wxString::Format(_("NPC not found: %s"), text));
		return;
	}

	auto& npc = data.GetNPCs()[it->second];
	currentHeadEditorId = npc.editorId;
	LoadHeadMesh(npc);
}

// ---------------------------------------------------------------------------
// BodySlide preset loading and application
// ---------------------------------------------------------------------------

void LeveledListPreviewer::LoadPresetList() {
	if (!presetCombo) return;

	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty()) projectPath = Config["AppDir"];
	std::string presetsDir = projectPath + "/SliderPresets";

	PresetCollection presets;
	std::vector<std::string> noGroups;
	presets.LoadPresets(presetsDir, "", noGroups, true);

	std::vector<std::string> names;
	presets.GetPresetNames(names);
	std::sort(names.begin(), names.end());

	presetCombo->Clear();
	presetCombo->Append(_("(none)"));
	for (auto& n : names)
		presetCombo->Append(wxString::FromUTF8(n));

	// Re-select previously chosen preset
	if (!currentPresetName.empty()) {
		int idx = presetCombo->FindString(wxString::FromUTF8(currentPresetName));
		if (idx != wxNOT_FOUND)
			presetCombo->SetSelection(idx);
		else
			presetCombo->SetSelection(0);
	}
	else {
		presetCombo->SetSelection(0);
	}
}

void LeveledListPreviewer::FindBodySliderProjects() {
	bodyProjects.clear();

	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty()) projectPath = Config["AppDir"];
	std::string sliderSetsDir = projectPath + "/SliderSets";

	// Body output NIF filenames (lower-case) to search for.
	// Use a set to track which output files are already claimed (first match wins).
	static const std::set<std::string> bodyOutputFiles = {
		"femalebody_1.nif", "femalehands_1.nif", "femalefeet_1.nif"
	};
	std::set<std::string> claimed;

	wxArrayString ospFiles;
	wxDir::GetAllFiles(wxString::FromUTF8(sliderSetsDir), &ospFiles, "*.osp", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(sliderSetsDir), &ospFiles, "*.xml", wxDIR_FILES);

	for (auto& ospPath : ospFiles) {
		SliderSetFile ssf(ospPath.ToStdString());
		if (ssf.fail()) continue;

		std::vector<std::string> setNames;
		ssf.GetSetNames(setNames);

		for (auto& setName : setNames) {
			std::string outFilePath;
			ssf.GetSetOutputFilePath(setName, outFilePath);
			if (outFilePath.empty()) continue;

			// Extract just the filename, compare case-insensitively
			auto pos = outFilePath.find_last_of("/\\");
			std::string outFile = (pos != std::string::npos) ? outFilePath.substr(pos + 1) : outFilePath;
			std::transform(outFile.begin(), outFile.end(), outFile.begin(), ::tolower);

			if (!bodyOutputFiles.count(outFile)) continue;
			if (claimed.count(outFile)) continue; // first match wins

			SliderSet ss;
			if (ssf.GetSet(setName, ss)) continue;
			ss.SetBaseDataPath(projectPath + "/ShapeData");
			std::string refNifPath = ss.GetInputFileName();

			if (!wxFileName::FileExists(wxString::FromUTF8(refNifPath))) continue;

			BodyProject proj;
			proj.refNifPath    = refNifPath;
			proj.sliderSetFile = ospPath.ToStdString();
			proj.setName       = setName;
			bodyProjects.push_back(std::move(proj));
			claimed.insert(outFile);

			wxLogMessage("LeveledListPreviewer: Body project '%s' → %s (ref: %s)",
						 setName, outFile, refNifPath);
		}
	}

	wxLogMessage("LeveledListPreviewer: Found %zu body slider project(s)", bodyProjects.size());
}

void LeveledListPreviewer::ApplyPresetToBody(const std::string& presetName) {
	if (bodyRefVerts.empty()) return;

	if (canvas && context)
		canvas->SetCurrent(*context);

	// "(none)" or empty → reset to reference shape (zero preset)
	if (presetName.empty() || presetName == "(none)") {
		// bodyRefVerts are in NIF space — gls.Update applies TransformPosNifToMesh
		for (auto& kv : bodyRefVerts) {
			auto uvIt = bodyRefUVs.find(kv.first);
			gls.Update(kv.first, &kv.second,
					   uvIt != bodyRefUVs.end() ? &uvIt->second : nullptr);
		}
		gls.RenderOneFrame();
		return;
	}

	if (bodyProjects.empty()) return;

	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty()) projectPath = Config["AppDir"];

	for (auto& proj : bodyProjects) {
		SliderSetFile ssf(proj.sliderSetFile);
		if (ssf.fail()) continue;

		SliderSet ss;
		if (ssf.GetSet(proj.setName, ss)) continue;
		ss.SetBaseDataPath(projectPath + "/ShapeData");

		DiffDataSets diffData;
		ss.LoadSetDiffData(diffData);

		SliderManager sliderMgr;
		sliderMgr.AddSlidersInSet(ss);

		std::vector<std::string> noGroups;
		sliderMgr.LoadPresets(projectPath + "/SliderPresets", proj.setName, noGroups, true);
		sliderMgr.InitializeSliders(presetName);

		for (auto it = ss.ShapesBegin(); it != ss.ShapesEnd(); ++it) {
			const std::string& shapeName   = it->first;
			const std::string& targetShape = it->second.targetShape;

			// Start from cached NIF-space reference verts (same file the diffs were built from)
			auto refIt = bodyRefVerts.find(shapeName);
			if (refIt == bodyRefVerts.end()) continue;
			if (!gls.GetMesh(shapeName)) continue;

			std::vector<Vector3> verts = refIt->second; // copy — will be modified

			// Apply all big-weight slider diffs (diffs are also in NIF space)
			for (auto& slider : sliderMgr.slidersBig) {
				if (slider.zap) continue;
				float val = slider.invert ? 1.0f - slider.value : slider.value;
				for (auto& ds : slider.linkedDataSets)
					diffData.ApplyDiff(ds, targetShape, val, &verts);
			}

			auto uvIt = bodyRefUVs.find(shapeName);
			gls.Update(shapeName, &verts,
					   uvIt != bodyRefUVs.end() ? &uvIt->second : nullptr);
		}
	}

	gls.RenderOneFrame();
}

void LeveledListPreviewer::OnPresetChanged(wxCommandEvent& WXUNUSED(event)) {
	if (!presetCombo) return;
	currentPresetName = presetCombo->GetValue().ToStdString();
	ApplyPresetToBody(currentPresetName);
}

// ---------------------------------------------------------------------------
// Outfit selection → 3D preview
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnOutfitSelected(wxListEvent& event) {
	long sel = event.GetIndex();
	if (sel < 0 || sel >= static_cast<long>(filteredOutfits.size()))
		return;

	LoadOutfitMeshes(*filteredOutfits[sel]);
}

void LeveledListPreviewer::LoadOutfitMeshes(const lldata::OutfitEntry& outfit) {
	// Clear existing meshes
	if (canvas && context)
		canvas->SetCurrent(*context);

	gls.Cleanup();
	shapeMaterials.clear();
	untexturedShapes.clear();
	bodyShapeNames.clear();
	headShapeNames.clear();

	// Load body base layer first (body, hands, feet)
	if (showBody)
		LoadBodyMeshes();

	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	int loadedCount = 0;

	for (auto& piece : outfit.pieces) {
		if (piece.nifPath.empty()) {
			wxLogMessage("  Piece '%s' [%08X]: no model path, skipping", piece.name, piece.formId);
			continue;
		}

		wxLogMessage("  Piece '%s' [%08X]: nifPath=%s", piece.name, piece.formId, piece.nifPath);

		std::string resolvedPath = data.ResolveNifPath(piece.nifPath);
		if (resolvedPath.empty()) {
			wxLogWarning("  NIF not found: %s", piece.nifPath);
			continue;
		}

		wxLogMessage("  Resolved to: %s", resolvedPath);

		// Load NIF file
		NifFile nif;
		std::string fullPath = resolvedPath;

		// If resolvedPath is relative (from archive), try loading from archive
		if (resolvedPath == piece.nifPath) {
			// Try loading from BSA/BA2
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (archive && archive->hasFile(piece.nifPath)) {
					wxMemoryBuffer outData;
					archive->fileContents(piece.nifPath, outData);
					if (!outData.IsEmpty()) {
						std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
						std::istringstream stream(content, std::istringstream::binary);
						if (nif.Load(stream) == 0)
							goto nifLoaded;
					}
				}
			}
			continue; // Could not load from archive
		}
		else {
			// Loose file
			if (nif.Load(fullPath) != 0)
				continue;
		}

	nifLoaded:
		// Add all shapes from NIF.
		// Prefix each shape name with the piece's FormID to avoid collisions when
		// multiple pieces share shape names (e.g. both have a "Body" shape).
		char piecePrefixBuf[16];
		std::snprintf(piecePrefixBuf, sizeof(piecePrefixBuf), "%08X_", piece.formId);
		std::string piecePrefix = piecePrefixBuf;
		for (auto& shapeName : nif.GetShapeNames()) {
			Mesh* m = gls.AddMeshFromNif(&nif, shapeName, nullptr, false);
			if (!m)
				continue;

			// Rename to avoid collisions with identically-named shapes from other pieces
			m->shapeName = piecePrefix + shapeName;

			// Vertex colors
			const std::vector<Color4>* vcolors = nif.GetColorsForShape(shapeName);
			if (vcolors) {
				for (size_t v = 0; v < vcolors->size(); v++) {
					m->vcolors[v].x = vcolors->at(v).r;
					m->vcolors[v].y = vcolors->at(v).g;
					m->vcolors[v].z = vcolors->at(v).b;
					m->valpha[v] = vcolors->at(v).a;
				}
			}

			m->CreateBuffers();
			const std::vector<lldata::TextureOverride>* overrides =
				piece.textureOverrides.empty() ? nullptr : &piece.textureOverrides;
			// Pass original shapeName for texture override matching (overrides use NIF shape names)
			bool hasTexture = AddNifShapeTextures(&nif, shapeName, overrides, m->shapeName);
			if (!hasTexture) {
				untexturedShapes.push_back(m->shapeName);
				if (!showUntextured) {
					wxLogWarning("  HIDING shape '%s' (no diffuse)", wxString(m->shapeName));
					gls.SetMeshVisibility(m->shapeName, false);
				}
			}
			++loadedCount;
		}
	}

	// Reload head mesh for the currently selected NPC
	if (!currentHeadEditorId.empty()) {
		auto& npcs = data.GetNPCs();
		for (auto& npc : npcs) {
			if (npc.editorId == currentHeadEditorId) {
				LoadHeadMesh(npc);
				break;
			}
		}
	}

	gls.RenderOneFrame();
	wxLog::FlushActive();
	SetStatusText(wxString::Format(_("Outfit: %s — %d meshes loaded"),
									outfit.name, loadedCount));
}

// ---------------------------------------------------------------------------
// Texture loading (same pattern as PreviewWindow)
// ---------------------------------------------------------------------------

bool LeveledListPreviewer::AddNifShapeTextures(NifFile* fromNif, const std::string& shapeName,
												const std::vector<lldata::TextureOverride>* overrides,
												const std::string& meshName) {
	bool hasMat = false;
	std::string matFile;

	const uint8_t MAX_TEXTURE_PATHS = 10;
	std::vector<std::string> texFiles(MAX_TEXTURE_PATHS);

	NiShader* shader = nullptr;
	auto shape = fromNif->FindBlockByName<NiShape>(shapeName);
	if (shape) {
		shader = fromNif->GetShader(shape);
		if (shader) {
			if (fromNif->GetHeader().GetVersion().IsFO4() || fromNif->GetHeader().GetVersion().IsFO76()) {
				matFile = shader->name.get();
				if (!matFile.empty())
					hasMat = true;
			}
		}
	}

	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	MaterialFile mat(MaterialFile::BGSM);
	if (hasMat) {
		matFile = std::regex_replace(matFile, std::regex("\\\\+"), "/");
		matFile = std::regex_replace(matFile, std::regex("^(.*?)/materials/", std::regex_constants::icase), "");
		matFile = std::regex_replace(matFile, std::regex("^/+"), "");
		matFile = std::regex_replace(matFile, std::regex("^(?!^materials/)", std::regex_constants::icase), "materials/");

		mat = MaterialFile(baseGamePath + matFile);

		if (mat.Failed()) {
			wxMemoryBuffer mdata;
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (archive && archive->hasFile(matFile)) {
					wxMemoryBuffer outData;
					archive->fileContents(matFile, outData);
					if (!outData.IsEmpty()) {
						mdata = std::move(outData);
						break;
					}
				}
			}
			if (!mdata.IsEmpty()) {
				std::string content(static_cast<char*>(mdata.GetData()), mdata.GetDataLen());
				std::istringstream contentStream(content, std::istringstream::binary);
				mat = MaterialFile(contentStream);
			}
		}

		if (!mat.Failed()) {
			if (mat.signature == MaterialFile::BGSM) {
				texFiles[0] = mat.diffuseTexture.c_str();
				texFiles[1] = mat.normalTexture.c_str();
				texFiles[2] = mat.glowTexture.c_str();
				texFiles[3] = mat.greyscaleTexture.c_str();
				texFiles[4] = mat.envmapTexture.c_str();
				texFiles[7] = mat.smoothSpecTexture.c_str();
			}
			else if (mat.signature == MaterialFile::BGEM) {
				texFiles[0] = mat.baseTexture.c_str();
				texFiles[1] = mat.fxNormalTexture.c_str();
				texFiles[3] = mat.grayscaleTexture.c_str();
				texFiles[4] = mat.fxEnvmapTexture.c_str();
				texFiles[5] = mat.envmapMaskTexture.c_str();
			}
		}
		else if (shader) {
			hasMat = false;
			for (int i = 0; i < MAX_TEXTURE_PATHS; i++)
				fromNif->GetTextureSlot(shape, texFiles[i], i);
		}
	}
	else if (shader) {
		for (int i = 0; i < MAX_TEXTURE_PATHS; i++)
			fromNif->GetTextureSlot(shape, texFiles[i], i);
	}

	// Apply alternate texture overrides from ESP (ARMA MO3S → TXST)
	if (overrides) {
		for (auto& ovr : *overrides) {
			// Case-insensitive shape name comparison
			if (ovr.shapeName.size() == shapeName.size() &&
				std::equal(ovr.shapeName.begin(), ovr.shapeName.end(), shapeName.begin(),
						   [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
				for (int i = 0; i < 8; i++) {
					if (!ovr.textures[i].empty())
						texFiles[i] = ovr.textures[i];
				}
				break;
			}
		}
	}

	for (int i = 0; i < MAX_TEXTURE_PATHS; i++) {
		if (!texFiles[i].empty()) {
			texFiles[i] = std::regex_replace(texFiles[i], std::regex("\\\\+"), "/");
			texFiles[i] = std::regex_replace(texFiles[i], std::regex("^(.*?)/textures/", std::regex_constants::icase), "");
			texFiles[i] = std::regex_replace(texFiles[i], std::regex("^/+"), "");
			texFiles[i] = std::regex_replace(texFiles[i], std::regex("^(?!^textures/)", std::regex_constants::icase), "textures/");

			// Try exact path first, then case-insensitive resolution for Linux
			std::string fullPath = baseGamePath + texFiles[i];
			if (wxFileName::FileExists(fullPath)) {
				texFiles[i] = fullPath;
			}
			else {
				std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, texFiles[i]);
				texFiles[i] = resolved.empty() ? fullPath : resolved;
			}
		}
	}

	std::string vShader = Config["AppDir"] + "/res/shaders/default.vert";
	std::string fShader = Config["AppDir"] + "/res/shaders/default.frag";

	TargetGame targetGame = (TargetGame)Config.GetIntValue("TargetGame");
	if (targetGame == FO4 || targetGame == FO4VR || targetGame == FO76) {
		vShader = Config["AppDir"] + "/res/shaders/fo4_default.vert";
		fShader = Config["AppDir"] + "/res/shaders/fo4_default.frag";
	}
	else if (targetGame == OB) {
		vShader = Config["AppDir"] + "/res/shaders/ob_default.vert";
		fShader = Config["AppDir"] + "/res/shaders/ob_default.frag";
	}

	// Check if we have a diffuse texture (slot 0)
	bool hasDiffuse = false;
	if (!texFiles[0].empty()) {
		if (wxFileName::FileExists(texFiles[0])) {
			hasDiffuse = true;
		}
		else {
			// Check BSA/BA2 for diffuse texture
			std::string relTex = texFiles[0];
			if (relTex.size() > baseGamePath.size() && relTex.substr(0, baseGamePath.size()) == baseGamePath)
				relTex = relTex.substr(baseGamePath.size());
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (archive && archive->hasFile(relTex)) {
					hasDiffuse = true;
					break;
				}
			}
		}
	}

	if (!texFiles[0].empty())
		wxLogMessage("  tex[0] for '%s': %s (exists=%d)",
					 wxString(shapeName), wxString(texFiles[0]),
					 wxFileName::FileExists(texFiles[0]) ? 1 : 0);

	const std::string& lookupName = meshName.empty() ? shapeName : meshName;
	Mesh* m = gls.GetMesh(lookupName);
	if (!m) {
		wxLogWarning("  GetMesh returned null for '%s'", wxString(lookupName));
		return false;
	}

	GLMaterial* glMat = gls.AddMaterial(texFiles, vShader, fShader);
	if (glMat) {
		m->material = glMat;
		shapeMaterials[lookupName] = glMat;

		if (hasMat)
			m->UpdateFromMaterialFile(mat);

		gls.UpdateShaders(m);
	}
	else {
		wxLogWarning("  AddMaterial returned null for '%s'", wxString(shapeName));
	}

	wxLogMessage("  result for '%s': hasDiffuse=%d glMat=%s visible=%d",
				 wxString(shapeName), hasDiffuse ? 1 : 0,
				 glMat ? "ok" : "NULL",
				 m->bVisible ? 1 : 0);

	return hasDiffuse;
}

// ---------------------------------------------------------------------------
// Toggle untextured meshes
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnToggleUntextured(wxCommandEvent& event) {
	showUntextured = event.IsChecked();
	for (auto& name : untexturedShapes)
		gls.SetMeshVisibility(name, showUntextured);
	gls.RenderOneFrame();
}

void LeveledListPreviewer::OnToggleBody(wxCommandEvent& event) {
	showBody = event.IsChecked();
	for (auto& name : bodyShapeNames)
		gls.SetMeshVisibility(name, showBody);
	gls.RenderOneFrame();
}

// ---------------------------------------------------------------------------
// Camera controls
// ---------------------------------------------------------------------------

void LeveledListPreviewer::RightDrag(int dX, int dY) {
	gls.TurnTableCamera(dX);
	gls.PitchCamera(dY);
	gls.RenderOneFrame();
}

void LeveledListPreviewer::LeftDrag(int dX, int dY) {
	gls.PanCamera(dX, dY);
	gls.RenderOneFrame();
}

void LeveledListPreviewer::TrackMouse(int X, int Y) {
	gls.UpdateCursor(X, Y);
	gls.RenderOneFrame();
}

void LeveledListPreviewer::MouseWheel(int dW) {
	gls.DollyCamera(dW);
	gls.RenderOneFrame();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnClose(wxCloseEvent& WXUNUSED(event)) {
	if (canvas && context)
		canvas->SetCurrent(*context);

	gls.Cleanup();
	shapeMaterials.clear();

	// Persist window layout
	bool maximized = IsMaximized();
	Config.SetBoolValue("LLPreviewer/Maximized", maximized);
	if (!maximized) {
		wxPoint pos = GetPosition();
		wxSize  sz  = GetSize();
		Config.SetValue("LLPreviewer/WindowX", pos.x);
		Config.SetValue("LLPreviewer/WindowY", pos.y);
		Config.SetValue("LLPreviewer/WindowW", sz.x);
		Config.SetValue("LLPreviewer/WindowH", sz.y);
	}
	if (splitter)
		Config.SetValue("LLPreviewer/SashPos", splitter->GetSashPosition());
	if (outfitList) {
		Config.SetValue("LLPreviewer/ColW0", outfitList->GetColumnWidth(0));
		Config.SetValue("LLPreviewer/ColW1", outfitList->GetColumnWidth(1));
		Config.SetValue("LLPreviewer/ColW2", outfitList->GetColumnWidth(2));

		// Persist selected outfit name
		long sel = outfitList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (sel >= 0)
			Config.SetValue("LLPreviewer/LastOutfit", outfitList->GetItemText(sel).ToStdString());
	}

	// Persist head NPC and preset
	Config.SetValue("LLPreviewer/HeadEditorId", currentHeadEditorId);
	Config.SetValue("LLPreviewer/PresetName",   currentPresetName);

	int ret = Config.SaveConfig(Config["AppDir"] + "/Config.xml");
	if (ret)
		wxLogWarning("LeveledListPreviewer: failed to save Config.xml (%d).", ret);

	Destroy();
	if (app)
		app->LeveledListPreviewerClosed();
}

// ---------------------------------------------------------------------------
// LLPreviewCanvas
// ---------------------------------------------------------------------------

wxBEGIN_EVENT_TABLE(LLPreviewCanvas, wxGLCanvas)
	EVT_KEY_UP(LLPreviewCanvas::OnKeyUp)
	EVT_MOTION(LLPreviewCanvas::OnMotion)
	EVT_MOUSEWHEEL(LLPreviewCanvas::OnMouseWheel)
	EVT_PAINT(LLPreviewCanvas::OnPaint)
	EVT_SIZE(LLPreviewCanvas::OnResized)
wxEND_EVENT_TABLE()

LLPreviewCanvas::LLPreviewCanvas(LeveledListPreviewer* pw, wxWindow* parent, const wxGLAttributes& attribs)
	: wxGLCanvas(parent, attribs, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
	, previewer(pw) {}

void LLPreviewCanvas::OnPaint(wxPaintEvent& WXUNUSED(event)) {
	if (firstPaint) {
		firstPaint = false;
		previewer->OnShown();
	}
	previewer->Render();
}

void LLPreviewCanvas::OnKeyUp(wxKeyEvent& event) {
	switch (event.GetKeyCode()) {
		case 'T': previewer->ToggleTextures(); break;
		case 'W': previewer->ToggleWireframe(); break;
		case 'U':
		case 'B': {
			int menuId = (event.GetKeyCode() == 'U')
				? LeveledListPreviewer::ID_ToggleUntextured
				: LeveledListPreviewer::ID_ToggleBody;
			wxMenuBar* mb = previewer->GetMenuBar();
			if (mb) {
				bool newState = !mb->IsChecked(menuId);
				mb->Check(menuId, newState);
				wxCommandEvent evt(wxEVT_MENU, menuId);
				evt.SetInt(newState ? 1 : 0);
				previewer->GetEventHandler()->ProcessEvent(evt);
			}
			break;
		}
	}
}

void LLPreviewCanvas::OnMotion(wxMouseEvent& event) {
	if (previewer->IsActive())
		SetFocus();

	auto delta = event.GetPosition() - lastMousePosition;

	if (event.LeftIsDown())
		previewer->LeftDrag(delta.x, delta.y);
	else if (event.MiddleIsDown()) {
		if (wxGetKeyState(WXK_SHIFT))
			previewer->MouseWheel(delta.y);
		else
			previewer->LeftDrag(delta.x, delta.y);
	}
	else if (event.RightIsDown()) {
		if (wxGetKeyState(WXK_SHIFT))
			previewer->LeftDrag(delta.x, delta.y);
		else
			previewer->RightDrag(delta.x, delta.y);
	}
	else
		previewer->TrackMouse(event.GetX(), event.GetY());

	lastMousePosition = event.GetPosition();
}

void LLPreviewCanvas::OnMouseWheel(wxMouseEvent& event) {
	previewer->MouseWheel(event.GetWheelRotation());
}

void LLPreviewCanvas::OnResized(wxSizeEvent& event) {
	previewer->Resized(event.GetSize().GetWidth(), event.GetSize().GetHeight());
}
