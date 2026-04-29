/*
 * LeveledListPreviewer — outfit browser and 3D preview for leveled list ESPs.
 */

#include "LeveledListPreviewer.h"
#include "BodySlideApp.h"
#include "../components/Anim.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <ExtraData.hpp>
#include <wx/dir.h>
#include <wx/filename.h>

using namespace nifly;

wxDEFINE_EVENT(wxEVT_LEVELEDLIST_CLOSED, wxCommandEvent);

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
	EVT_CHECKBOX(LeveledListPreviewer::ID_HighWeight, LeveledListPreviewer::OnHighWeightChanged)
	EVT_TIMER(LeveledListPreviewer::ID_SmpTimer, LeveledListPreviewer::OnSmpTimer)
	EVT_FSWATCHER(wxID_ANY, LeveledListPreviewer::OnFileChanged)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LeveledListPreviewer::LeveledListPreviewer(wxEvtHandler* handler)
	: wxFrame(nullptr, wxID_ANY, _("Leveled List Previewer"), wxDefaultPosition, wxDefaultSize)
	, eventHandler(handler)
	, smpTimer_(this, ID_SmpTimer) {
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

	// Preset selector + high/low weight checkbox
	wxBoxSizer* presetSizer = new wxBoxSizer(wxHORIZONTAL);
	presetSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Preset:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	presetCombo = new wxComboBox(leftPanel, ID_Preset, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxArrayString{}, wxCB_READONLY | wxCB_DROPDOWN);
	presetCombo->SetToolTip(_("Choose a BodySlide preset to morph the preview body"));
	presetSizer->Add(presetCombo, 1, wxRIGHT, 5);
	highWeightCheck = new wxCheckBox(leftPanel, ID_HighWeight, _("Hi"));
	highWeightCheck->SetValue(true);
	highWeightCheck->SetToolTip(_("High weight (_1) when checked, low weight (_0) when unchecked"));
	presetSizer->Add(highWeightCheck, 0, wxALIGN_CENTER_VERTICAL);
	leftSizer->Add(presetSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// SMP physics toggle
	wxBoxSizer* smpSizer = new wxBoxSizer(wxHORIZONTAL);
	smpToggle_ = new wxCheckBox(leftPanel, ID_ToggleSmp, _("SMP Physics Preview"));
	smpToggle_->SetToolTip(_("Enable real-time cloth/hair physics simulation for SMP-enabled outfits"));
	smpToggle_->Bind(wxEVT_CHECKBOX, &LeveledListPreviewer::OnToggleSmp, this);
	smpSizer->Add(smpToggle_, 0, wxALIGN_CENTER_VERTICAL);
	leftSizer->Add(smpSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);


	// Outfit list
	outfitList = new wxListCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	{
		int w0 = Config.GetIntValue("LLPreviewer/ColW0");
		int w1 = Config.GetIntValue("LLPreviewer/ColW1");
		int w2 = Config.GetIntValue("LLPreviewer/ColW2");
		outfitList->AppendColumn(_("Name"), wxLIST_FORMAT_LEFT, w0 > 0 ? w0 : 200);
		outfitList->AppendColumn(_("Level"), wxLIST_FORMAT_RIGHT, w1 > 0 ? w1 : 50);
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

	// Mesh list overlay — floating panel over the GL canvas, not in sizer
	meshOverlayPanel = new wxScrolledWindow(rightPanel, wxID_ANY, wxDefaultPosition, wxSize(220, 100), wxVSCROLL | wxBORDER_SIMPLE);
	meshOverlayPanel->SetScrollRate(0, 8);
	meshOverlayPanel->Hide();

	canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& ev) {
		ev.Skip();
		RepositionMeshOverlay();
	});

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

	// Restore persisted head / preset / weight / outfit selections
	currentHeadEditorId = Config["LLPreviewer/HeadEditorId"];
	currentPresetName = Config["LLPreviewer/PresetName"];
	useHighWeight = (Config["LLPreviewer/HighWeight"] != "false");
	if (headCtrl && !currentHeadEditorId.empty()) {
		// Prefer the in-game display name over the editor ID.
		std::string display = currentHeadEditorId;
		for (auto& n : data.GetNPCs()) {
			if (n.editorId == currentHeadEditorId) {
				if (!n.displayName.empty())
					display = n.displayName;
				break;
			}
		}
		headCtrl->SetValue(wxString::FromUTF8(display));
	}
	if (highWeightCheck)
		highWeightCheck->SetValue(useHighWeight);

	Show();

	// Offer to reload last ESP after window is shown
	if (!lastFile.empty() && wxFileName::FileExists(lastFile)) {
		CallAfter([this, lastFile]() {
			if (wxMessageBox(wxString::Format(_("Reload last ESP?\n%s"), lastFile), _("Leveled List Previewer"), wxYES_NO | wxICON_QUESTION, this) == wxYES) {
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

	gls.UpdateLights(ambient,
					 frontal,
					 d0,
					 d1,
					 d2,
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
	wxFileDialog dlg(this, _("Open ESP/ESM File"), lastESPDirectory, "", "Elder Scrolls Plugin (*.esp;*.esm;*.esl)|*.esp;*.esm;*.esl", wxFD_OPEN | wxFD_FILE_MUST_EXIST);

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
	fsWatcher->Add(wxFileName::DirName(espFileName.GetPath()), wxFSW_EVENT_MODIFY | wxFSW_EVENT_CREATE | wxFSW_EVENT_RENAME);

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

	// Re-sync headCtrl's text: on startup OnShown runs before LoadNPCs, so a
	// persisted editor id could not yet be mapped to a display name.
	if (headCtrl && !currentHeadEditorId.empty()) {
		for (auto& n : data.GetNPCs()) {
			if (n.editorId == currentHeadEditorId) {
				if (!n.displayName.empty())
					headCtrl->ChangeValue(wxString::FromUTF8(n.displayName));
				break;
			}
		}
	}

	LoadPresetList();

	SetStatusText(wxString::Format(_("Loaded %s — %s"), data.GetFilename(), data.GetLoadInfo()));
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
// .tri file loading helper
// ---------------------------------------------------------------------------

TriFile* LeveledListPreviewer::GetOrLoadTriFile(const std::string& nifRelativePath) {
	auto it = triFileCache_.find(nifRelativePath);
	if (it != triFileCache_.end())
		return &it->second;

	// Derive .tri path from the NIF path: strip _0/_1 weight suffix, replace .nif with .tri
	std::string triRelPath = nifRelativePath;
	if (triRelPath.size() > 6) {
		std::string ending = triRelPath.substr(triRelPath.size() - 6);
		std::transform(ending.begin(), ending.end(), ending.begin(), ::tolower);
		if (ending == "_0.nif" || ending == "_1.nif") {
			triRelPath = triRelPath.substr(0, triRelPath.size() - 6) + ".tri";
		}
		else if (triRelPath.size() > 4) {
			std::string ext = triRelPath.substr(triRelPath.size() - 4);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == ".nif")
				triRelPath = triRelPath.substr(0, triRelPath.size() - 4) + ".tri";
		}
	}
	else if (triRelPath.size() > 4) {
		std::string ext = triRelPath.substr(triRelPath.size() - 4);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".nif")
			triRelPath = triRelPath.substr(0, triRelPath.size() - 4) + ".tri";
	}

	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	// Try loose file (with case-insensitive fallback)
	std::string fullTriPath = baseGamePath + triRelPath;
	TriFile tri;
	bool loaded = false;

	if (wxFileName::FileExists(fullTriPath)) {
		loaded = tri.Read(fullTriPath);
	}
	if (!loaded) {
		std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, triRelPath);
		if (!resolved.empty())
			loaded = tri.Read(resolved);
	}

	if (!loaded)
		return nullptr;

	wxLogMessage("LeveledListPreviewer: Loaded .tri file for '%s' (%u shapes)", nifRelativePath, tri.GetShapeCount(MORPHTYPE_POSITION));
	auto [insIt, ok] = triFileCache_.emplace(nifRelativePath, std::move(tri));
	return &insIt->second;
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
		Mesh* m = gls.AddMeshFromNif(&nif, shapeName, displayName, nullptr, false);
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
		AddNifShapeTextures(&nif, shapeName, nullptr, m->shapeName);
		if (std::find(bodyShapeNames.begin(), bodyShapeNames.end(), m->shapeName) == bodyShapeNames.end())
			bodyShapeNames.push_back(m->shapeName);
		shapeNifSource[m->shapeName] = relativePath;

		// Extract partition body part IDs from NIF dismember skin
		auto* shape = nif.FindBlockByName<NiShape>(shapeName);
		if (shape) {
			NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
			std::vector<int> triParts;
			if (nif.GetShapePartitions(shape, partInfo, triParts)) {
				std::set<uint16_t> partIds;
				for (size_t i = 0; i < partInfo.size(); ++i)
					partIds.insert(partInfo[i].partID);
				bodyShapePartMap[m->shapeName] = std::move(partIds);
				wxLogMessage("  Body shape '%s' partition IDs: %zu entries", m->shapeName, bodyShapePartMap[m->shapeName].size());
			}
		}
	}

	return true;
}

void LeveledListPreviewer::LoadBodyMeshes() {
	// Selectively clear race-body shape info while preserving entries belonging
	// to head shapes AND outfit shapes. ReconcileBodyAndOutfitShapes relies on
	// bodyShapePartMap entries for outfit shapes (populated during outfit load)
	// to know which slots an outfit covers and to retarget race textures onto
	// the outfit's reference body shape — wiping them here breaks the deferred
	// reload path on initial load.
	{
		std::set<std::string> raceBodySet(bodyShapeNames.begin(), bodyShapeNames.end());

		for (auto it = bodyGameVerts.begin(); it != bodyGameVerts.end();) {
			if (raceBodySet.count(it->first)) it = bodyGameVerts.erase(it);
			else ++it;
		}
		for (auto it = bodyShapePartMap.begin(); it != bodyShapePartMap.end();) {
			if (raceBodySet.count(it->first)) it = bodyShapePartMap.erase(it);
			else ++it;
		}
	}

	// Determine which body NIF paths to load.
	//
	// Game-accurate skin resolution chain when an NPC is selected:
	//   NIFs:     NPC's WNAM (skin ARMO) → race's WNAM ARMO. No vanilla fallback;
	//             a slot the chain cannot resolve is an error (status bar + log).
	//   Textures: NPC's WNAM TXST → race's WNAM TXST → NIF's embedded BSShaderTextureSet
	//             (i.e. the textures shipped with the chosen skin NIF).
	//
	// Only load body parts whose slots are NOT already provided by the outfit.
	// If the outfit declares e.g. slot 32, it supplies its own body mesh and we
	// must not load the default/WNAM body underneath it.
	const std::string suffix = useHighWeight ? "_1.nif" : "_0.nif";

	struct SlotNif {
		int slot;
		std::string path;
	};
	std::vector<SlotNif> bodyNifs;

	if (!currentHeadEditorId.empty()) {
		auto paths = data.ResolveNpcBodyNifPaths(currentHeadEditorId, useHighWeight);
		wxLogMessage("LeveledListPreviewer: ResolveNpcBodyNifPaths('%s') -> body='%s' hands='%s' feet='%s'",
					 currentHeadEditorId, paths.body, paths.hands, paths.feet);

		if (!paths.body.empty())
			bodyNifs.push_back({32, paths.body});
		if (!paths.hands.empty())
			bodyNifs.push_back({33, paths.hands});
		if (!paths.feet.empty())
			bodyNifs.push_back({37, paths.feet});

		// Report any slots the NPC/race chain failed to provide (and that the outfit
		// doesn't already cover). No vanilla femalebody fallback under an NPC.
		std::vector<std::string> missing;
		if (paths.body.empty() && !outfitDeclaredSlots.count(32))
			missing.emplace_back("body");
		if (paths.hands.empty() && !outfitDeclaredSlots.count(33))
			missing.emplace_back("hands");
		if (paths.feet.empty() && !outfitDeclaredSlots.count(37))
			missing.emplace_back("feet");

		if (!missing.empty()) {
			std::string slotList;
			for (size_t i = 0; i < missing.size(); ++i) {
				if (i)
					slotList += ", ";
				slotList += missing[i];
			}
			std::string err = "NPC '" + currentHeadEditorId
							  + "': no skin NIF defined for " + slotList
							  + " (neither NPC's WNAM nor race's WNAM resolves)";
			wxLogError("LeveledListPreviewer: %s", err.c_str());
			SetStatusText(wxString::FromUTF8(err));
		}
		else if (!bodyNifs.empty()) {
			wxLogMessage("LeveledListPreviewer: Using NPC '%s' body meshes (NPC->race chain)", currentHeadEditorId);
		}
	}
	else {
		// No NPC selected — fall back to vanilla femalebody so the outfit can be
		// previewed standalone. This is the only path that may use defaults.
		bodyNifs.push_back({32, "meshes/actors/character/character assets/femalebody" + suffix});
		bodyNifs.push_back({33, "meshes/actors/character/character assets/femalehands" + suffix});
		bodyNifs.push_back({37, "meshes/actors/character/character assets/femalefeet" + suffix});
	}

	for (auto& entry : bodyNifs) {
		if (!LoadNifFromPath(entry.path)) {
			wxLogMessage("LeveledListPreviewer: Body part not found: %s", entry.path);
			continue;
		}
		// When the outfit's ARMO already covers this slot, hide the race body
		// shape so we don't double-render. We still loaded the NIF so its
		// BSShaderTextureSet is captured into shapeTexFiles — that lets
		// ReconcileBodyAndOutfitShapes retarget the race skin texture onto the
		// outfit's bundled body shape afterwards.
		if (outfitDeclaredSlots.count(entry.slot)) {
			wxLogMessage("  Race body slot %d loaded but hidden — outfit ARMO declares this slot", entry.slot);
			for (auto& raceShape : bodyShapeNames) {
				auto pit = bodyShapePartMap.find(raceShape);
				if (pit != bodyShapePartMap.end()
					&& pit->second.count(static_cast<uint16_t>(entry.slot))) {
					gls.SetMeshVisibility(raceShape, false);
				}
			}
		}
	}

	if (bodyShapeNames.empty())
		return;

	// When NPC is selected but neither the NPC's WNAM nor the race's WNAM defined
	// any TXST overrides, fall through and let the loaded NIF's embedded
	// BSShaderTextureSet (the model author's chosen textures) remain in effect.
	// That IS the in-game behavior and the user-requested fallback — log it so
	// the chain is traceable.
	if (!currentHeadEditorId.empty() && !hasNpcSkinTextures) {
		wxLogMessage("LeveledListPreviewer: NPC '%s' has no WNAM/race TXST override - using NIF embedded textures",
					 currentHeadEditorId);
	}

	// Apply per-slot NPC skin textures (body / hands / feet independently) and
	// the NPC's QNAM tint. Skin textures come from the NPC's WNAM ARMO or (fallback)
	// the race's default skin ARMO, with ARMAs filtered by RNAM to the NPC's race.
	if (hasNpcSkinTextures || hasNpcTint) {
		std::string baseGamePath = Config["GameDataPath"];
		if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
			baseGamePath += '/';

		auto resolveTexFiles = [&](const std::array<std::string, 8>& src) -> std::vector<std::string> {
			const uint8_t MAX_TEX = 10;
			std::vector<std::string> tf(MAX_TEX);
			for (int i = 0; i < 8; ++i) {
				if (src[i].empty())
					continue;
				std::string path = src[i];
				std::replace(path.begin(), path.end(), '\\', '/');
				if (path.find("textures/") == std::string::npos)
					path = "textures/" + path;
				std::string fullPath = baseGamePath + path;
				if (wxFileName::FileExists(fullPath)) {
					tf[i] = fullPath;
				}
				else {
					std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, path);
					tf[i] = resolved.empty() ? fullPath : resolved;
				}
			}
			return tf;
		};

		std::vector<std::string> bodyTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.body) : std::vector<std::string>{};
		std::vector<std::string> handsTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.hands) : std::vector<std::string>{};
		std::vector<std::string> feetTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.feet) : std::vector<std::string>{};

		std::string vShader = Config["AppDir"] + "/res/shaders/default.vert";
		std::string fShader = Config["AppDir"] + "/res/shaders/default.frag";
		TargetGame targetGame = (TargetGame)Config.GetIntValue("TargetGame");
		if (targetGame == FO4 || targetGame == FO4VR || targetGame == FO76) {
			vShader = Config["AppDir"] + "/res/shaders/fo4_default.vert";
			fShader = Config["AppDir"] + "/res/shaders/fo4_default.frag";
		}

		auto applyToShape = [&](const std::string& name, const std::vector<std::string>& texFiles, const char* slotLabel) {
			Mesh* m = gls.GetMesh(name);
			if (!m)
				return;

			// Material override (if we resolved textures for this slot)
			if (!texFiles.empty() && !texFiles[0].empty()) {
				GLMaterial* glMat = gls.AddMaterial(texFiles, vShader, fShader);
				if (glMat) {
					m->material = glMat;
					shapeMaterials[name] = glMat;
					gls.UpdateShaders(m);
					wxLogMessage("LeveledListPreviewer: Applied NPC %s skin texture to '%s': %s", slotLabel, name, texFiles[0]);
				}
			}

			// QNAM tint via the mesh-level subColor uniform (shader does vColor.rgb *= subColor).
			// Unlike vertex colors this doesn't require the NIF's vertex-color flag, so it works
			// on the vanilla femalebody.nif.
			if (hasNpcTint)
				m->color = nifly::Vector3(npcTintR, npcTintG, npcTintB);
		};

		for (auto& [name, partIds] : bodyShapePartMap) {
			if (partIds.count(32))
				applyToShape(name, bodyTex, "body");
			else if (partIds.count(33))
				applyToShape(name, handsTex, "hands");
			else if (partIds.count(37))
				applyToShape(name, feetTex, "feet");
		}
	}

	// Cache game body verts for reset to "(none)".
	for (auto& name : bodyShapeNames) {
		Mesh* m = gls.GetMesh(name);
		if (!m || m->nVerts <= 0)
			continue;
		std::vector<Vector3> verts(m->nVerts);
		for (int i = 0; i < m->nVerts; i++)
			verts[i] = Mesh::TransformPosMeshToNif(m->verts[i]);
		bodyGameVerts[name] = std::move(verts);

		// Pre-load .tri file for this body NIF
		auto srcIt = shapeNifSource.find(name);
		if (srcIt != shapeNifSource.end())
			GetOrLoadTriFile(srcIt->second);
	}

	// Apply current preset if one was chosen
	if (!currentPresetName.empty() && currentPresetName != "(none)")
		ApplyPresetToBody(currentPresetName, true);
}

// Reconcile after outfit pieces are loaded:
// 1. Hide race-loaded body parts whose dismember slot is already represented by
//    an outfit shape (e.g. a cloak NIF that bundles its own hands shape — the
//    cloak ARMO declares slot 14, not 33, so we cannot tell ahead of time, and
//    end up rendering both ChildHands AND the cloak's bundled Hands).
// 2. Apply the resolved NPC/race skin TXST onto outfit shapes that occupy a
//    body slot, so the outfit's "Hands"/"Body"/"Feet" use the NPC's skin tone
//    rather than whatever the model author baked into the NIF.
void LeveledListPreviewer::ReconcileBodyAndOutfitShapes() {
	if (outfitShapeNames.empty())
		return;

	// Set of dismember slot IDs that any outfit shape occupies.
	std::set<uint16_t> outfitCoveredSlots;
	std::set<std::string> outfitShapeSet(outfitShapeNames.begin(), outfitShapeNames.end());
	for (auto& [name, partIds] : bodyShapePartMap) {
		if (!outfitShapeSet.count(name))
			continue;
		for (uint16_t p : partIds) {
			if (p == 32 || p == 33 || p == 37)
				outfitCoveredSlots.insert(p);
		}
	}

	if (!outfitCoveredSlots.empty()) {
		std::string covered;
		for (uint16_t p : outfitCoveredSlots) {
			if (!covered.empty())
				covered += ",";
			covered += std::to_string(p);
		}
		wxLogMessage("ReconcileBodyAndOutfitShapes: outfit covers body slots [%s]", covered);
	}

	// Hide race-loaded body shapes that duplicate outfit-bundled body parts.
	for (auto& raceShape : bodyShapeNames) {
		auto it = bodyShapePartMap.find(raceShape);
		if (it == bodyShapePartMap.end())
			continue;
		bool overlap = false;
		for (uint16_t p : it->second) {
			if (outfitCoveredSlots.count(p)) {
				overlap = true;
				break;
			}
		}
		if (overlap) {
			wxLogMessage("  Hiding race body shape '%s' (covered by outfit)", raceShape);
			gls.SetMeshVisibility(raceShape, false);
		}
	}

	// Apply NPC/race skin TXST (or, when no ESP TXST was found, the textures
	// captured from the race body NIF's BSShaderTextureSet) to outfit body
	// shapes. Note: we do NOT early-exit on `!hasNpcSkinTextures && !hasNpcTint`
	// — the NIF-fallback path further down still needs to run so the outfit's
	// reference body shape picks up the right race texture even when no NPC
	// has been re-selected since the previewer launched (initial load with a
	// previously persisted outfit, in particular).
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	auto resolveTexFiles = [&](const std::array<std::string, 8>& src) -> std::vector<std::string> {
		const uint8_t MAX_TEX = 10;
		std::vector<std::string> tf(MAX_TEX);
		for (int i = 0; i < 8; ++i) {
			if (src[i].empty())
				continue;
			std::string path = src[i];
			std::replace(path.begin(), path.end(), '\\', '/');
			if (path.find("textures/") == std::string::npos)
				path = "textures/" + path;
			std::string fullPath = baseGamePath + path;
			if (wxFileName::FileExists(fullPath)) {
				tf[i] = fullPath;
			}
			else {
				std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, path);
				tf[i] = resolved.empty() ? fullPath : resolved;
			}
		}
		return tf;
	};

	std::vector<std::string> bodyTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.body) : std::vector<std::string>{};
	std::vector<std::string> handsTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.hands) : std::vector<std::string>{};
	std::vector<std::string> feetTex = hasNpcSkinTextures ? resolveTexFiles(npcBodyPartTextures.feet) : std::vector<std::string>{};

	std::string vShader = Config["AppDir"] + "/res/shaders/default.vert";
	std::string fShader = Config["AppDir"] + "/res/shaders/default.frag";
	TargetGame targetGame = (TargetGame)Config.GetIntValue("TargetGame");
	if (targetGame == FO4 || targetGame == FO4VR || targetGame == FO76) {
		vShader = Config["AppDir"] + "/res/shaders/fo4_default.vert";
		fShader = Config["AppDir"] + "/res/shaders/fo4_default.frag";
	}

	auto applyToShape = [&](const std::string& name, const std::vector<std::string>& texFiles, const char* slotLabel) {
		Mesh* m = gls.GetMesh(name);
		if (!m)
			return;
		if (!texFiles.empty() && !texFiles[0].empty()) {
			GLMaterial* glMat = gls.AddMaterial(texFiles, vShader, fShader);
			if (glMat) {
				m->material = glMat;
				shapeMaterials[name] = glMat;
				gls.UpdateShaders(m);
				wxLogMessage("ReconcileBodyAndOutfitShapes: Applied NPC %s skin texture to outfit shape '%s': %s", slotLabel, name, texFiles[0]);
			}
		}
		if (hasNpcTint)
			m->color = nifly::Vector3(npcTintR, npcTintG, npcTintB);
	};

	// Lookup the resolved texture file paths captured from a race body shape's
	// BSShaderTextureSet during LoadNifFromPath/AddNifShapeTextures. We use this
	// as a fallback for outfit body shapes when the ESP texture chain (WNAM
	// ARMA → MO3S → TXST) didn't produce any textures: the race NIF's embedded
	// texture references are the next source of truth (game behavior).
	auto raceShapeTexFor = [&](uint16_t slot) -> const std::vector<std::string>* {
		for (auto& raceShape : bodyShapeNames) {
			auto pit = bodyShapePartMap.find(raceShape);
			if (pit == bodyShapePartMap.end() || !pit->second.count(slot))
				continue;
			auto tit = shapeTexFiles.find(raceShape);
			if (tit != shapeTexFiles.end() && !tit->second.empty() && !tit->second[0].empty())
				return &tit->second;
		}
		return nullptr;
	};

	// Many outfit shapes carry body-slot dismember partitions (32/33/37) just
	// because they're skinned to the body — a dress that morphs with the body
	// will have partition 32 even though it's not a body mesh. To avoid
	// retexturing dresses, sleeves, etc., only override an outfit shape when
	// its current diffuse texture path looks like a vanilla body/hands/feet
	// texture. That singles out the outfit's "reference" body shape (the
	// femalebody substitute baked into the outfit NIF) without touching the
	// clothing parts.
	auto lower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		return s;
	};
	auto isVanillaBodyTex = [&](const std::string& path) {
		std::string p = lower(path);
		return p.find("femalebody") != std::string::npos
			   || p.find("malebody") != std::string::npos;
	};
	auto isVanillaHandsTex = [&](const std::string& path) {
		std::string p = lower(path);
		return p.find("femalehands") != std::string::npos
			   || p.find("malehands") != std::string::npos;
	};
	auto isVanillaFeetTex = [&](const std::string& path) {
		std::string p = lower(path);
		return p.find("femalefeet") != std::string::npos
			   || p.find("malefeet") != std::string::npos;
	};

	for (auto& outfitShape : outfitShapeNames) {
		auto it = bodyShapePartMap.find(outfitShape);
		if (it == bodyShapePartMap.end())
			continue;

		uint16_t slot = 0;
		const std::vector<std::string>* tex = nullptr;
		const char* slotLabel = nullptr;
		if (it->second.count(32)) {
			slot = 32; slotLabel = "body";
			if (!bodyTex.empty() && !bodyTex[0].empty())
				tex = &bodyTex;
		}
		else if (it->second.count(33)) {
			slot = 33; slotLabel = "hands";
			if (!handsTex.empty() && !handsTex[0].empty())
				tex = &handsTex;
		}
		else if (it->second.count(37)) {
			slot = 37; slotLabel = "feet";
			if (!feetTex.empty() && !feetTex[0].empty())
				tex = &feetTex;
		}
		if (slot == 0)
			continue;

		// Verify that the outfit shape is a body part (not a dress/sleeve/etc.)
		// by inspecting its existing diffuse texture path.
		auto curIt = shapeTexFiles.find(outfitShape);
		if (curIt == shapeTexFiles.end() || curIt->second.empty() || curIt->second[0].empty())
			continue;
		const std::string& curDiffuse = curIt->second[0];
		bool looksLikeBodyPart = false;
		switch (slot) {
			case 32: looksLikeBodyPart = isVanillaBodyTex(curDiffuse); break;
			case 33: looksLikeBodyPart = isVanillaHandsTex(curDiffuse); break;
			case 37: looksLikeBodyPart = isVanillaFeetTex(curDiffuse); break;
		}
		if (!looksLikeBodyPart)
			continue;

		// Fall back to the race body NIF's embedded texture set when no
		// ESP-defined TXST was found for this slot.
		if (!tex)
			tex = raceShapeTexFor(slot);
		if (tex)
			applyToShape(outfitShape, *tex, slotLabel);
	}
}

// ---------------------------------------------------------------------------
// NPC head mesh loading
// ---------------------------------------------------------------------------

void LeveledListPreviewer::ClearHeadMeshes() {
	if (canvas && context)
		canvas->SetCurrent(*context);
	for (auto& name : headShapeNames) {
		gls.DeleteMesh(name);
		shapeNifSource.erase(name);
		bodyShapePartMap.erase(name);
		shapeMaterials.erase(name);
	}
	headShapeNames.clear();
}

void LeveledListPreviewer::LoadHeadMesh(const lldata::NPCEntry& npc) {
	ClearHeadMeshes();

	// Ensure skeleton is loaded for bone transforms
	std::string skelPath = FindSkeletonNifPath();
	if (!skelPath.empty()) {
		// Initialize skeleton root name if not set
		if (Config["Anim/SkeletonRootName"].empty()) {
			Config.SetValue("Anim/SkeletonRootName", "NPC Root [Root]");
		}
		AnimSkeleton::getInstance().LoadFromNif(skelPath);
	}

	// FaceGen NIF path: meshes/actors/character/FaceGenData/FaceGeom/{plugin}/{formid:08X}.nif
	// The top byte of a FormID is the plugin's load-order index and is NOT part of
	// the on-disk name — FaceGen files (especially in BSAs) use the FormID with top
	// byte == 0x00 regardless of where the plugin sits in load order. Strip it.
	char formIdBuf[16];
	std::snprintf(formIdBuf, sizeof(formIdBuf), "%08X", npc.formId & 0x00FFFFFFu);
	std::string relativePath = "meshes/actors/character/FaceGenData/FaceGeom/" + npc.plugin + "/" + formIdBuf + ".nif";

	// Load NIF via the same three-way chain LoadNifFromPath uses: loose exact →
	// loose case-insensitive → BSA via FSManager. The BSA path is what matters
	// here — vanilla DLC FaceGen NIFs (Dawnguard / HearthFires / Dragonborn)
	// ship packed inside the DLC BSAs, never as loose files, so the previous
	// nif.Load(resolvedPath) failed silently on them.
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	NifFile nif;
	bool loaded = false;

	std::string fullPath = baseGamePath + relativePath;
	if (wxFileName::FileExists(fullPath))
		loaded = (nif.Load(fullPath) == 0);
	if (!loaded) {
		std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, relativePath);
		if (!resolved.empty())
			loaded = (nif.Load(resolved) == 0);
	}
	if (!loaded) {
		for (FSArchiveFile* archive : FSManager::archiveList()) {
			if (!archive || !archive->hasFile(relativePath)) continue;
			wxMemoryBuffer outData;
			archive->fileContents(relativePath, outData);
			if (outData.IsEmpty()) continue;
			std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
			std::istringstream stream(content, std::istringstream::binary);
			loaded = (nif.Load(stream) == 0);
			break;
		}
	}

	// If the primary (native) path didn't resolve, walk the override chain —
	// every plugin that has an NPC_ record for this NPC is a candidate, and the
	// CK writes FaceGen under the plugin that last modified the face. Iterate
	// in REVERSE scan order (latest first) to approximate "highest priority
	// plugin wins" behavior.
	if (!loaded) {
		for (auto it = npc.overrides.rbegin(); it != npc.overrides.rend() && !loaded; ++it) {
			const std::string& ovPlugin = it->first;
			uint32_t ovFormId = it->second;
			if (ovPlugin == npc.plugin && ovFormId == npc.formId)
				continue; // already tried above
			char ovBuf[16];
			std::snprintf(ovBuf, sizeof(ovBuf), "%08X", ovFormId & 0x00FFFFFFu);
			std::string ovRelPath = "meshes/actors/character/FaceGenData/FaceGeom/" + ovPlugin + "/" + ovBuf + ".nif";

			std::string ovFull = baseGamePath + ovRelPath;
			if (wxFileName::FileExists(ovFull) && nif.Load(ovFull) == 0) {
				loaded = true;
				wxLogMessage("LeveledListPreviewer: FaceGen via override (loose): %s", ovRelPath);
				break;
			}
			std::string ovResolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, ovRelPath);
			if (!ovResolved.empty() && nif.Load(ovResolved) == 0) {
				loaded = true;
				wxLogMessage("LeveledListPreviewer: FaceGen via override (loose CI): %s", ovRelPath);
				break;
			}
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (!archive || !archive->hasFile(ovRelPath)) continue;
				wxMemoryBuffer outData;
				archive->fileContents(ovRelPath, outData);
				if (outData.IsEmpty()) continue;
				std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
				std::istringstream stream(content, std::istringstream::binary);
				if (nif.Load(stream) == 0) {
					loaded = true;
					wxLogMessage("LeveledListPreviewer: FaceGen via override (BSA): %s", ovRelPath);
					break;
				}
			}
		}
	}

	if (!loaded) {
		wxLogWarning("LeveledListPreviewer: FaceGen NIF not found (native path + %zu overrides): %s", npc.overrides.size(), relativePath);
		return;
	}

	// Position correction for head shapes. We're not running a skinning shader,
	// so AddMeshFromNif positions each shape using its stored xformSkinToGlobal,
	// which is inconsistent across shapes within a FaceGen NIF: the face shape
	// has verts pre-baked at head height with skinToGlobal=identity, while the
	// mouth/teeth shape has verts at bone-local origin with skinToGlobal=identity
	// too — the latter renders on the ground.
	//
	// The correct per-vertex position in world space is:
	//     final = char_headBone_global * xformSkinToBone * vert_nif
	// which in mesh-space coordinates is:
	//     xformMeshToModel = xformNifToMesh * char_head_global * xformSkinToBone * xformMeshToNif
	//
	// This replaces AddMeshFromNif's positioning entirely. It uses xformSkinToBone
	// from each shape's own skin data, so the face's translation(0,0,-120) maps
	// face verts to bone-local origin and then char_head_global lifts them back to
	// head height (net identity), while the mouth's identity skin-to-bone leaves
	// verts at origin and char_head_global translates them to the head.
	nifly::MatTransform charHeadBoneGlobal;
	bool haveCharHeadBone = AnimSkeleton::getInstance().GetBoneTransformToGlobal("NPC Head [Head]", charHeadBoneGlobal)
						 || AnimSkeleton::getInstance().GetBoneTransformToGlobal("NPC Head", charHeadBoneGlobal);
	if (!haveCharHeadBone) {
		wxLogError("LeveledListPreviewer: NPC Head bone not in skeleton; head shape positioning will be incorrect");
	}

	auto applyHeadCorrection = [&](Mesh* m, NifFile& sourceNif, const std::string& shapeName) {
		if (!m || !haveCharHeadBone)
			return;
		auto* shape = sourceNif.FindBlockByName<NiShape>(shapeName);
		if (!shape || !shape->IsSkinned())
			return;

		nifly::MatTransform skinToBone;
		if (!sourceNif.GetShapeTransformSkinToBone(shape, "NPC Head [Head]", skinToBone))
			if (!sourceNif.GetShapeTransformSkinToBone(shape, "NPC Head", skinToBone))
				return;  // shape isn't weighted to NPC Head — leave AddMeshFromNif's positioning

		nifly::MatTransform targetNif = charHeadBoneGlobal.ComposeTransforms(skinToBone);
		nifly::MatTransform targetMesh = Mesh::xformNifToMesh.ComposeTransforms(targetNif.ComposeTransforms(Mesh::xformMeshToNif));
		m->SetXformMeshToModel(targetMesh);
		wxLogMessage("LeveledListPreviewer: head correction for '%s': skinToBone=(%.2f,%.2f,%.2f) charHead=(%.2f,%.2f,%.2f)",
					 shapeName,
					 skinToBone.translation.x, skinToBone.translation.y, skinToBone.translation.z,
					 charHeadBoneGlobal.translation.x, charHeadBoneGlobal.translation.y, charHeadBoneGlobal.translation.z);
	};

	for (auto& shapeName : nif.GetShapeNames()) {
		std::string meshName = "_head_" + shapeName;
		Mesh* m = gls.AddMeshFromNif(&nif, shapeName, meshName, nullptr, false);
		if (!m)
			continue;

		applyHeadCorrection(m, nif, shapeName);

		// Targeted opacity fix: only for the face shape. Hair, eyelashes and brows RELY on
		// per-vertex alpha + alpha-blending to fade their edges; zeroing those flags turns
		// them into solid planes (the "artifact" the user saw).
		//
		// The invisible-face bug is specific to FaceGen NIFs where the face shape has
		// vertexAlpha=true but the vertex data includes zero-alpha verts (used in-engine as
		// a tint-layer blend mask, not opacity). Our shader treats that alpha as opacity and
		// discards via alphaThreshold → invisible face.
		//
		// Heuristic for "this is the face shape": name contains "head", "face" or "basehead"
		// (nifly shape naming convention for FaceGen), case-insensitive.
		auto toLower = [](std::string s) { for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c)); return s; };
		std::string lowerShape = toLower(shapeName);
		bool isFaceShape = lowerShape.find("head") != std::string::npos
						   || lowerShape.find("face") != std::string::npos
						   || lowerShape.find("basehead") != std::string::npos;

		if (isFaceShape) {
			// Force valpha buffer to 1.0 and disable the shape's vertex-alpha flag so the
			// vertex shader's default vColor.a = 1.0 wins. Leaves alphaFlags untouched so
			// other shape flags (e.g. model-space normals) stay correct.
			if (m->valpha) {
				for (int v = 0; v < m->nVerts; v++)
					m->valpha[v] = 1.0f;
			}
			m->vertexAlpha = false;
		}

		wxLogMessage("LoadHeadMesh: shape='%s' face=%s nVerts=%d vcolorFlag=%s valphaFlag=%s alphaFlags=%u alphaThr=%u",
					 shapeName,
					 isFaceShape ? "yes" : "no",
					 m->nVerts,
					 m->vertexColors ? "on" : "off",
					 m->vertexAlpha ? "on" : "off",
					 (unsigned)m->alphaFlags,
					 (unsigned)m->alphaThreshold);

		// QNAM tint via mesh subColor uniform (shader: vColor.rgb *= subColor).
		if (hasNpcTint)
			m->color = nifly::Vector3(npcTintR, npcTintG, npcTintB);

		m->CreateBuffers();
		AddNifShapeTextures(&nif, shapeName, nullptr, meshName);
		headShapeNames.push_back(meshName);
		shapeNifSource[meshName] = relativePath;

		// Extract partition body part IDs from NIF dismember skin
		auto* shape = nif.FindBlockByName<NiShape>(shapeName);
		if (shape) {
			NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
			std::vector<int> triParts;
			bool gotParts = nif.GetShapePartitions(shape, partInfo, triParts);
			
			// Fallback: if GetShapePartitions fails (e.g. no NiSkinPartition), 
			// try to get slot info directly from BSDismemberSkinInstance if it exists.
			if (!gotParts || partInfo.empty()) {
				auto* bsdSkin = nif.GetHeader().GetBlock<BSDismemberSkinInstance>(shape->SkinInstanceRef());
				if (bsdSkin) {
					partInfo = bsdSkin->partitions;
					gotParts = true;
				}
			}

			if (gotParts && !partInfo.empty()) {
				std::set<uint16_t> partIds;
				for (size_t i = 0; i < partInfo.size(); ++i)
					partIds.insert(partInfo[i].partID);
				bodyShapePartMap[meshName] = std::move(partIds);
			}
		}
	}

	UpdateHeadVisibility();

	// Load additional head parts (eyes, hair, etc.) if they have separate NIFs.
	for (uint32_t hpFid : npc.headParts) {
		std::string hpRelPath = data.ResolveHeadPartNif(hpFid);
		if (hpRelPath.empty())
			continue;

		// Skip paths that are already loaded as part of the main FaceGen NIF
		// (rare but possible if the cache is messy).
		bool alreadyLoaded = false;
		auto toLower = [](std::string s) { for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c)); return s; };
		for (auto const& [meshName, source] : shapeNifSource) {
			if (toLower(source) == toLower(hpRelPath)) {
				alreadyLoaded = true;
				break;
			}
		}
		if (alreadyLoaded)
			continue;

		NifFile hpNif;
		std::string hpFullPath = baseGamePath + hpRelPath;
		bool hpLoaded = false;
		if (wxFileName::FileExists(hpFullPath))
			hpLoaded = (hpNif.Load(hpFullPath) == 0);
		if (!hpLoaded) {
			std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, hpRelPath);
			if (!resolved.empty())
				hpLoaded = (hpNif.Load(resolved) == 0);
		}
		if (!hpLoaded) {
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (!archive || !archive->hasFile(hpRelPath)) continue;
				wxMemoryBuffer outData;
				archive->fileContents(hpRelPath, outData);
				if (outData.IsEmpty()) continue;
				std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
				std::istringstream stream(content, std::istringstream::binary);
				hpLoaded = (hpNif.Load(stream) == 0);
				break;
			}
		}

		if (!hpLoaded) {
			wxLogWarning("LeveledListPreviewer: HeadPart NIF not found: %s", hpRelPath);
			continue;
		}

		for (auto& shapeName : hpNif.GetShapeNames()) {
			std::string meshName = "_head_" + shapeName;
			// Avoid name collisions if multiple NIFs have the same shape name
			int suffix = 1;
			while (std::find(headShapeNames.begin(), headShapeNames.end(), meshName) != headShapeNames.end()) {
				meshName = "_head_" + shapeName + "_" + std::to_string(suffix++);
			}

			Mesh* m = gls.AddMeshFromNif(&hpNif, shapeName, meshName, nullptr, false);
			if (!m)
				continue;

			applyHeadCorrection(m, hpNif, shapeName);

			m->CreateBuffers();
			AddNifShapeTextures(&hpNif, shapeName, nullptr, meshName);
			headShapeNames.push_back(meshName);
			shapeNifSource[meshName] = hpRelPath;
		}
	}

	UpdateHeadVisibility();

	wxLogMessage("LeveledListPreviewer: Loaded head NIF + parts for NPC '%s' (%s): %zu shapes", npc.editorId, formIdBuf, headShapeNames.size());
	gls.RenderOneFrame();
}

void LeveledListPreviewer::OnHeadEntered(wxCommandEvent& WXUNUSED(event)) {
	if (!headCtrl)
		return;

	std::string text = headCtrl->GetValue().ToStdString();
	if (text.empty()) {
		ClearHeadMeshes();
		currentHeadEditorId.clear();
		hasNpcSkinTextures = false;
		npcBodyPartTextures = {};
		hasNpcTint = false;
		npcTintR = npcTintG = npcTintB = 1.0f;
		// Reload default body meshes
		if (showBody) {
			if (canvas && context)
				canvas->SetCurrent(*context);
			for (auto& name : bodyShapeNames)
				gls.DeleteMesh(name);
			bodyShapeNames.clear();
			bodyGameVerts.clear();
			LoadBodyMeshes();
		}
		RefreshMeshOverlay();
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

	// Show the display name (full in-game name) in the text box, not the editor ID.
	if (headCtrl && !npc.displayName.empty() && headCtrl->GetValue().ToStdString() != npc.displayName)
		headCtrl->ChangeValue(wxString::FromUTF8(npc.displayName));

	const std::string& logName = !npc.fullName.empty() ? npc.fullName : npc.editorId;

	// Resolve per-slot NPC skin textures (body / hands / feet independently).
	npcBodyPartTextures = data.ResolveNpcBodyPartTextures(npc.editorId);
	hasNpcSkinTextures = !npcBodyPartTextures.body[0].empty()
						 || !npcBodyPartTextures.hands[0].empty()
						 || !npcBodyPartTextures.feet[0].empty();

	if (hasNpcSkinTextures) {
		std::string skinLog = "LeveledListPreviewer: NPC '" + logName
							  + "' skin resolved — body='" + npcBodyPartTextures.body[0]
							  + "' hands='" + npcBodyPartTextures.hands[0]
							  + "' feet='" + npcBodyPartTextures.feet[0] + "'";
		wxLogMessage("%s", skinLog.c_str());
	}
	else {
		wxLogMessage("LeveledListPreviewer: NPC '%s' no skin texture overrides resolved (will use NIF embedded textures)", logName.c_str());
	}

	// QNAM face tint (skin complexion).
	hasNpcTint = npc.hasTint;
	if (hasNpcTint) {
		npcTintR = npc.tintR / 255.0f;
		npcTintG = npc.tintG / 255.0f;
		npcTintB = npc.tintB / 255.0f;
		wxLogMessage("LeveledListPreviewer: NPC '%s' tint RGB=(%u,%u,%u)", logName, npc.tintR, npc.tintG, npc.tintB);
	}

	// Always reload body meshes when NPC changes — the body mesh paths themselves
	// may differ (e.g. a custom-race NPC uses a different femalebody NIF).
	if (showBody) {
		if (canvas && context)
			canvas->SetCurrent(*context);
		for (auto& name : bodyShapeNames)
			gls.DeleteMesh(name);
		bodyShapeNames.clear();
		bodyGameVerts.clear();
		LoadBodyMeshes();
		// If an outfit is currently displayed, reapply race skin TXST (or NIF
		// embedded textures from the new race body NIF) onto its bundled body
		// shapes. Otherwise the outfit keeps the previous NPC's textures.
		ReconcileBodyAndOutfitShapes();
	}

	RefreshMeshOverlay();
}

// ---------------------------------------------------------------------------
// BodySlide preset loading and application
// ---------------------------------------------------------------------------

void LeveledListPreviewer::UpdateHeadVisibility() {
	if (headShapeNames.empty())
		return;

	// According to the user, ONLY slot 31 is the hair slot.
	bool hideHair = outfitDeclaredSlots.count(31) != 0;

	for (const auto& meshName : headShapeNames) {
		Mesh* m = gls.GetMesh(meshName);
		if (!m) continue;

		bool visible = true;
		auto it = bodyShapePartMap.find(meshName);
		if (it != bodyShapePartMap.end()) {
			// ONLY hide if it's a hair slot (31) AND the outfit includes it (31).
			if (it->second.count(31)) {
				if (hideHair) {
					visible = false;
				}
			}
		}

		if (m->bVisible != visible) {
			m->bVisible = visible;
		}
	}
}

void LeveledListPreviewer::UpdateActiveSlotsAndVisibility() {
	if (!currentOutfit)
		return;

	outfitDeclaredSlots.clear();
	for (auto& piece : currentOutfit->pieces) {
		if (piece.useAnyGroup >= 0) {
			if (useAnyGroups_.empty()) {
				// Initial load: no groups built yet, assume variant 0
				if (piece.useAnyVariant != 0)
					continue;
			}
			else {
				// Switching variants: use the current built group state
				bool isActive = false;
				for (auto& g : useAnyGroups_) {
					if (g.groupId == piece.useAnyGroup) {
						// Active variant is indexed by g.activeIndex
						if (g.activeIndex >= 0 && g.activeIndex < static_cast<int>(g.variants.size())) {
							if (g.variants[g.activeIndex].variantIdx == piece.useAnyVariant) {
								isActive = true;
								break;
							}
						}
					}
				}
				if (!isActive)
					continue;
			}
		}
		for (int slot : piece.bodySlots)
			outfitDeclaredSlots.insert(slot);
	}

	UpdateHeadVisibility();
}

void LeveledListPreviewer::LoadPresetList() {
	if (!presetCombo)
		return;

	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty())
		projectPath = Config["AppDir"];
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

void LeveledListPreviewer::ApplyPresetToBody(const std::string& presetName, bool deferredRender) {
	if (bodyGameVerts.empty()) {
		wxLogMessage("ApplyPresetToBody: no body verts cached");
		return;
	}

	if (canvas && context)
		canvas->SetCurrent(*context);

	// "(none)" or empty → reset to the game body shape (as-built)
	if (presetName.empty() || presetName == "(none)") {
		for (auto& [shapeName, verts] : bodyGameVerts)
			gls.Update(shapeName, &verts, nullptr);
		if (!deferredRender)
			gls.RenderOneFrame();
		return;
	}

	// Load all presets
	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty())
		projectPath = Config["AppDir"];

	PresetCollection presets;
	std::vector<std::string> noGroups;
	presets.LoadPresets(projectPath + "/SliderPresets", "", noGroups, true);

	wxLogMessage("ApplyPresetToBody: preset='%s', shapes=%zu", presetName, bodyGameVerts.size());

	for (auto& [shapeName, gameVerts] : bodyGameVerts) {
		auto srcIt = shapeNifSource.find(shapeName);
		if (srcIt == shapeNifSource.end())
			continue;

		TriFile* tri = GetOrLoadTriFile(srcIt->second);
		if (!tri)
			continue;
		if (!gls.GetMesh(shapeName))
			continue;

		// Body shapes use NIF shape name directly (no prefix)
		std::vector<Vector3> verts = gameVerts;

		int applied = 0;
		auto allMorphs = tri->GetMorphs();
		auto morphIt = allMorphs.find(shapeName);
		if (morphIt == allMorphs.end())
			continue;

		for (auto& morph : morphIt->second) {
			if (morph->type != MORPHTYPE_POSITION)
				continue;

			float val = 0.0f;
			bool found = useHighWeight ? presets.GetBigPreset(presetName, morph->name, val) : presets.GetSmallPreset(presetName, morph->name, val);
			if (!found || val == 0.0f)
				continue;

			for (auto& [idx, delta] : morph->offsets) {
				if (idx < verts.size())
					verts[idx] += delta * val;
			}
			++applied;
		}

		wxLogMessage("  body shape '%s': %d morphs applied", shapeName, applied);
		gls.Update(shapeName, &verts, nullptr);
	}

	if (!deferredRender)
		gls.RenderOneFrame();
	wxLog::FlushActive();
}

void LeveledListPreviewer::ApplyPresetToOutfit(const std::string& presetName, bool deferredRender) {
	if (outfitGameVerts.empty())
		return;

	if (canvas && context)
		canvas->SetCurrent(*context);

	// "(none)" or empty → reset to game verts
	if (presetName.empty() || presetName == "(none)") {
		for (auto& [shapeName, verts] : outfitGameVerts)
			gls.Update(shapeName, &verts, nullptr);
		if (!deferredRender)
			gls.RenderOneFrame();
		return;
	}

	// Load all presets
	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty())
		projectPath = Config["AppDir"];

	PresetCollection presets;
	std::vector<std::string> noGroups;
	presets.LoadPresets(projectPath + "/SliderPresets", "", noGroups, true);

	wxLogMessage("ApplyPresetToOutfit: preset='%s', shapes=%zu", presetName, outfitGameVerts.size());

	for (auto& [displayName, gameVerts] : outfitGameVerts) {
		auto srcIt = shapeNifSource.find(displayName);
		if (srcIt == shapeNifSource.end())
			continue;

		TriFile* tri = GetOrLoadTriFile(srcIt->second);
		if (!tri)
			continue;
		if (!gls.GetMesh(displayName))
			continue;

		// Outfit shapes have a formId prefix (XXXXXXXX_) — strip it to get the NIF shape name
		auto nameIt = outfitShapeNifName_.find(displayName);
		if (nameIt == outfitShapeNifName_.end())
			continue;
		const std::string& nifShapeName = nameIt->second;

		std::vector<Vector3> verts = gameVerts;

		int applied = 0;
		auto allMorphs = tri->GetMorphs();
		auto morphIt = allMorphs.find(nifShapeName);
		if (morphIt == allMorphs.end())
			continue;

		for (auto& morph : morphIt->second) {
			if (morph->type != MORPHTYPE_POSITION)
				continue;

			float val = 0.0f;
			bool found = useHighWeight ? presets.GetBigPreset(presetName, morph->name, val) : presets.GetSmallPreset(presetName, morph->name, val);
			if (!found || val == 0.0f)
				continue;

			for (auto& [idx, delta] : morph->offsets) {
				if (idx < verts.size())
					verts[idx] += delta * val;
			}
			++applied;
		}

		wxLogMessage("  outfit shape '%s' (nif '%s'): %d morphs applied", displayName, nifShapeName, applied);
		gls.Update(displayName, &verts, nullptr);
	}

	if (!deferredRender)
		gls.RenderOneFrame();
	wxLog::FlushActive();
}

void LeveledListPreviewer::OnPresetChanged(wxCommandEvent& WXUNUSED(event)) {
	if (!presetCombo)
		return;
	currentPresetName = presetCombo->GetValue().ToStdString();
	ApplyPresetToBody(currentPresetName);
	ApplyPresetToOutfit(currentPresetName);

	// If SMP is running, feed the newly morphed mesh vertices back to the simulator
	if (smpSimulator_) {
		for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
			Mesh* mesh = gls.GetMesh(name);
			if (!mesh || mesh->nVerts <= 0)
				continue;
			std::vector<Vector3> nifVerts(mesh->nVerts);
			for (int i = 0; i < mesh->nVerts; i++)
				nifVerts[i] = Mesh::TransformPosMeshToNif(mesh->verts[i]);
			smpSimulator_->UpdateSkinPositions(name, nifVerts);
		}
	}
}

void LeveledListPreviewer::OnHighWeightChanged(wxCommandEvent& event) {
	useHighWeight = event.IsChecked();

	if (!showBody)
		return;

	if (canvas && context)
		canvas->SetCurrent(*context);

	// Optimization: if a preset is active and .tri files are loaded,
	// we can just re-apply presets with the new weight values without
	// reloading body NIFs from disk.
	bool presetActive = !currentPresetName.empty() && currentPresetName != "(none)";
	if (presetActive && !bodyGameVerts.empty()) {
		ApplyPresetToBody(currentPresetName);
		ApplyPresetToOutfit(currentPresetName);
		// If SMP is running, sync morphed verts
		if (smpSimulator_) {
			for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
				Mesh* mesh = gls.GetMesh(name);
				if (!mesh || mesh->nVerts <= 0)
					continue;
				std::vector<Vector3> nifVerts(mesh->nVerts);
				for (int i = 0; i < mesh->nVerts; i++)
					nifVerts[i] = Mesh::TransformPosMeshToNif(mesh->verts[i]);
				smpSimulator_->UpdateSkinPositions(name, nifVerts);
			}
		}
		// Mesh set unchanged — just sync checkbox states
		SyncMeshOverlayStates();
		gls.RenderOneFrame();
		return;
	}

	// No preset active (or no morph maps) — full reload of body and outfit needed
	// for correct weight variant NIFs.
	{
		long sel = outfitList ? outfitList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
		if (sel >= 0 && sel < static_cast<long>(filteredOutfits.size())) {
			// Reload entire outfit (which also reloads body); RefreshMeshOverlay called inside
			LoadOutfitMeshes(*filteredOutfits[sel]);
			gls.RenderOneFrame();
			return;
		}
	}

	// Fallback: no outfit selected, just reload body
	for (auto& name : bodyShapeNames)
		gls.DeleteMesh(name);
	bodyShapeNames.clear();
	bodyGameVerts.clear();

	LoadBodyMeshes();

	RefreshMeshOverlay();
	gls.RenderOneFrame();
}

// ---------------------------------------------------------------------------
// Outfit selection → 3D preview
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnOutfitSelected(wxListEvent& event) {
	long sel = event.GetIndex();
	if (sel < 0 || sel >= static_cast<long>(filteredOutfits.size()))
		return;

	// Persist for reloads/filtering
	Config.SetValue("LLPreviewer/LastOutfit", filteredOutfits[sel]->name);

	LoadOutfitMeshes(*filteredOutfits[sel]);
}

void LeveledListPreviewer::LoadOutfitMeshes(const lldata::OutfitEntry& outfit) {
	currentOutfit = &outfit;
	// Stop any running SMP simulation
	StopSmpSimulation();
	smpXmlPaths_.clear();
	if (smpToggle_)
		smpToggle_->SetValue(false);

	if (canvas && context)
		canvas->SetCurrent(*context);

	// Selectively clear outfit and body meshes, keeping head meshes
	// Clean up tracking for all deleted shapes (non-head)
	{
		std::set<std::string> headSet(headShapeNames.begin(), headShapeNames.end());
		auto isNotHead = [&](const std::string& name) { return headSet.find(name) == headSet.end(); };

		for (auto& name : outfitShapeNames)
			gls.DeleteMesh(name);
		for (auto& name : bodyShapeNames)
			gls.DeleteMesh(name);

		for (auto it = shapeNifSource.begin(); it != shapeNifSource.end();) {
			if (isNotHead(it->first))
				it = shapeNifSource.erase(it);
			else
				++it;
		}
		for (auto it = bodyShapePartMap.begin(); it != bodyShapePartMap.end();) {
			if (isNotHead(it->first))
				it = bodyShapePartMap.erase(it);
			else
				++it;
		}
		for (auto it = bodyGameVerts.begin(); it != bodyGameVerts.end();) {
			if (isNotHead(it->first))
				it = bodyGameVerts.erase(it);
			else
				++it;
		}
		for (auto it = shapeMaterials.begin(); it != shapeMaterials.end();) {
			if (isNotHead(it->first))
				it = shapeMaterials.erase(it);
			else
				++it;
		}
		for (auto it = shapeArmoName.begin(); it != shapeArmoName.end();) {
			if (isNotHead(it->first))
				it = shapeArmoName.erase(it);
			else
				++it;
		}

		untexturedShapes.erase(std::remove_if(untexturedShapes.begin(), untexturedShapes.end(), isNotHead), untexturedShapes.end());

		bodyShapeNames.clear();
		outfitShapeNames.clear();
		outfitGameVerts.clear();
		outfitShapeNifName_.clear();
		useAnyGroups_.clear();
		shapeToVariantGroup_.clear();
	}

	gls.RenderOneFrame();

	// Population algorithm is now identical for initial load and switches.
	UpdateActiveSlotsAndVisibility();

	// Load default body/hands/feet for slots NOT declared by the outfit ARMO.
	if (showBody)
		LoadBodyMeshes();

	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	int loadedCount = 0;

	// Track shapes per piece for variant group building
	struct PieceShapeInfo {
		int useAnyGroup;
		int useAnyVariant;
		std::string pieceName;
		std::vector<std::string> shapeNames;
	};
	std::vector<PieceShapeInfo> pieceInfos;

	for (auto& piece : outfit.pieces) {
		if (piece.nifPath.empty()) {
			wxLogMessage("  Piece '%s' [%08X]: no model path, skipping", piece.name, piece.formId);
			continue;
		}

		wxLogMessage("  Piece '%s' [%08X]: nifPath=%s (group=%d, variant=%d)", piece.name, piece.formId, piece.nifPath, piece.useAnyGroup, piece.useAnyVariant);

		// When in low weight mode, try the _0.nif variant of the piece
		std::string piecePath = piece.nifPath;
		if (!useHighWeight && piecePath.size() > 6) {
			std::string ending = piecePath.substr(piecePath.size() - 6);
			std::string endingLower = ending;
			std::transform(endingLower.begin(), endingLower.end(), endingLower.begin(), ::tolower);
			if (endingLower == "_1.nif") {
				std::string lowPath = piecePath.substr(0, piecePath.size() - 6) + "_0.nif";
				std::string resolvedLow = data.ResolveNifPath(lowPath);
				if (!resolvedLow.empty()) {
					piecePath = lowPath;
					wxLogMessage("  Using low weight variant: %s", piecePath);
				}
			}
		}

		std::string resolvedPath = data.ResolveNifPath(piecePath);
		if (resolvedPath.empty()) {
			wxLogWarning("  NIF not found: %s", piecePath);
			continue;
		}

		wxLogMessage("  Resolved to: %s", resolvedPath);

		// Load NIF file
		NifFile nif;
		std::string fullPath = resolvedPath;

		// If resolvedPath is relative (from archive), try loading from archive
		if (resolvedPath == piecePath) {
			// Try loading from BSA/BA2
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (archive && archive->hasFile(piecePath)) {
					wxMemoryBuffer outData;
					archive->fileContents(piecePath, outData);
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
		// Extract SMP XML path from NIF extra data
		{
			std::string xmlPath = FindSmpXml(nif, piece.nifPath);
			if (!xmlPath.empty()) {
				smpXmlPaths_[piecePath] = xmlPath;
				wxLogMessage("  SMP XML found for '%s': %s", piecePath, xmlPath);
			}
		}

		// Add all shapes from NIF.
		// Prefix each shape name with the piece's FormID and an index to avoid
		// collisions when one ARMO has multiple pieces/NIFs (ARMAs) or when
		// multiple pieces share shape names.
		char piecePrefixBuf[32];
		std::snprintf(piecePrefixBuf, sizeof(piecePrefixBuf), "%08X_%d_", piece.formId, loadedCount);
		std::string piecePrefix = piecePrefixBuf;

		// Try to pre-load .tri file for this piece NIF
		TriFile* pieceTri = GetOrLoadTriFile(piecePath);

		PieceShapeInfo psi;
		psi.useAnyGroup = piece.useAnyGroup;
		psi.useAnyVariant = piece.useAnyVariant;
		psi.pieceName = piece.name;

		for (auto& shapeName : nif.GetShapeNames()) {
			std::string displayName = piecePrefix + shapeName;
			Mesh* m = gls.AddMeshFromNif(&nif, shapeName, displayName, nullptr, false);
			if (!m)
				continue;

			outfitShapeNifName_[m->shapeName] = shapeName;

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
			const std::vector<lldata::TextureOverride>* overrides = piece.textureOverrides.empty() ? nullptr : &piece.textureOverrides;
			// Pass original shapeName for texture override matching (overrides use NIF shape names)
			bool hasTexture = AddNifShapeTextures(&nif, shapeName, overrides, m->shapeName);
			if (!hasTexture) {
				untexturedShapes.push_back(m->shapeName);
				if (!showUntextured) {
					wxLogWarning("  HIDING shape '%s' (no diffuse)", wxString(m->shapeName));
					gls.SetMeshVisibility(m->shapeName, false);
				}
			}
			if (std::find(outfitShapeNames.begin(), outfitShapeNames.end(), m->shapeName) == outfitShapeNames.end())
				outfitShapeNames.push_back(m->shapeName);
			shapeNifSource[m->shapeName] = piecePath;
			shapeArmoName[m->shapeName] = piece.name;
			psi.shapeNames.push_back(m->shapeName);
			++loadedCount;

			// Game-accurate filter: a NIF shape only renders if its dismember
			// partition matches one of the slots declared by the owning ARMO.
			// Cloak NIFs commonly bundle a copy of the body / hands / feet
			// shapes for rigging reference even though the cloak's ARMO only
			// declares the cloak slot — the engine skips those bundled shapes
			// and renders only the parts whose partition the ARMO actually
			// claims. We mirror that here: shapes whose partition isn't
			// claimed by their piece's ARMO are hidden on load.
			if (auto* nifShape = nif.FindBlockByName<NiShape>(shapeName)) {
				NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
				std::vector<int> triParts;
				if (nif.GetShapePartitions(nifShape, partInfo, triParts)) {
					std::set<uint16_t> partIds;
					std::string partList;
					for (size_t i = 0; i < partInfo.size(); ++i) {
						partIds.insert(partInfo[i].partID);
						if (!partList.empty()) partList += ",";
						partList += std::to_string(partInfo[i].partID);
					}
					if (!partIds.empty()) {
						bodyShapePartMap[m->shapeName] = partIds;

						bool slotCoveredByPiece = false;
						if (!piece.bodySlots.empty()) {
							for (int slot : piece.bodySlots) {
								if (partIds.count(static_cast<uint16_t>(slot))) {
									slotCoveredByPiece = true;
									break;
								}
							}
						}
						if (!piece.bodySlots.empty() && !slotCoveredByPiece) {
							std::string declared;
							for (int slot : piece.bodySlots) {
								if (!declared.empty()) declared += ",";
								declared += std::to_string(slot);
							}
							wxLogMessage("  Hiding outfit shape '%s' (partitions [%s] not in piece slots [%s])",
										 m->shapeName, partList, declared);
							gls.SetMeshVisibility(m->shapeName, false);
						}
						else {
							wxLogMessage("  Outfit shape '%s' partitions: [%s]", m->shapeName, partList);
						}
					}
				}
			}

			// Cache game verts for reset and .tri morphing
			if (pieceTri && m->nVerts > 0) {
				std::vector<Vector3> gameVerts(m->nVerts);
				for (int i = 0; i < m->nVerts; i++)
					gameVerts[i] = Mesh::TransformPosMeshToNif(m->verts[i]);
				outfitGameVerts[m->shapeName] = std::move(gameVerts);
				wxLogMessage("  Outfit morph: '%s' has .tri (nif shape '%s', %d verts)", m->shapeName, shapeName, m->nVerts);
			}
		}

		if (!psi.shapeNames.empty())
			pieceInfos.push_back(std::move(psi));
	}


	// Build "Use Any" variant groups and set initial visibility.
	// Group pieces by useAnyGroup, then create UseAnyGroup entries with variants.
	{
		std::map<int, std::map<int, std::vector<PieceShapeInfo*>>> groupMap;
		for (auto& psi : pieceInfos) {
			if (psi.useAnyGroup >= 0)
				groupMap[psi.useAnyGroup][psi.useAnyVariant].push_back(&psi);
		}

		for (auto& [groupId, variantMap] : groupMap) {
			UseAnyGroup group;
			group.groupId = groupId;
			group.activeIndex = 0;

			for (auto& [variantIdx, pieces] : variantMap) {
				UseAnyVariant variant;
				variant.variantIdx = variantIdx;
				// Build label from piece names (use first piece's name, or combine)
				if (!pieces.empty())
					variant.label = pieces[0]->pieceName;
				if (pieces.size() > 1)
					variant.label += " (+" + std::to_string(pieces.size() - 1) + ")";

				for (auto* p : pieces) {
					for (auto& sn : p->shapeNames)
						variant.shapeNames.push_back(sn);
				}
				group.variants.push_back(std::move(variant));
			}

			// Register shape → group mapping and set visibility
			size_t groupIdx = useAnyGroups_.size();
			for (size_t vi = 0; vi < group.variants.size(); ++vi) {
				bool isActive = (static_cast<int>(vi) == group.activeIndex);
				for (auto& sn : group.variants[vi].shapeNames) {
					shapeToVariantGroup_[sn] = {groupIdx, vi};
					if (!isActive)
						gls.SetMeshVisibility(sn, false);
				}
			}

			wxLogMessage("  Use Any group %d: %zu variants, showing variant 0 ('%s')", groupId, group.variants.size(), group.variants.empty() ? "" : group.variants[0].label);

			useAnyGroups_.push_back(std::move(group));
		}
	}

	if (smpToggle_) {
		smpToggle_->Enable(!smpXmlPaths_.empty());

		// Restore sticky SMP toggle: auto-start if previously enabled
		if (!smpXmlPaths_.empty() && Config["SmpEnabled"] == "true") {
			smpToggle_->SetValue(true);
			wxCommandEvent smpEvt(wxEVT_CHECKBOX);
			smpEvt.SetInt(1);
			OnToggleSmp(smpEvt);
		}
	}

	// Apply preset to outfit pieces
	if (!outfitGameVerts.empty() && !currentPresetName.empty() && currentPresetName != "(none)")
		ApplyPresetToOutfit(currentPresetName, true);

	// If SMP is running, sync the morphed mesh vertices to the simulator.
	// SMP auto-restore above captured unmorphed verts; now that the preset has
	// been applied we need to update skin positions so SkinVertices preserves
	// the preset shape.
	if (smpSimulator_) {
		for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
			Mesh* mesh = gls.GetMesh(name);
			if (!mesh || mesh->nVerts <= 0)
				continue;
			std::vector<Vector3> nifVerts(mesh->nVerts);
			for (int i = 0; i < mesh->nVerts; i++)
				nifVerts[i] = Mesh::TransformPosMeshToNif(mesh->verts[i]);
			smpSimulator_->UpdateSkinPositions(name, nifVerts);
		}
	}

	// Head mesh is preserved across outfit changes — only reload if not yet loaded
	if (headShapeNames.empty() && !currentHeadEditorId.empty()) {
		auto& npcs = data.GetNPCs();
		for (auto& npc : npcs) {
			if (npc.editorId == currentHeadEditorId) {
				LoadHeadMesh(npc);

				// Resolve per-slot NPC skin + QNAM tint if not yet done
				if (!hasNpcSkinTextures && !hasNpcTint) {
					npcBodyPartTextures = data.ResolveNpcBodyPartTextures(npc.editorId);
					hasNpcSkinTextures = !npcBodyPartTextures.body[0].empty()
										 || !npcBodyPartTextures.hands[0].empty()
										 || !npcBodyPartTextures.feet[0].empty();
					hasNpcTint = npc.hasTint;
					if (hasNpcTint) {
						npcTintR = npc.tintR / 255.0f;
						npcTintG = npc.tintG / 255.0f;
						npcTintB = npc.tintB / 255.0f;
					}
					if (hasNpcSkinTextures || hasNpcTint) {
						wxLogMessage("LeveledListPreviewer: NPC '%s' skin resolved deferred — body='%s' hands='%s' feet='%s' tint=%s",
									 !npc.fullName.empty() ? npc.fullName : npc.editorId,
									 npcBodyPartTextures.body[0],
									 npcBodyPartTextures.hands[0],
									 npcBodyPartTextures.feet[0],
									 hasNpcTint ? "yes" : "no");
						// Drop only the previously loaded race body shapes; leave
						// outfit-shape entries in shapeTexFiles / bodyShapePartMap
						// so ReconcileBodyAndOutfitShapes (called below) can still
						// see what slots the outfit covers and retarget textures.
						for (auto& name : bodyShapeNames) {
							gls.DeleteMesh(name);
							shapeNifSource.erase(name);
							shapeMaterials.erase(name);
							shapeArmoName.erase(name);
							bodyShapePartMap.erase(name);
							shapeTexFiles.erase(name);
						}
						bodyShapeNames.clear();
						bodyGameVerts.clear();
						LoadBodyMeshes();
					}
				}
				break;
			}
		}
	}

	wxLog::FlushActive();
	SetStatusText(wxString::Format(_("Outfit: %s — %d meshes loaded"), outfit.name, loadedCount));

	// After all outfit pieces are loaded, hide race body shapes that duplicate
	// outfit-bundled body shapes, and apply NPC/race skin TXST to the bundled
	// body shapes so they pick up the right skin tone (per game behavior).
	ReconcileBodyAndOutfitShapes();

	RefreshMeshOverlay();
	gls.RenderOneFrame();
}

// ---------------------------------------------------------------------------
// Texture loading (same pattern as PreviewWindow)
// ---------------------------------------------------------------------------

bool LeveledListPreviewer::AddNifShapeTextures(NifFile* fromNif, const std::string& shapeName, const std::vector<lldata::TextureOverride>* overrides, const std::string& meshName) {
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

		// Case-insensitive filesystem fallback for Linux
		if (mat.Failed()) {
			std::string ciMatPath = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, matFile);
			if (!ciMatPath.empty())
				mat = MaterialFile(ciMatPath);
		}

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
			if (ovr.shapeName.size() == shapeName.size()
				&& std::equal(ovr.shapeName.begin(), ovr.shapeName.end(), shapeName.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
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
		wxLogMessage("  tex[0] for '%s': %s (exists=%d)", wxString(shapeName), wxString(texFiles[0]), wxFileName::FileExists(texFiles[0]) ? 1 : 0);

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
		shapeTexFiles[lookupName] = texFiles;

		if (hasMat)
			m->UpdateFromMaterialFile(mat);

		gls.UpdateShaders(m);
	}
	else {
		wxLogWarning("  AddMaterial returned null for '%s'", wxString(shapeName));
	}

	wxLogMessage("  result for '%s': hasDiffuse=%d glMat=%s visible=%d", wxString(shapeName), hasDiffuse ? 1 : 0, glMat ? "ok" : "NULL", m->bVisible ? 1 : 0);

	return hasDiffuse;
}

// ---------------------------------------------------------------------------
// Mesh list overlay
// ---------------------------------------------------------------------------

void LeveledListPreviewer::RepositionMeshOverlay() {
	if (!meshOverlayPanel || !canvas)
		return;
	wxPoint pos = canvas->GetPosition();
	meshOverlayPanel->SetPosition(pos + wxPoint(5, 5));
}

void LeveledListPreviewer::SyncMeshOverlayStates() {
	// Full rebuild to keep NIF-group checkboxes in sync
	RefreshMeshOverlay();
}

void LeveledListPreviewer::SwitchUseAnyVariant(size_t groupIdx, int newVariantIdx) {
	if (groupIdx >= useAnyGroups_.size())
		return;
	auto& group = useAnyGroups_[groupIdx];
	if (newVariantIdx < 0 || newVariantIdx >= static_cast<int>(group.variants.size()))
		return;
	if (newVariantIdx == group.activeIndex)
		return;

	// Hide old active variant
	if (group.activeIndex >= 0 && group.activeIndex < static_cast<int>(group.variants.size())) {
		for (auto& sn : group.variants[group.activeIndex].shapeNames)
			gls.SetMeshVisibility(sn, false);
	}

	// Show new active variant (but keep untextured shapes hidden if appropriate)
	group.activeIndex = newVariantIdx;
	std::set<std::string> untexturedSet(untexturedShapes.begin(), untexturedShapes.end());
	for (auto& sn : group.variants[newVariantIdx].shapeNames) {
		if (!showUntextured && untexturedSet.count(sn))
			continue; // keep hidden — no valid texture
		gls.SetMeshVisibility(sn, true);
	}

	wxLogMessage("  Switched Use Any group %d to variant %d ('%s')", group.groupId, newVariantIdx, group.variants[newVariantIdx].label);

	// Use exactly the same algorithm as initial load for re-calculating slots and visibility
	UpdateActiveSlotsAndVisibility();

	gls.RenderOneFrame();
	RefreshMeshOverlay();
}

void LeveledListPreviewer::RefreshMeshOverlay() {
	if (!meshOverlayPanel)
		return;

	meshOverlayPanel->DestroyChildren();
	meshCheckboxes.clear();

	// --- Build grouped structure: category → NIF path → list of shapes ---
	struct ShapeEntry {
		std::string shapeName;
		std::string displayName;
		bool visible;
	};
	struct NifGroup {
		std::string nifPath;	 // full relative path
		std::string displayName; // short filename for display
		std::vector<ShapeEntry> shapes;
	};
	struct Category {
		std::string label;
		std::vector<NifGroup> groups;
	};

	auto buildCategory = [&](const std::string& label, const std::vector<std::string>& names, std::function<std::string(const std::string&)> displayFn) -> Category {
		Category cat;
		cat.label = label;

		// Group shapes by NIF source path, preserving order
		std::vector<std::string> orderedNifs;
		std::unordered_map<std::string, std::vector<ShapeEntry>> byNif;
		std::unordered_map<std::string, std::string> nifDisplayName; // nifPath → display name
		for (auto& n : names) {
			Mesh* m = gls.GetMesh(n);
			std::string nif = "unknown";
			auto srcIt = shapeNifSource.find(n);
			if (srcIt != shapeNifSource.end())
				nif = srcIt->second;
			if (byNif.find(nif) == byNif.end()) {
				orderedNifs.push_back(nif);
				// Prefer ARMO display name; fall back to NIF filename
				auto armoIt = shapeArmoName.find(n);
				if (armoIt != shapeArmoName.end() && !armoIt->second.empty()) {
					nifDisplayName[nif] = armoIt->second;
				}
				else {
					auto slashPos = nif.find_last_of("/\\");
					nifDisplayName[nif] = (slashPos != std::string::npos) ? nif.substr(slashPos + 1) : nif;
				}
			}
			byNif[nif].push_back({n, displayFn(n), m ? m->bVisible : true});
		}
		for (auto& nifPath : orderedNifs) {
			NifGroup grp;
			grp.nifPath = nifPath;
			grp.displayName = nifDisplayName[nifPath];
			grp.shapes = std::move(byNif[nifPath]);
			cat.groups.push_back(std::move(grp));
		}
		return cat;
	};

	std::vector<Category> categories;

	auto headCat = buildCategory("Head", headShapeNames, [](const std::string& n) -> std::string {
		if (n.size() > 6 && n.substr(0, 6) == "_head_") {
			std::string base = n.substr(6);
			// Clean up common head part shape names
			if (base.find("Eye") != std::string::npos) return "Eyes (" + base + ")";
			if (base.find("Teeth") != std::string::npos || base.find("Mouth") != std::string::npos) return "Teeth (" + base + ")";
			if (base.find("Hair") != std::string::npos) return "Hair (" + base + ")";
			if (base.find("Brow") != std::string::npos) return "Brows (" + base + ")";
			return base;
		}
		return n;
	});
	if (!headCat.groups.empty())
		categories.push_back(std::move(headCat));

	// Separate bodyShapeNames into Body, Hands, Feet categories based on partition IDs
	std::vector<std::string> bodyOnly, handsOnly, feetOnly;
	for (auto& n : bodyShapeNames) {
		auto it = bodyShapePartMap.find(n);
		if (it != bodyShapePartMap.end()) {
			if (it->second.count(33)) handsOnly.push_back(n);
			else if (it->second.count(37)) feetOnly.push_back(n);
			else bodyOnly.push_back(n);
		}
		else bodyOnly.push_back(n);
	}

	auto bodyOnlyCat = buildCategory("Body", bodyOnly, [](const std::string& n) -> std::string { return n; });
	if (!bodyOnlyCat.groups.empty())
		categories.push_back(std::move(bodyOnlyCat));

	auto handsOnlyCat = buildCategory("Hands", handsOnly, [](const std::string& n) -> std::string { return n; });
	if (!handsOnlyCat.groups.empty())
		categories.push_back(std::move(handsOnlyCat));

	auto feetOnlyCat = buildCategory("Feet", feetOnly, [](const std::string& n) -> std::string { return n; });
	if (!feetOnlyCat.groups.empty())
		categories.push_back(std::move(feetOnlyCat));

	// For outfit shapes, include non-grouped shapes AND shapes from the active variant of each group.
	// Non-active variant shapes are excluded from the overlay.
	std::set<std::string> activeVariantShapes;
	for (auto& group : useAnyGroups_) {
		if (group.activeIndex >= 0 && group.activeIndex < static_cast<int>(group.variants.size())) {
			for (auto& sn : group.variants[group.activeIndex].shapeNames)
				activeVariantShapes.insert(sn);
		}
	}

	std::vector<std::string> visibleOutfitShapes;
	for (auto& n : outfitShapeNames) {
		auto it = shapeToVariantGroup_.find(n);
		if (it == shapeToVariantGroup_.end()) {
			// Not in a variant group — always include
			visibleOutfitShapes.push_back(n);
		}
		else if (activeVariantShapes.count(n)) {
			// In a variant group and is the active variant — include
			visibleOutfitShapes.push_back(n);
		}
	}

	auto outfitCat = buildCategory("Outfit", visibleOutfitShapes, [this](const std::string& n) -> std::string {
		auto it = outfitShapeNifName_.find(n);
		if (it != outfitShapeNifName_.end())
			return it->second;
		return n;
	});
	if (!outfitCat.groups.empty())
		categories.push_back(std::move(outfitCat));

	bool hasVariantGroups = !useAnyGroups_.empty();

	if (categories.empty() && !hasVariantGroups) {
		meshOverlayPanel->Hide();
		return;
	}

	wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

	for (auto& cat : categories) {
		// Category header (bold)
		wxStaticText* header = new wxStaticText(meshOverlayPanel, wxID_ANY, wxString::FromUTF8(cat.label));
		wxFont boldFont = header->GetFont();
		boldFont.SetWeight(wxFONTWEIGHT_BOLD);
		header->SetFont(boldFont);
		sizer->Add(header, 0, wxLEFT | wxTOP, 4);

		for (auto& grp : cat.groups) {
			bool expanded = expandedNifGroups.count(grp.nifPath) > 0;

			// NIF group row: triangle + checkbox
			wxBoxSizer* grpSizer = new wxBoxSizer(wxHORIZONTAL);

			// Expand/collapse triangle button — only if more than one shape
			bool canExpand = grp.shapes.size() > 1;
			if (canExpand) {
				wxButton* triBtn = new wxButton(meshOverlayPanel,
												wxID_ANY,
												expanded ? wxString::FromUTF8("\u25BC") : wxString::FromUTF8("\u25B6"),
												wxDefaultPosition,
												wxSize(18, 18),
												wxBU_EXACTFIT | wxBORDER_NONE);
				triBtn->SetBackgroundColour(meshOverlayPanel->GetBackgroundColour());
				std::string nifKey = grp.nifPath;
				triBtn->Bind(wxEVT_BUTTON, [this, nifKey](wxCommandEvent&) {
					if (expandedNifGroups.count(nifKey))
						expandedNifGroups.erase(nifKey);
					else
						expandedNifGroups.insert(nifKey);
					RefreshMeshOverlay();
				});
				grpSizer->Add(triBtn, 0, wxALIGN_CENTER_VERTICAL);
			}
			else {
				grpSizer->AddSpacer(18);
			}

			// NIF-level checkbox toggles all shapes in this group
			bool allVisible = true;
			for (auto& e : grp.shapes)
				if (!e.visible) {
					allVisible = false;
					break;
				}

			wxCheckBox* grpCb = new wxCheckBox(meshOverlayPanel, wxID_ANY, wxString::FromUTF8(grp.displayName));
			grpCb->SetValue(allVisible);
			std::vector<std::string> grpShapeNames;
			for (auto& e : grp.shapes)
				grpShapeNames.push_back(e.shapeName);
			grpCb->Bind(wxEVT_CHECKBOX, [this, grpShapeNames](wxCommandEvent& ev) {
				bool vis = ev.IsChecked();
				for (auto& sn : grpShapeNames) {
					gls.SetMeshVisibility(sn, vis);
					auto cbIt = meshCheckboxes.find(sn);
					if (cbIt != meshCheckboxes.end() && cbIt->second)
						cbIt->second->SetValue(vis);
				}
				gls.RenderOneFrame();
			});
			grpSizer->Add(grpCb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
			sizer->Add(grpSizer, 0, wxLEFT, 8);

			// Individual shape checkboxes (shown only when expanded)
			if (expanded) {
				for (auto& entry : grp.shapes) {
					wxCheckBox* cb = new wxCheckBox(meshOverlayPanel, wxID_ANY, wxString::FromUTF8(entry.displayName));
					cb->SetValue(entry.visible);
					std::string shapeName = entry.shapeName;
					std::string nifKey = grp.nifPath;
					cb->Bind(wxEVT_CHECKBOX, [this, shapeName, nifKey](wxCommandEvent& ev) {
						gls.SetMeshVisibility(shapeName, ev.IsChecked());
						gls.RenderOneFrame();
					});
					sizer->Add(cb, 0, wxLEFT | wxBOTTOM, 28);
					meshCheckboxes[entry.shapeName] = cb;
				}
			}
		}
	}

	// --- "Use Any" variant group selectors ---
	if (!useAnyGroups_.empty()) {
		wxStaticText* variantHeader = new wxStaticText(meshOverlayPanel, wxID_ANY, wxT("Variants (select one)"));
		wxFont boldFont = variantHeader->GetFont();
		boldFont.SetWeight(wxFONTWEIGHT_BOLD);
		variantHeader->SetFont(boldFont);
		sizer->Add(variantHeader, 0, wxLEFT | wxTOP, 4);

		for (size_t gi = 0; gi < useAnyGroups_.size(); ++gi) {
			auto& group = useAnyGroups_[gi];
			if (group.variants.size() <= 1)
				continue; // single variant — no selector needed

			// Build choice items
			wxArrayString choices;
			for (auto& v : group.variants)
				choices.Add(wxString::FromUTF8(v.label));

			wxChoice* choice = new wxChoice(meshOverlayPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
			choice->SetSelection(group.activeIndex);

			size_t capturedGi = gi;
			choice->Bind(wxEVT_CHOICE, [this, capturedGi](wxCommandEvent& ev) { SwitchUseAnyVariant(capturedGi, ev.GetSelection()); });

			sizer->Add(choice, 0, wxLEFT | wxTOP | wxRIGHT | wxEXPAND, 8);
		}
	}

	meshOverlayPanel->SetSizer(sizer);
	meshOverlayPanel->FitInside();

	// Size: content width + margin, use full canvas height
	wxSize canvasSize = canvas ? canvas->GetSize() : wxSize(600, 400);
	wxSize minSz = sizer->GetMinSize();
	int panelW = std::min(std::max(minSz.GetWidth() + 20, 160), 400);
	int totalH = minSz.GetHeight() + 12;
	int panelH = std::min(totalH, canvasSize.GetHeight() - 10);

	meshOverlayPanel->SetSize(wxSize(panelW, panelH));
	RepositionMeshOverlay();
	meshOverlayPanel->Layout();
	meshOverlayPanel->Show();
	meshOverlayPanel->Raise();
}

// ---------------------------------------------------------------------------
// Toggle untextured meshes
// ---------------------------------------------------------------------------

void LeveledListPreviewer::OnToggleUntextured(wxCommandEvent& event) {
	showUntextured = event.IsChecked();
	for (auto& name : untexturedShapes)
		gls.SetMeshVisibility(name, showUntextured);
	SyncMeshOverlayStates();
	gls.RenderOneFrame();
}

void LeveledListPreviewer::OnToggleBody(wxCommandEvent& event) {
	showBody = event.IsChecked();
	for (auto& name : bodyShapeNames)
		gls.SetMeshVisibility(name, showBody);
	SyncMeshOverlayStates();
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
	if (meshOverlayPanel)
		meshOverlayPanel->Hide();

	if (canvas && context)
		canvas->SetCurrent(*context);

	gls.Cleanup();
	shapeMaterials.clear();

	// Persist window layout
	bool maximized = IsMaximized();
	Config.SetBoolValue("LLPreviewer/Maximized", maximized);
	if (!maximized) {
		wxPoint pos = GetPosition();
		wxSize sz = GetSize();
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

	// Persist head NPC, preset, and weight
	Config.SetValue("LLPreviewer/HeadEditorId", currentHeadEditorId);
	Config.SetValue("LLPreviewer/PresetName", currentPresetName);
	Config.SetBoolValue("LLPreviewer/HighWeight", useHighWeight);

	int ret = Config.SaveConfig(Config["AppDir"] + "/Config.xml");
	if (ret)
		wxLogWarning("LeveledListPreviewer: failed to save Config.xml (%d).", ret);

	StopSmpSimulation();

	Destroy();
	if (eventHandler) {
		wxCommandEvent closeEvent(wxEVT_LEVELEDLIST_CLOSED);
		eventHandler->ProcessEvent(closeEvent);
	}
}

// ---------------------------------------------------------------------------
// SMP Physics Simulation
// ---------------------------------------------------------------------------

std::string LeveledListPreviewer::FindSkeletonNifPath() {
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	// Try common Skyrim skeleton paths
	static const char* candidatePaths[] = {
		"meshes/actors/character/character assets/skeleton_female.nif",
		"meshes/actors/character/character assets/skeleton.nif",
		"meshes/actors/character/character assets female/skeleton.nif",
	};

	for (auto& candidate : candidatePaths) {
		std::string fullPath = baseGamePath + candidate;
		if (wxFileName::FileExists(fullPath))
			return fullPath;

		// Case-insensitive fallback
		std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, candidate);
		if (!resolved.empty())
			return resolved;
	}

	// Also try the bundled skeletons in res/
	std::string appDir = Config["AppDir"];
	std::string bundled = appDir + "/res/skeleton_female_sse.nif";
	if (wxFileName::FileExists(bundled))
		return bundled;

	return {};
}

std::string LeveledListPreviewer::FindSmpXml(NifFile& nif, const std::string& nifRelativePath) {
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';

	// The SMP XML path is stored in the NIF as NiStringExtraData on the
	// root node with name "HDT Skinned Mesh Physics Object".
	auto extraDatas = nif.GetChildren<NiStringExtraData>(nullptr, true);
	for (auto* sed : extraDatas) {
		if (sed->name.get() == "HDT Skinned Mesh Physics Object") {
			std::string xmlRelPath = sed->stringData.get();
			if (xmlRelPath.empty())
				continue;

			// Normalise backslashes to forward slashes
			std::replace(xmlRelPath.begin(), xmlRelPath.end(), '\\', '/');

			// Strip leading slashes
			while (!xmlRelPath.empty() && xmlRelPath[0] == '/')
				xmlRelPath = xmlRelPath.substr(1);

			// Strip leading "Data/" prefix (case-insensitive) — the base path already
			// points to the Data folder, so keeping it would double the directory.
			if (xmlRelPath.size() > 5) {
				std::string prefix5 = xmlRelPath.substr(0, 5);
				std::transform(prefix5.begin(), prefix5.end(), prefix5.begin(), ::tolower);
				if (prefix5 == "data/")
					xmlRelPath = xmlRelPath.substr(5);
			}

			// The path is relative to the game Data folder
			std::string fullPath = baseGamePath + xmlRelPath;
			if (wxFileName::FileExists(fullPath))
				return fullPath;

			// Try case-insensitive resolve
			std::string resolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, xmlRelPath);
			if (!resolved.empty())
				return resolved;

			wxLogWarning("SMP XML path from NIF '%s' not found on disk: %s", nifRelativePath, fullPath);
		}
	}

	return {};
}

void LeveledListPreviewer::SetupSmpSimulation() {
	StopSmpSimulation();

	if (smpXmlPaths_.empty()) {
		wxLogMessage("SMP: No SMP XML configs found for this outfit");
		return;
	}

	// Find skeleton
	std::string skelPath = FindSkeletonNifPath();
	if (skelPath.empty()) {
		wxLogWarning("SMP: Could not find skeleton NIF");
		return;
	}

	smpSimulator_ = std::make_unique<SmpSimulator>();

	// Load skeleton
	if (!smpSimulator_->LoadSkeleton(skelPath)) {
		wxLogWarning("SMP: Failed to load skeleton from %s", skelPath);
		smpSimulator_.reset();
		return;
	}

	// Pre-load unique NIFs that have SMP configs.
	// We need bone transforms from outfit NIFs BEFORE parsing XML configs,
	// because SMP physics bones (e.g. "vv1 1") only exist in outfit NIFs,
	// not in the skeleton. Without correct transforms, constraint frames
	// are computed from identity and the cloth chain collapses.
	std::unordered_map<std::string, std::unique_ptr<NifFile>> loadedNifs;

	for (auto& [nifPath, xmlPath] : smpXmlPaths_) {
		if (loadedNifs.count(nifPath))
			continue;

		auto nif = std::make_unique<NifFile>();
		std::string resolvedPath = data.ResolveNifPath(nifPath);
		bool loaded = false;
		if (!resolvedPath.empty() && nif->Load(resolvedPath) == 0) {
			loaded = true;
		}
		else {
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (archive && archive->hasFile(nifPath)) {
					wxMemoryBuffer outData;
					archive->fileContents(nifPath, outData);
					if (!outData.IsEmpty()) {
						std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
						std::istringstream stream(content, std::istringstream::binary);
						if (nif->Load(stream) == 0) {
							loaded = true;
							break;
						}
					}
				}
			}
		}
		if (loaded)
			loadedNifs[nifPath] = std::move(nif);
		else
			wxLogWarning("SMP: Failed to load NIF: '%s'", nifPath);
	}

	// Phase 1: Populate bone world transforms from outfit NIFs
	for (auto& [nifPath, nif] : loadedNifs)
		smpSimulator_->PopulateBoneTransforms(*nif);

	// Phase 2: Parse SMP XML configs (bone transforms are now known)
	// Track already-parsed XMLs to avoid duplicate loading when multiple
	// armor pieces reference the same XML (would create duplicate constraints).
	std::unordered_set<std::string> parsedXmls;
	for (auto& [nifPath, xmlPath] : smpXmlPaths_) {
		if (!parsedXmls.insert(xmlPath).second)
			continue;
		if (!smpSimulator_->LoadSmpConfig(xmlPath))
			wxLogWarning("SMP: Failed to load XML config: %s", xmlPath);
	}

	// Phase 3: Load shape skinning data
	for (auto& shapeName : outfitShapeNames) {
		auto srcIt = shapeNifSource.find(shapeName);
		if (srcIt == shapeNifSource.end())
			continue;

		auto nifIt = loadedNifs.find(srcIt->second);
		if (nifIt == loadedNifs.end())
			continue;

		std::string origShapeName;
		auto nameIt = outfitShapeNifName_.find(shapeName);
		if (nameIt != outfitShapeNifName_.end())
			origShapeName = nameIt->second;
		else
			origShapeName = shapeName;

		auto* shape = nifIt->second->FindBlockByName<NiShape>(origShapeName);
		if (shape)
			smpSimulator_->LoadShapeSkinning(*nifIt->second, shape, shapeName);
	}

	// Initialise physics world
	if (!smpSimulator_->Initialise()) {
		wxLogWarning("SMP: Failed to initialise physics simulation");
		smpSimulator_.reset();
		return;
	}

	wxLogMessage("SMP: Simulation ready — %zu physics shapes", smpSimulator_->GetPhysicsShapeNames().size());

	// Feed currently displayed (morphed) mesh vertices into SMP skin data
	// so that preset morphs are preserved when the simulation runs.
	for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
		Mesh* mesh = gls.GetMesh(name);
		if (!mesh || mesh->nVerts <= 0)
			continue;
		std::vector<Vector3> nifVerts(mesh->nVerts);
		for (int i = 0; i < mesh->nVerts; i++)
			nifVerts[i] = Mesh::TransformPosMeshToNif(mesh->verts[i]);
		smpSimulator_->UpdateSkinPositions(name, nifVerts);
	}
}

void LeveledListPreviewer::StopSmpSimulation() {
	smpRunning_ = false;
	smpTimer_.Stop();
	smpSimulator_.reset();
}

void LeveledListPreviewer::OnToggleSmp(wxCommandEvent& event) {
	Config.SetValue("SmpEnabled", event.IsChecked() ? "true" : "false");

	if (event.IsChecked()) {
		if (!smpSimulator_) {
			SetupSmpSimulation();
		}
		if (smpSimulator_ && !smpSimulator_->GetPhysicsShapeNames().empty()) {
			smpRunning_ = true;
			smpTimer_.Start(16); // ~60 FPS
			SetStatusText(_("SMP Physics simulation active"));
		}
		else {
			smpToggle_->SetValue(false);
			SetStatusText(_("No SMP physics data found for this outfit"));
		}
	}
	else {
		smpRunning_ = false;
		smpTimer_.Stop();

		// Reset meshes to their original positions
		if (smpSimulator_) {
			smpSimulator_->Reset();
			// Update mesh vertices to rest pose
			if (canvas && context)
				canvas->SetCurrent(*context);

			for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
				auto& verts = smpSimulator_->GetSkinnedVerts(name);
				Mesh* mesh = gls.GetMesh(name);
				if (!mesh || verts.empty())
					continue;

				int count = std::min(static_cast<int>(verts.size()), mesh->nVerts);
				for (int i = 0; i < count; i++)
					mesh->verts[i] = Mesh::TransformPosNifToMesh(verts[i]);
				mesh->QueueUpdate(Mesh::Position);
				mesh->UpdateBuffers();
			}
			gls.RenderOneFrame();
		}
		SetStatusText(_("SMP Physics simulation stopped"));
	}
}

void LeveledListPreviewer::OnSmpTimer(wxTimerEvent& WXUNUSED(event)) {
	if (!smpRunning_ || !smpSimulator_)
		return;

	if (canvas && context)
		canvas->SetCurrent(*context);

	// Step physics
	smpSimulator_->Step(1.0f / 60.0f);

	// Update mesh vertices
	for (auto& name : smpSimulator_->GetPhysicsShapeNames()) {
		auto& verts = smpSimulator_->GetSkinnedVerts(name);
		Mesh* mesh = gls.GetMesh(name);
		if (!mesh || verts.empty())
			continue;

		int count = std::min(static_cast<int>(verts.size()), mesh->nVerts);
		for (int i = 0; i < count; i++)
			mesh->verts[i] = Mesh::TransformPosNifToMesh(verts[i]);

		mesh->QueueUpdate(Mesh::Position);
		mesh->UpdateBuffers();
	}

	gls.RenderOneFrame();
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
			int menuId = (event.GetKeyCode() == 'U') ? LeveledListPreviewer::ID_ToggleUntextured : LeveledListPreviewer::ID_ToggleBody;
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
