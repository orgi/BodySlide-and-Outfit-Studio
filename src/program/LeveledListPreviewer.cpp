/*
 * LeveledListPreviewer — outfit browser and 3D preview for leveled list ESPs.
 */

#include "LeveledListPreviewer.h"
#include "../components/Anim.h"
#include "BodySlideApp.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <ExtraData.hpp>
#include <Shaders.hpp>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/treectrl.h>

using namespace nifly;

wxDEFINE_EVENT(wxEVT_LEVELEDLIST_CLOSED, wxCommandEvent);

extern ConfigurationManager Config;

// ---------------------------------------------------------------------------
// Tree item data
// ---------------------------------------------------------------------------
// Single payload type used by both the outfit and NPC trees.
//   kind == Outfit / Npc → `topIndex` is an index into filteredOutfits or
//                          filteredNpcIndices. groupId / variantIdx are -1.
//   kind == Group        → variant gate-group node. Only `groupId` is set.
//   kind == Variant      → variant slot node. `groupId` and `variantIdx` set.
//   `unavailable` true   → render greyed-out + serve a tooltip explaining why.
class LLTreeItemData : public wxTreeItemData {
public:
	enum class Kind { Outfit, Npc, Group, Variant, Plugin };
	Kind kind = Kind::Outfit;
	int topIndex = -1;
	int groupId = -1;
	int variantIdx = -1;
	bool unavailable = false;
	std::string tooltip;
};

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
	EVT_TIMER(LeveledListPreviewer::ID_ActorTimer, LeveledListPreviewer::OnActorTimer)
	EVT_FSWATCHER(wxID_ANY, LeveledListPreviewer::OnFileChanged)
wxEND_EVENT_TABLE()

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LeveledListPreviewer::LeveledListPreviewer(wxEvtHandler* handler)
	: wxFrame(nullptr, wxID_ANY, _("Leveled List Previewer"), wxDefaultPosition, wxDefaultSize)
	, eventHandler(handler)
	, actorTimer_(this, ID_ActorTimer) {
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

	// NPC head selector (shared between modes; the head ties Outfit-mode body
	// skin and is also the NPC selected in NPC mode).
	wxBoxSizer* headSizer = new wxBoxSizer(wxHORIZONTAL);
	headSizer->Add(new wxStaticText(leftPanel, wxID_ANY, _("Head:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	headCtrl = new wxTextCtrl(leftPanel, ID_HeadNPC, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	headCtrl->SetToolTip(_("Type an NPC editor ID or name and press Enter"));
	headSizer->Add(headCtrl, 1);
	leftSizer->Add(headSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 5);

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
	smpToggle_->SetToolTip(_("Simulate cloth, hair and body physics with FSMP, as configured by the meshes' physics XML files and defaultBBPs.xml"));
	smpToggle_->Bind(wxEVT_CHECKBOX, &LeveledListPreviewer::OnToggleSmp, this);
	smpSizer->Add(smpToggle_, 0, wxALIGN_CENTER_VERTICAL);
	animToggle_ = new wxCheckBox(leftPanel, ID_ToggleAnim, _("Animation"));
	animToggle_->SetToolTip(_("Play an animation of the actor's character, resolved through the race's behavior project (default: the idle MT_Idle)"));
	animToggle_->Bind(wxEVT_CHECKBOX, &LeveledListPreviewer::OnToggleAnim, this);
	smpSizer->Add(animToggle_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
	clipText_ = new wxTextCtrl(leftPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	clipText_->SetToolTip(_("The animation that plays"));
	smpSizer->Add(clipText_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
	clipButton_ = new wxButton(leftPanel, ID_AnimClip, _("..."), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	clipButton_->SetToolTip(_("Choose one of the animations of the actor's character"));
	clipButton_->Bind(wxEVT_BUTTON, &LeveledListPreviewer::OnChooseClip, this);
	clipButton_->Disable();
	smpSizer->Add(clipButton_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);
	pauseToggle_ = new wxCheckBox(leftPanel, ID_AnimPause, _("Pause"));
	pauseToggle_->SetToolTip(_("Hold the animation; physics keeps running"));
	pauseToggle_->Bind(wxEVT_CHECKBOX, &LeveledListPreviewer::OnTogglePause, this);
	smpSizer->Add(pauseToggle_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
	leftSizer->Add(smpSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// Notebook: tab 0 = Outfits (LVLI/OTFT from the loaded ESP), tab 1 = NPCs.
	// Each tab owns its own list, search box and filters.
	viewNotebook = new wxNotebook(leftPanel, ID_ViewNotebook);

	// --- Outfits tab ---
	wxPanel* outfitsTab = new wxPanel(viewNotebook);
	wxBoxSizer* outfitsSizer = new wxBoxSizer(wxVERTICAL);

	searchCtrl = new wxSearchCtrl(outfitsTab, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	searchCtrl->SetDescriptiveText(_("Search outfits..."));
	searchCtrl->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, &LeveledListPreviewer::OnSearchChanged, this);
	searchCtrl->Bind(wxEVT_TEXT, &LeveledListPreviewer::OnSearchChanged, this);
	outfitsSizer->Add(searchCtrl, 0, wxEXPAND | wxALL, 5);

	wxBoxSizer* levelSizer = new wxBoxSizer(wxHORIZONTAL);
	levelSizer->Add(new wxStaticText(outfitsTab, wxID_ANY, _("Level:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	levelMinSpin = new wxSpinCtrl(outfitsTab, wxID_ANY, "1", wxDefaultPosition, wxSize(60, -1), wxSP_ARROW_KEYS, 1, 100, 1);
	levelMaxSpin = new wxSpinCtrl(outfitsTab, wxID_ANY, "100", wxDefaultPosition, wxSize(60, -1), wxSP_ARROW_KEYS, 1, 100, 100);
	levelSizer->Add(levelMinSpin, 0, wxRIGHT, 5);
	levelSizer->Add(new wxStaticText(outfitsTab, wxID_ANY, _("-")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	levelSizer->Add(levelMaxSpin, 0);
	levelMinSpin->Bind(wxEVT_SPINCTRL, &LeveledListPreviewer::OnLevelChanged, this);
	levelMaxSpin->Bind(wxEVT_SPINCTRL, &LeveledListPreviewer::OnLevelChanged, this);
	outfitsSizer->Add(levelSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	outfitList = new wxTreeCtrl(outfitsTab, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_FULL_ROW_HIGHLIGHT);
	outfitList->AddRoot(wxT("outfits-root"));
	outfitList->Bind(wxEVT_TREE_SEL_CHANGED, &LeveledListPreviewer::OnOutfitSelected, this);
	outfitList->Bind(wxEVT_TREE_ITEM_GETTOOLTIP, &LeveledListPreviewer::OnTreeItemTooltip, this);
	outfitsSizer->Add(outfitList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	outfitsTab->SetSizer(outfitsSizer);
	viewNotebook->AddPage(outfitsTab, _("Outfits"));

	// --- NPCs tab ---
	wxPanel* npcsTab = new wxPanel(viewNotebook);
	wxBoxSizer* npcsSizer = new wxBoxSizer(wxVERTICAL);

	npcSearchCtrl = new wxSearchCtrl(npcsTab, ID_NpcSearch, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	npcSearchCtrl->SetDescriptiveText(_("Search NPCs..."));
	npcSearchCtrl->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, &LeveledListPreviewer::OnNpcSearchChanged, this);
	npcSearchCtrl->Bind(wxEVT_TEXT, &LeveledListPreviewer::OnNpcSearchChanged, this);
	npcsSizer->Add(npcSearchCtrl, 0, wxEXPAND | wxALL, 5);

	loadAllNpcsBtn = new wxButton(npcsTab, ID_LoadAllNpcs, _("Load all NPCs (incl. mod-native)"));
	loadAllNpcsBtn->SetToolTip(_("Scan every plugin in Data/ for natively-defined NPC_ records and add them to the list. May be slow on large mod setups."));
	loadAllNpcsBtn->Bind(wxEVT_BUTTON, &LeveledListPreviewer::OnLoadAllNpcs, this);
	npcsSizer->Add(loadAllNpcsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// Humanoid-only filter — hides creatures (Ice Wraith, Wolf, Dragon, etc.).
	// A race counts as humanoid when its effective armor-race set includes
	// DefaultRace (0x19), i.e. it can wear DefaultRace-targeted ARMA worn meshes.
	npcHumanoidOnlyCheck = new wxCheckBox(npcsTab, ID_NpcHumanoidOnly, _("Humanoid NPCs only"));
	npcHumanoidOnlyCheck->SetToolTip(_("Hide creatures (Ice Wraith, Wolf, Dragon, etc.) and only list NPCs whose race shares humanoid armor compatibility with DefaultRace."));
	npcHumanoidOnlyCheck->SetValue(Config.GetIntValue("LLPreviewer/NpcHumanoidOnly") != 0);
	npcHumanoidOnlyCheck->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
		Config.SetValue("LLPreviewer/NpcHumanoidOnly", npcHumanoidOnlyCheck->GetValue() ? 1 : 0);
		RefreshNpcList();
	});
	npcsSizer->Add(npcHumanoidOnlyCheck, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	// Female-only filter — gated on the NPC's ACBS female flag. Since BodySlide
	// is a female-body utility, the rendered body/outfit only really makes sense
	// for female NPCs; checking this hides every male NPC.
	npcFemaleOnlyCheck = new wxCheckBox(npcsTab, ID_NpcFemaleOnly, _("Female NPCs only"));
	npcFemaleOnlyCheck->SetToolTip(
		_("Hide male NPCs (ACBS female flag = 0). BodySlide renders female bodies/outfits, so male NPCs will appear with a female body unless filtered out."));
	npcFemaleOnlyCheck->SetValue(Config.GetIntValue("LLPreviewer/NpcFemaleOnly") != 0);
	npcFemaleOnlyCheck->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
		Config.SetValue("LLPreviewer/NpcFemaleOnly", npcFemaleOnlyCheck->GetValue() ? 1 : 0);
		RefreshNpcList();
	});
	npcsSizer->Add(npcFemaleOnlyCheck, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	npcList = new wxTreeCtrl(npcsTab, ID_NpcList, wxDefaultPosition, wxDefaultSize, wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_FULL_ROW_HIGHLIGHT);
	npcList->AddRoot(wxT("npcs-root"));
	npcList->Bind(wxEVT_TREE_SEL_CHANGED, &LeveledListPreviewer::OnNpcSelected, this);
	npcList->Bind(wxEVT_TREE_ITEM_GETTOOLTIP, &LeveledListPreviewer::OnTreeItemTooltip, this);
	npcsSizer->Add(npcList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

	npcsTab->SetSizer(npcsSizer);
	viewNotebook->AddPage(npcsTab, _("NPCs"));

	viewNotebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &LeveledListPreviewer::OnViewTabChanged, this);

	// Restore the last-active tab. Page 0 = Outfits, Page 1 = NPCs. Default to
	// Outfits when no preference has been stored yet.
	{
		int storedTab = Config.GetIntValue("LLPreviewer/ActiveTab");
		if (storedTab >= 0 && storedTab < static_cast<int>(viewNotebook->GetPageCount()))
			viewNotebook->ChangeSelection(storedTab);
	}

	leftSizer->Add(viewNotebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

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

	if (Config["AnimEnabled"] == "true" && animToggle_) {
		animToggle_->SetValue(true);
		animRunning_ = true;
	}
	if (Config["SmpEnabled"] == "true" && smpToggle_) {
		smpToggle_->SetValue(true);
		smpRunning_ = true;
	}
	UpdateActorMotion();
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
	// Re-entry guard: wxLog::FlushActive() inside LoadOutfitMeshes can pump
	// the event loop and dispatch a queued CallAfter from OnFileChanged while
	// we are still mid-load. Re-entering data.LoadESP() invalidates the
	// OutfitEntry pointers the outer call is still using.
	if (isLoadingESP_) {
		wxLogMessage("LoadESPFile: reentry detected (already loading), deferring '%s'", filepath.c_str());
		if (!reloadPending_) {
			reloadPending_ = true;
			CallAfter([this, filepath]() {
				reloadPending_ = false;
				if (!isLoadingESP_)
					LoadESPFile(filepath);
			});
		}
		return;
	}
	isLoadingESP_ = true;
	struct LoadingGuard {
		bool& flag;
		~LoadingGuard() { flag = false; }
	} loadingGuard{isLoadingESP_};

	wxLogMessage("LoadESPFile: loading '%s'", filepath.c_str());

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
	animActorDirty_ = true;

	SetStatusText(wxString::Format(_("Loaded %s — %s"), data.GetFilename(), data.GetLoadInfo()));
	RefreshOutfitList();
	RefreshNpcList();

	// Re-select the last outfit if one was persisted. Skip this when the NPC
	// tab is the active one: RefreshNpcList already restored the NPC selection
	// (which triggers SelectNpc → default-outfit render), and overlaying the
	// last leveled-list outfit on top would clobber the NPC's chosen outfit.
	{
		std::string lastOutfit = Config["LLPreviewer/LastOutfit"];
		bool npcTabActive = viewNotebook && viewNotebook->GetSelection() == 1;
		if (!lastOutfit.empty() && !npcTabActive) {
			// Walk Plugin group nodes, then their child Outfit nodes, to find
			// the entry whose underlying OutfitEntry::name matches the
			// persisted one.
			wxTreeItemId root = outfitList->GetRootItem();
			wxTreeItemIdValue pcookie;
			bool found = false;
			for (wxTreeItemId pluginItem = outfitList->GetFirstChild(root, pcookie); pluginItem.IsOk() && !found; pluginItem = outfitList->GetNextChild(root, pcookie)) {
				wxTreeItemIdValue ocookie;
				for (wxTreeItemId child = outfitList->GetFirstChild(pluginItem, ocookie); child.IsOk(); child = outfitList->GetNextChild(pluginItem, ocookie)) {
					auto* d = dynamic_cast<LLTreeItemData*>(outfitList->GetItemData(child));
					if (!d || d->kind != LLTreeItemData::Kind::Outfit)
						continue;
					if (d->topIndex < 0 || d->topIndex >= static_cast<int>(filteredOutfits.size()))
						continue;
					if (filteredOutfits[d->topIndex]->name == lastOutfit) {
						outfitList->Expand(pluginItem);
						outfitList->SelectItem(child);
						outfitList->EnsureVisible(child);
						found = true;
						break;
					}
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

	// Coalesce multiple watcher events from a single ESP rewrite (xEdit /
	// Python scripts typically emit several MODIFY+RENAME events) and skip
	// while a load is in progress to avoid re-entering data.LoadESP() while
	// LoadOutfitMeshes is still using OutfitEntry pointers.
	if (reloadPending_ || isLoadingESP_)
		return;
	reloadPending_ = true;

	// Use CallAfter so the event handler returns quickly and the file has
	// finished being written before we reload.
	CallAfter([this]() {
		reloadPending_ = false;
		if (isLoadingESP_)
			return;
		if (!lastESPFilepath.empty()) {
			wxLogMessage("OnFileChanged: auto-reloading ESP '%s'", lastESPFilepath.c_str());
			LoadESPFile(lastESPFilepath);
		}
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
	wxTreeItemId root = outfitList->AddRoot(wxT("outfits-root"));
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

	// Populate tree: group outfits by source plugin so the user can see
	// which plugin contributed each entry, then individual outfits beneath.
	// Variant subtree is added lazily after the outfit is loaded (see
	// PopulateVariantSubtree).
	std::string mainPluginName;
	{
		wxFileName fn(wxString::FromUTF8(data.GetFilename()));
		mainPluginName = fn.GetFullName().ToStdString();
	}
	// Stable per-plugin order: main ESP first, then alphabetical by filename.
	std::map<std::string, std::vector<size_t>> byPlugin;
	for (size_t i = 0; i < filteredOutfits.size(); ++i) {
		const auto* e = filteredOutfits[i];
		const std::string& key = e->sourcePlugin.empty() ? std::string("<unknown>") : e->sourcePlugin;
		byPlugin[key].push_back(i);
	}
	auto appendPluginGroup = [&](const std::string& pluginKey, const std::vector<size_t>& indices) {
		wxString label = wxString::Format("%s  (%zu)", wxString::FromUTF8(pluginKey), indices.size());
		auto* pd = new LLTreeItemData();
		pd->kind = LLTreeItemData::Kind::Plugin;
		wxTreeItemId pluginItem = outfitList->AppendItem(root, label, -1, -1, pd);
		for (size_t i : indices) {
			auto* entry = filteredOutfits[i];
			wxString olabel = wxString::Format("%s  (L%d, %d/%zu)",
											   wxString::FromUTF8(entry->name),
											   entry->minLevel,
											   entry->resolvedPieceCount,
											   entry->pieces.size());
			auto* d = new LLTreeItemData();
			d->kind = LLTreeItemData::Kind::Outfit;
			d->topIndex = static_cast<int>(i);
			if (entry->resolvedPieceCount == 0) {
				d->unavailable = true;
				if (!entry->loadDiagnostics.empty())
					d->tooltip = entry->loadDiagnostics;
				else
					d->tooltip = "This outfit has no resolvable pieces (no renderable ARMOs were found).";
			}
			wxTreeItemId item = outfitList->AppendItem(pluginItem, olabel, -1, -1, d);
			if (d->unavailable)
				outfitList->SetItemTextColour(item, wxColour(150, 150, 150));
		}
	};
	auto mainIt = byPlugin.find(mainPluginName);
	if (mainIt != byPlugin.end()) {
		appendPluginGroup(mainIt->first, mainIt->second);
		byPlugin.erase(mainIt);
	}
	for (auto& kv : byPlugin)
		appendPluginGroup(kv.first, kv.second);
}

// ---------------------------------------------------------------------------
// NPC list / NPC mode
// ---------------------------------------------------------------------------

void LeveledListPreviewer::RefreshNpcList() {
	if (!npcList)
		return;
	npcList->DeleteAllItems();
	filteredNpcIndices.clear();

	std::string query = npcSearchCtrl ? npcSearchCtrl->GetValue().ToStdString() : "";
	std::string queryLower = query;
	std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), [](unsigned char c) { return std::tolower(c); });

	bool humanoidOnly = npcHumanoidOnlyCheck && npcHumanoidOnlyCheck->GetValue();
	bool femaleOnly = npcFemaleOnlyCheck && npcFemaleOnlyCheck->GetValue();
	// Cache "is humanoid?" per race id across this refresh so we don't walk
	// the skin-ARMO chain once per NPC. A race counts as humanoid when its
	// effective armor-race set includes DefaultRace (0x19) — i.e. it can wear
	// the same ARMA worn meshes as a default human.
	constexpr uint32_t DEFAULT_RACE_FID = 0x00000019;
	std::unordered_map<uint32_t, bool> raceIsHumanoid;
	auto checkHumanoid = [&](uint32_t raceFid) -> bool {
		if (raceFid == 0)
			return false;
		auto it = raceIsHumanoid.find(raceFid);
		if (it != raceIsHumanoid.end())
			return it->second;
		auto effRaces = data.GetEffectiveArmorRaces(raceFid);
		bool human = effRaces.count(DEFAULT_RACE_FID) != 0;
		raceIsHumanoid[raceFid] = human;
		return human;
	};

	auto& npcs = data.GetNPCs();
	wxTreeItemId root = npcList->AddRoot(wxT("npcs-root"));
	wxTreeItemId selectedNode;
	for (size_t i = 0; i < npcs.size(); ++i) {
		const auto& n = npcs[i];
		if (!queryLower.empty()) {
			std::string hay = n.displayName + " " + n.editorId + " " + n.fullName;
			std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
			if (hay.find(queryLower) == std::string::npos)
				continue;
		}
		if (humanoidOnly) {
			auto effRace = data.GetEffectiveNpcRace(n.editorId);
			if (!checkHumanoid(effRace.formId))
				continue;
		}
		if (femaleOnly && !n.isFemale)
			continue;
		filteredNpcIndices.push_back(i);

		std::string label = !n.fullName.empty() ? n.fullName + " (" + n.editorId + ")" : n.editorId;
		if (!n.plugin.empty())
			label += "  [" + n.plugin + "]";
		auto* d = new LLTreeItemData();
		d->kind = LLTreeItemData::Kind::Npc;
		d->topIndex = static_cast<int>(filteredNpcIndices.size() - 1);
		wxTreeItemId item = npcList->AppendItem(root, wxString::FromUTF8(label), -1, -1, d);
		if (!currentHeadEditorId.empty() && n.editorId == currentHeadEditorId)
			selectedNode = item;
	}

	if (selectedNode.IsOk()) {
		npcList->SelectItem(selectedNode);
		npcList->EnsureVisible(selectedNode);
	}
}

void LeveledListPreviewer::OnNpcSearchChanged(wxCommandEvent& WXUNUSED(event)) {
	RefreshNpcList();
}

void LeveledListPreviewer::OnNpcSelected(wxTreeEvent& event) {
	wxTreeItemId id = event.GetItem();
	if (!id.IsOk())
		return;
	auto* d = dynamic_cast<LLTreeItemData*>(npcList->GetItemData(id));
	if (!d)
		return;

	if (d->kind == LLTreeItemData::Kind::Npc) {
		if (d->topIndex < 0 || d->topIndex >= static_cast<int>(filteredNpcIndices.size()))
			return;
		size_t idx = filteredNpcIndices[d->topIndex];
		auto& npcs = data.GetNPCs();
		if (idx >= npcs.size())
			return;
		SelectNpc(npcs[idx], /*loadDefaultOutfit*/ true);
		// After the outfit loads, refresh this NPC node's variant subtree.
		PopulateVariantSubtree(npcList, id);
		npcList->Expand(id);
		return;
	}

	if (d->kind == LLTreeItemData::Kind::Variant) {
		if (d->unavailable)
			return;
		ActivateVariantChain(d->groupId, d->variantIdx, /*descend*/ true);
		return;
	}

	if (d->kind == LLTreeItemData::Kind::Group) {
		// Selecting a gate group node activates its first variant.
		for (auto& g : useAnyGroups_) {
			if (g.groupId == d->groupId) {
				if (!g.variants.empty())
					ActivateVariantChain(d->groupId, g.variants.front().variantIdx, /*descend*/ true);
				break;
			}
		}
	}
}

void LeveledListPreviewer::OnLoadAllNpcs(wxCommandEvent& WXUNUSED(event)) {
	wxBusyCursor wait;
	SetStatusText(_("Scanning all plugins for NPC_ records..."));
	wxYield();
	size_t before = data.GetNPCs().size();
	data.LoadAllPluginNativeNPCs();
	size_t after = data.GetNPCs().size();

	// Rebuild the autocomplete map and refresh the NPC list.
	npcByDisplay.clear();
	wxArrayString npcNames;
	auto& all = data.GetNPCs();
	npcNames.reserve(all.size());
	for (size_t i = 0; i < all.size(); ++i) {
		npcByDisplay[all[i].displayName] = i;
		npcNames.push_back(wxString::FromUTF8(all[i].displayName));
	}
	if (headCtrl)
		headCtrl->AutoComplete(npcNames);
	RefreshNpcList();

	SetStatusText(wxString::Format(_("Loaded %zu additional mod-native NPCs (total %zu)"), after - before, after));
}

void LeveledListPreviewer::OnViewTabChanged(wxBookCtrlEvent& event) {
	// Persist the active tab so the previewer reopens on whichever view (Outfits
	// or NPCs) the user was last using.
	Config.SetValue("LLPreviewer/ActiveTab", event.GetSelection());

	// Re-sync the NPC list when switching INTO the NPCs tab so the currently
	// selected NPC (driven by Outfit-mode head selector or vice versa) is
	// highlighted. Refresh is also responsible for re-applying the search
	// filter the user typed last time.
	if (event.GetSelection() == 1)
		RefreshNpcList();
	event.Skip();
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

bool LeveledListPreviewer::LoadNifFromPath(const std::string& relativePath, const std::string& prefix, int sourceSlot) {
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

	std::vector<std::pair<NiShape*, Mesh*>> modelMeshes;
	for (auto& shapeName : nif.GetShapeNames()) {
		// Head geometry comes from the NPC's FaceGen NIF only. A shape named
		// after an HDPT record is head-part geometry and is not loaded from here.
		if (data.IsHeadPartEditorId(shapeName)) {
			wxLogMessage("  Ignoring head part shape '%s' in '%s' (head is provided by FaceGen NIF)", shapeName, relativePath);
			continue;
		}

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
		if (sourceSlot != 0)
			bodyShapeSourceSlot[m->shapeName] = sourceSlot;

		// Extract partition body part IDs from NIF dismember skin
		auto* shape = nif.FindBlockByName<NiShape>(shapeName);
		if (shape) {
			modelMeshes.emplace_back(shape, m);

			NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
			std::vector<int> triParts;
			if (nif.GetShapePartitions(shape, partInfo, triParts)) {
				std::set<uint16_t> partIds;
				for (size_t i = 0; i < partInfo.size(); ++i)
					partIds.insert(partInfo[i].partID);
				bodyShapePartMap[m->shapeName] = std::move(partIds);
				wxLogMessage("  Body shape '%s' partition IDs: %zu entries", m->shapeName, bodyShapePartMap[m->shapeName].size());
			}

			// Record whether the shape's BSLightingShaderProperty has Skin Tint
			// enabled. The game engine forces the NPC's NAM1 skin textures + QNAM
			// tint onto skin-tinted shapes regardless of dismember partition or
			// alt-tex entries; non-skin-tinted shapes keep their NIF-embedded
			// BSShaderTextureSet untouched.
			NiShader* shader = nif.GetShader(shape);
			bool skinTinted = shader && shader->IsSkinTinted();
			shapeIsSkinTinted[m->shapeName] = skinTinted;
			if (auto* bslsp = dynamic_cast<BSLightingShaderProperty*>(shader)) {
				wxLogMessage("  Body shape '%s' skinTinted=%d bslspShaderType=%u SF1=0x%08X SF2=0x%08X tint=(%.3f,%.3f,%.3f) clamp=%u alpha=%.3f",
							 m->shapeName,
							 skinTinted ? 1 : 0,
							 shader->bslspShaderType,
							 bslsp->shaderFlags1,
							 bslsp->shaderFlags2,
							 bslsp->skinTintColor.x,
							 bslsp->skinTintColor.y,
							 bslsp->skinTintColor.z,
							 bslsp->textureClampMode,
							 bslsp->alpha);
			}
			else {
				wxLogMessage("  Body shape '%s' skinTinted=%d (no BSLightingShaderProperty)", m->shapeName, skinTinted ? 1 : 0);
			}
		}
	}

	// The race's skin is worn wherever no shown outfit shape covers its slots.
	std::vector<std::string> modelMeshNames;
	for (auto& entry : modelMeshes)
		modelMeshNames.push_back(entry.second->shapeName);
	actorAnim_.RegisterModel(nif, relativePath, ActorAnimation::ModelKind::Armor, modelMeshes, [this, modelMeshNames]() {
		for (auto& name : modelMeshNames) {
			auto it = raceBodyShapeCovered_.find(name);
			if (it == raceBodyShapeCovered_.end() || !it->second)
				return true;
		}
		return false;
	});

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
			if (raceBodySet.count(it->first))
				it = bodyGameVerts.erase(it);
			else
				++it;
		}
		for (auto it = bodyShapePartMap.begin(); it != bodyShapePartMap.end();) {
			if (raceBodySet.count(it->first))
				it = bodyShapePartMap.erase(it);
			else
				++it;
		}
		for (auto it = shapeIsSkinTinted.begin(); it != shapeIsSkinTinted.end();) {
			if (raceBodySet.count(it->first))
				it = shapeIsSkinTinted.erase(it);
			else
				++it;
		}
		for (auto it = bodyShapeSourceSlot.begin(); it != bodyShapeSourceSlot.end();) {
			if (raceBodySet.count(it->first))
				it = bodyShapeSourceSlot.erase(it);
			else
				++it;
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
		wxLogMessage("LeveledListPreviewer: ResolveNpcBodyNifPaths('%s') -> body='%s' hands='%s' feet='%s'", currentHeadEditorId, paths.body, paths.hands, paths.feet);

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
			std::string err = "NPC '" + currentHeadEditorId + "': no skin NIF defined for " + slotList + " (neither NPC's WNAM nor race's WNAM resolves)";
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
		if (!LoadNifFromPath(entry.path, "", entry.slot)) {
			wxLogMessage("LeveledListPreviewer: Body part not found: %s", entry.path);
			continue;
		}
		// Visibility of race body shapes is decided later in
		// ReconcileBodyAndOutfitShapes(), which inspects the dismember
		// partitions of the actually-loaded outfit shapes. We must not
		// pre-hide based solely on outfitDeclaredSlots (the union of ARMO
		// BOD2 flags), because an ARMO may declare e.g. slot 33 (Hands)
		// without its NIF actually containing a hands shape — pre-hiding
		// would leave the body without any hands geometry.
	}

	if (bodyShapeNames.empty())
		return;

	// When NPC is selected but neither the NPC's WNAM nor the race's WNAM defined
	// any TXST overrides, fall through and let the loaded NIF's embedded
	// BSShaderTextureSet (the model author's chosen textures) remain in effect.
	// That IS the in-game behavior and the user-requested fallback — log it so
	// the chain is traceable.
	if (!currentHeadEditorId.empty() && !hasNpcSkinTextures) {
		wxLogMessage("LeveledListPreviewer: NPC '%s' has no WNAM/race TXST override - using NIF embedded textures", currentHeadEditorId);
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

		// Game-accurate skin retexture: the engine forces the NPC's NAM1
		// skin TXST (and QNAM tint) onto shapes whose BSLightingShaderProperty
		// has Skin Tint enabled (bslspShaderType == BSLSP_SKINTINT). All other
		// shapes — including secondary anatomy shapes (3BA vagina/anus) that
		// share partition 32 with the body but carry their own non-skin-tint
		// shader — keep their NIF-embedded BSShaderTextureSet untouched and
		// do not receive the QNAM tint.
		//
		// Slot routing (body vs hands vs feet TXST) is by which body NIF the
		// shape was loaded from (recorded in bodyShapeSourceSlot at load time).
		for (auto& name : bodyShapeNames) {
			auto stIt = shapeIsSkinTinted.find(name);
			if (stIt == shapeIsSkinTinted.end() || !stIt->second) {
				wxLogMessage("LeveledListPreviewer: Preserving NIF-embedded textures for non-skin-tinted body shape '%s'", name);
				continue;
			}
			auto slotIt = bodyShapeSourceSlot.find(name);
			if (slotIt == bodyShapeSourceSlot.end())
				continue;
			switch (slotIt->second) {
				case 32: applyToShape(name, bodyTex, "body"); break;
				case 33: applyToShape(name, handsTex, "hands"); break;
				case 37: applyToShape(name, feetTex, "feet"); break;
			}
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

	// Set of dismember slot IDs that any outfit shape occupies. Used below
	// only for choosing which race-shape textures to copy onto outfit body
	// shapes (the visibility hide decision now lives in
	// ApplyRaceBodyPartVisibility, which keys off currently-shown outfit
	// shapes so per-shape overlay toggles can re-reveal hidden body parts).
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

	// Also treat slots declared by the outfit ARMOs' BOD2 as covered, even when
	// the bundled NIF shapes carry different partitions (common for high heels
	// and gauntlets that paint themselves with partition 32 but whose ARMA BOD2
	// explicitly claims slot 37 / 33). This matches game behaviour: the engine
	// uses the ARMA slot mask to suppress race body shapes, not the NIF
	// partitions. Example: Vin Valkyrie boots NIF shapes are tagged [32], but
	// the ARMA declares slot 37 — without this union the race Feet shape would
	// clip through the boots.
	for (int slot : {32, 33, 37}) {
		if (outfitDeclaredSlots.count(slot))
			outfitCoveredSlots.insert(static_cast<uint16_t>(slot));
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

	// Drive race body / hair visibility from the set of currently visible
	// outfit shapes (mirrors the user's overlay state instead of the outfit's
	// declared slot union).
	ApplyRaceBodyPartVisibility();

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

	// Game-accurate retexture for outfit shapes: only shapes whose
	// BSLightingShaderProperty has Skin Tint enabled receive the NPC's NAM1
	// skin TXST + QNAM tint. Slot routing is by dismember partition (32/33/37).
	// Non-skin-tinted outfit shapes keep their NIF-embedded BSShaderTextureSet,
	// which is what the engine does too.
	for (auto& outfitShape : outfitShapeNames) {
		auto stIt = shapeIsSkinTinted.find(outfitShape);
		if (stIt == shapeIsSkinTinted.end() || !stIt->second)
			continue;
		auto it = bodyShapePartMap.find(outfitShape);
		if (it == bodyShapePartMap.end())
			continue;

		uint16_t slot = 0;
		const std::vector<std::string>* tex = nullptr;
		const char* slotLabel = nullptr;
		if (it->second.count(32)) {
			slot = 32;
			slotLabel = "body";
			if (!bodyTex.empty() && !bodyTex[0].empty())
				tex = &bodyTex;
		}
		else if (it->second.count(33)) {
			slot = 33;
			slotLabel = "hands";
			if (!handsTex.empty() && !handsTex[0].empty())
				tex = &handsTex;
		}
		else if (it->second.count(37)) {
			slot = 37;
			slotLabel = "feet";
			if (!feetTex.empty() && !feetTex[0].empty())
				tex = &feetTex;
		}
		if (slot == 0)
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
		headGameVerts.erase(name);
		headShapeNifName_.erase(name);
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
	std::string loadedPath = relativePath;

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
			if (!archive || !archive->hasFile(relativePath))
				continue;
			wxMemoryBuffer outData;
			archive->fileContents(relativePath, outData);
			if (outData.IsEmpty())
				continue;
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
				loadedPath = ovRelPath;
				wxLogMessage("LeveledListPreviewer: FaceGen via override (loose): %s", ovRelPath);
				break;
			}
			std::string ovResolved = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, ovRelPath);
			if (!ovResolved.empty() && nif.Load(ovResolved) == 0) {
				loaded = true;
				loadedPath = ovRelPath;
				wxLogMessage("LeveledListPreviewer: FaceGen via override (loose CI): %s", ovRelPath);
				break;
			}
			for (FSArchiveFile* archive : FSManager::archiveList()) {
				if (!archive || !archive->hasFile(ovRelPath))
					continue;
				wxMemoryBuffer outData;
				archive->fileContents(ovRelPath, outData);
				if (outData.IsEmpty())
					continue;
				std::string content(static_cast<char*>(outData.GetData()), outData.GetDataLen());
				std::istringstream stream(content, std::istringstream::binary);
				if (nif.Load(stream) == 0) {
					loaded = true;
					loadedPath = ovRelPath;
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
				return; // shape isn't weighted to NPC Head — leave AddMeshFromNif's positioning

		nifly::MatTransform targetNif = charHeadBoneGlobal.ComposeTransforms(skinToBone);
		nifly::MatTransform targetMesh = Mesh::xformNifToMesh.ComposeTransforms(targetNif.ComposeTransforms(Mesh::xformMeshToNif));
		m->SetXformMeshToModel(targetMesh);
		wxLogMessage("LeveledListPreviewer: head correction for '%s': skinToBone=(%.2f,%.2f,%.2f) charHead=(%.2f,%.2f,%.2f)",
					 shapeName,
					 skinToBone.translation.x,
					 skinToBone.translation.y,
					 skinToBone.translation.z,
					 charHeadBoneGlobal.translation.x,
					 charHeadBoneGlobal.translation.y,
					 charHeadBoneGlobal.translation.z);
	};

	std::vector<std::pair<NiShape*, Mesh*>> modelMeshes;
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
		auto toLower = [](std::string s) {
			for (auto& c : s)
				c = std::tolower(static_cast<unsigned char>(c));
			return s;
		};
		std::string lowerShape = toLower(shapeName);
		bool isFaceShape = lowerShape.find("head") != std::string::npos || lowerShape.find("face") != std::string::npos || lowerShape.find("basehead") != std::string::npos;

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
		headShapeNifName_[meshName] = shapeName;
		if (m->nVerts > 0) {
			std::vector<Vector3> gameVerts(m->nVerts);
			for (int i = 0; i < m->nVerts; i++)
				gameVerts[i] = Mesh::TransformPosMeshToNif(m->verts[i]);
			headGameVerts[meshName] = std::move(gameVerts);
		}
		GetOrLoadTriFile(relativePath);

		// Extract partition body part IDs from NIF dismember skin
		auto* shape = nif.FindBlockByName<NiShape>(shapeName);
		if (shape) {
			modelMeshes.emplace_back(shape, m);

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
	actorAnim_.RegisterModel(nif, loadedPath, ActorAnimation::ModelKind::FaceGenHead, modelMeshes, nullptr);

	UpdateHeadVisibility();

	// Apply the active BodySlide preset to head morphs so the head's neck
	// vertices follow the body's morphed neck (closes the otherwise-visible
	// neck seam on the standard races).
	if (!currentPresetName.empty() && currentPresetName != "(none)")
		ApplyPresetToHead(currentPresetName, true);

	wxLogMessage("LeveledListPreviewer: Loaded head NIF for NPC '%s' (%s): %zu shapes", npc.editorId, formIdBuf, headShapeNames.size());
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
	SelectNpc(npc, /*loadDefaultOutfit*/ false);
}

// ---------------------------------------------------------------------------
// SelectNpc — shared head + body + skin texture wiring for both modes
// ---------------------------------------------------------------------------

void LeveledListPreviewer::SelectNpc(const lldata::NPCEntry& npc, bool loadDefaultOutfit) {
	currentHeadEditorId = npc.editorId;
	Config.SetValue("LLPreviewer/HeadEditorId", npc.editorId);
	LoadHeadMesh(npc);

	// Show the display name (full in-game name) in the text box, not the editor ID.
	if (headCtrl && !npc.displayName.empty() && headCtrl->GetValue().ToStdString() != npc.displayName)
		headCtrl->ChangeValue(wxString::FromUTF8(npc.displayName));

	const std::string& logName = !npc.fullName.empty() ? npc.fullName : npc.editorId;

	// Resolve per-slot NPC skin textures (body / hands / feet independently).
	npcBodyPartTextures = data.ResolveNpcBodyPartTextures(npc.editorId);
	hasNpcSkinTextures = !npcBodyPartTextures.body[0].empty() || !npcBodyPartTextures.hands[0].empty() || !npcBodyPartTextures.feet[0].empty();

	if (hasNpcSkinTextures) {
		std::string skinLog = "LeveledListPreviewer: NPC '" + logName + "' skin resolved — body='" + npcBodyPartTextures.body[0] + "' hands='" + npcBodyPartTextures.hands[0]
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
		wxLogMessage("LeveledListPreviewer: NPC '%s' tint RGB=(%u,%u,%u)", logName.c_str(), npc.tintR, npc.tintG, npc.tintB);
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

	if (loadDefaultOutfit) {
		const lldata::OutfitEntry* outfit = data.ResolveNpcDefaultOutfit(npc);
		if (outfit) {
			wxLogMessage("SelectNpc: rendering default outfit '%s' for NPC '%s'", outfit->name.c_str(), npc.editorId.c_str());
			LoadOutfitMeshes(*outfit);
		}
		else {
			// No default outfit known — drop any previously loaded outfit pieces
			// so the NPC stands in just race body + head.
			if (canvas && context)
				canvas->SetCurrent(*context);
			for (auto& name : outfitShapeNames)
				gls.DeleteMesh(name);
			outfitShapeNames.clear();
			outfitGameVerts.clear();
			shapeArmoBodySlots.clear();
			useAnyGroups_.clear();
			shapeUseAnyPath_.clear();
			variantNifLoadFailures_.clear();
			loadedOutfitPieces_.clear();
			renderPieceIdx_.clear();
			currentOutfit = nullptr;
			// Distinguish "no DOFT defined" (normal for creatures) from "DOFT exists
			// but couldn't be resolved" (mod outfit chain not loaded).
			if (npc.defaultOutfitFormIdRaw == 0)
				SetStatusText(wxString::Format(_("NPC '%s' has no default outfit"), wxString::FromUTF8(logName)));
			else
				SetStatusText(
					wxString::Format(_("NPC '%s' default outfit unresolvable (defined in '%s')"), wxString::FromUTF8(logName), wxString::FromUTF8(npc.defaultOutfitSourcePlugin)));
		}
	}

	RefreshMeshOverlay();
	gls.RenderOneFrame();
}

// ---------------------------------------------------------------------------
// BodySlide preset loading and application
// ---------------------------------------------------------------------------

void LeveledListPreviewer::UpdateHeadVisibility() {
	// Hair (slot 31) and the race body parts (slots 32/33/37) are now driven
	// by ApplyRaceBodyPartVisibility, which inspects currently visible outfit
	// shapes rather than the outfit's declared-slot union.
	ApplyRaceBodyPartVisibility();
}

void LeveledListPreviewer::ApplyRaceBodyPartVisibility() {
	if (bodyShapeNames.empty() && headShapeNames.empty())
		return;

	// Gather slot coverage from currently VISIBLE outfit shapes only. Slots
	// 32/33/37 may be contributed by either NIF dismember partitions or by
	// the piece's ARMO BOD2; slot 31 (hair) is BOD2-only because hair-
	// covering pieces (hoods, helms) rarely paint partition 31 on their
	// own geometry.
	std::set<int> shownSlots;
	for (auto& sn : outfitShapeNames) {
		Mesh* m = gls.GetMesh(sn);
		if (!m || !m->bVisible)
			continue;
		auto pit = bodyShapePartMap.find(sn);
		if (pit != bodyShapePartMap.end()) {
			for (uint16_t p : pit->second) {
				if (p == 32 || p == 33 || p == 37)
					shownSlots.insert(static_cast<int>(p));
			}
		}
		auto sit = shapeArmoBodySlots.find(sn);
		if (sit != shapeArmoBodySlots.end()) {
			for (int s : sit->second) {
				if (s == 31 || s == 32 || s == 33 || s == 37)
					shownSlots.insert(s);
			}
		}
	}

	// Race body parts (32/33/37): hide if a visible outfit shape covers the
	// slot. Honour the global `showBody` toggle as an upper bound.
	for (auto& raceShape : bodyShapeNames) {
		auto it = bodyShapePartMap.find(raceShape);
		if (it == bodyShapePartMap.end())
			continue;
		bool overlap = false;
		for (uint16_t p : it->second) {
			if ((p == 32 || p == 33 || p == 37) && shownSlots.count(static_cast<int>(p))) {
				overlap = true;
				break;
			}
		}
		raceBodyShapeCovered_[raceShape] = overlap;
		bool desired = showBody && !overlap;
		Mesh* m = gls.GetMesh(raceShape);
		if (m && m->bVisible != desired) {
			gls.SetMeshVisibility(raceShape, desired);
			wxLogMessage("ApplyRaceBodyPartVisibility: race body shape '%s' -> %s", raceShape, desired ? "show" : "hide");
			auto cbIt = meshCheckboxes.find(raceShape);
			if (cbIt != meshCheckboxes.end() && cbIt->second)
				cbIt->second->SetValue(desired);
		}
	}

	// Hair (slot 31): only the head shapes that carry partition 31.
	bool hideHair = shownSlots.count(31) != 0;
	for (const auto& meshName : headShapeNames) {
		auto it = bodyShapePartMap.find(meshName);
		if (it == bodyShapePartMap.end() || !it->second.count(31))
			continue;
		bool desired = !hideHair;
		Mesh* m = gls.GetMesh(meshName);
		if (m && m->bVisible != desired) {
			gls.SetMeshVisibility(meshName, desired);
			wxLogMessage("ApplyRaceBodyPartVisibility: hair shape '%s' -> %s", meshName, desired ? "show" : "hide");
			auto cbIt = meshCheckboxes.find(meshName);
			if (cbIt != meshCheckboxes.end() && cbIt->second)
				cbIt->second->SetValue(desired);
		}
	}
}

void LeveledListPreviewer::UpdateActiveSlotsAndVisibility() {
	if (!currentOutfit)
		return;

	outfitDeclaredSlots.clear();
	for (auto& piece : currentOutfit->pieces) {
		if (!piece.useAnyPath.empty()) {
			// Pieces sitting behind a gate only contribute slots when every
			// gate on their path is currently selected. During initial load
			// (before useAnyGroups_ is populated) treat the first variant as
			// active for every gate to match the default activeIndex = 0.
			if (useAnyGroups_.empty()) {
				bool allZero = true;
				for (auto& [g, v] : piece.useAnyPath) {
					(void)g;
					if (v != 0) {
						allZero = false;
						break;
					}
				}
				if (!allZero)
					continue;
			}
			else if (!IsUseAnyPathActive(piece.useAnyPath)) {
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

void LeveledListPreviewer::ApplyPresetToHead(const std::string& presetName, bool deferredRender) {
	if (headGameVerts.empty())
		return;

	if (canvas && context)
		canvas->SetCurrent(*context);

	// "(none)" or empty → reset every head shape to its game verts.
	if (presetName.empty() || presetName == "(none)") {
		for (auto& [meshName, verts] : headGameVerts)
			gls.Update(meshName, &verts, nullptr);
		if (!deferredRender)
			gls.RenderOneFrame();
		return;
	}

	std::string projectPath = Config["ProjectPath"];
	if (projectPath.empty())
		projectPath = Config["AppDir"];

	PresetCollection presets;
	std::vector<std::string> noGroups;
	presets.LoadPresets(projectPath + "/SliderPresets", "", noGroups, true);

	wxLogMessage("ApplyPresetToHead: preset='%s', shapes=%zu", presetName, headGameVerts.size());

	int shapesTouched = 0;
	for (auto& [meshName, gameVerts] : headGameVerts) {
		auto srcIt = shapeNifSource.find(meshName);
		if (srcIt == shapeNifSource.end())
			continue;

		TriFile* tri = GetOrLoadTriFile(srcIt->second);
		if (!tri)
			continue;
		if (!gls.GetMesh(meshName))
			continue;

		auto nameIt = headShapeNifName_.find(meshName);
		if (nameIt == headShapeNifName_.end())
			continue;
		const std::string& nifShapeName = nameIt->second;

		auto allMorphs = tri->GetMorphs();
		auto morphIt = allMorphs.find(nifShapeName);
		if (morphIt == allMorphs.end())
			continue;

		std::vector<Vector3> verts = gameVerts;
		int applied = 0;
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

		wxLogMessage("  head shape '%s' (nif '%s'): %d morphs applied", meshName, nifShapeName, applied);
		gls.Update(meshName, &verts, nullptr);
		if (applied > 0)
			++shapesTouched;
	}
	wxLogMessage("ApplyPresetToHead: %d/%zu shapes had matching morphs", shapesTouched, headGameVerts.size());

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
	ApplyPresetToHead(currentPresetName);

	actorAnim_.RestPoseChanged();
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
		ApplyPresetToHead(currentPresetName);
		actorAnim_.RestPoseChanged();
		// Mesh set unchanged — just sync checkbox states
		SyncMeshOverlayStates();
		gls.RenderOneFrame();
		return;
	}

	// No preset active (or no morph maps) — full reload of body and outfit needed
	// for correct weight variant NIFs.
	if (outfitList) {
		wxTreeItemId selId = outfitList->GetSelection();
		if (selId.IsOk()) {
			auto* d = dynamic_cast<LLTreeItemData*>(outfitList->GetItemData(selId));
			if (d && d->kind == LLTreeItemData::Kind::Outfit && d->topIndex >= 0 && d->topIndex < static_cast<int>(filteredOutfits.size())) {
				LoadOutfitMeshes(*filteredOutfits[d->topIndex]);
				gls.RenderOneFrame();
				return;
			}
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

void LeveledListPreviewer::OnOutfitSelected(wxTreeEvent& event) {
	wxTreeItemId id = event.GetItem();
	if (!id.IsOk())
		return;
	auto* d = dynamic_cast<LLTreeItemData*>(outfitList->GetItemData(id));
	if (!d)
		return;

	if (d->kind == LLTreeItemData::Kind::Outfit) {
		if (d->topIndex < 0 || d->topIndex >= static_cast<int>(filteredOutfits.size()))
			return;
		if (d->unavailable)
			return;
		wxLogMessage("OnOutfitSelected: index=%d name='%s' isLoadingESP=%d", d->topIndex, filteredOutfits[d->topIndex]->name.c_str(), isLoadingESP_ ? 1 : 0);
		Config.SetValue("LLPreviewer/LastOutfit", filteredOutfits[d->topIndex]->name);
		LoadOutfitMeshes(*filteredOutfits[d->topIndex]);
		// After the outfit loads, surface its variant gate tree as children.
		PopulateVariantSubtree(outfitList, id);
		outfitList->Expand(id);
		return;
	}

	if (d->kind == LLTreeItemData::Kind::Variant) {
		if (d->unavailable)
			return;
		ActivateVariantChain(d->groupId, d->variantIdx, /*descend*/ true);
		return;
	}

	if (d->kind == LLTreeItemData::Kind::Group) {
		for (auto& g : useAnyGroups_) {
			if (g.groupId == d->groupId) {
				if (!g.variants.empty())
					ActivateVariantChain(d->groupId, g.variants.front().variantIdx, /*descend*/ true);
				break;
			}
		}
	}
}

void LeveledListPreviewer::LoadOutfitMeshes(const lldata::OutfitEntry& outfit) {
	wxLogMessage("LoadOutfitMeshes: outfit='%s' pieces=%zu showBody=%d", outfit.name.c_str(), outfit.pieces.size(), showBody ? 1 : 0);
	currentOutfit = &outfit;

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
		for (auto it = shapeArmoBodySlots.begin(); it != shapeArmoBodySlots.end();) {
			if (isNotHead(it->first))
				it = shapeArmoBodySlots.erase(it);
			else
				++it;
		}

		untexturedShapes.erase(std::remove_if(untexturedShapes.begin(), untexturedShapes.end(), isNotHead), untexturedShapes.end());

		bodyShapeNames.clear();
		outfitShapeNames.clear();
		outfitGameVerts.clear();
		outfitShapeNifName_.clear();
		useAnyGroups_.clear();
		shapeUseAnyPath_.clear();
		variantNifLoadFailures_.clear();
		loadedOutfitPieces_.clear();
		renderPieceIdx_.clear();
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

	// Determine the selected NPC's race, if any. ARMOs that contain multiple
	// ARMAs targeting different races (one ARMA per race) need to be filtered
	// to the ARMA that matches the NPC's race — otherwise we render every
	// race variant on top of each other.
	uint32_t npcRaceFid = 0;
	std::set<lldata::FormId> effectiveArmorRaces;
	if (!currentHeadEditorId.empty()) {
		auto effRace = data.GetEffectiveNpcRace(currentHeadEditorId);
		npcRaceFid = effRace.formId;
		// "Effective" set covers custom child / variant races that inherit
		// armor compatibility from a parent via the skin ARMO's ARMAs.
		effectiveArmorRaces = data.GetEffectiveArmorRaces(npcRaceFid);
	}
	if (npcRaceFid != 0) {
		std::string ras;
		for (lldata::FormId r : effectiveArmorRaces) {
			if (!ras.empty())
				ras += ",";
			char buf[24];
			std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(r));
			ras += buf;
		}
		wxLogMessage("LeveledListPreviewer: NPC race %08X effective armor races: [%s]", npcRaceFid, ras);
	}

	// Build the set of piece indices (into outfit.pieces) we will actually
	// render. For each ARMO we pick the ARMA tier that best matches the NPC's
	// race, using the same priority the engine uses:
	//   1. ARMAs whose race (primary or additional) is exactly the NPC's race
	//   2. ARMAs whose race (primary or additional) is DefaultRace (0x19) —
	//      these are the catch-all human variants used when no race-specific
	//      ARMA exists
	//   3. ARMAs whose race is reachable via the NPC race's skin-ARMO chain
	//      (covers custom child / variant races that inherit armor
	//      compatibility from a parent race)
	//   4. Fallback: keep everything (no race info available at all)
	// This prevents e.g. the Argonian / Khajiit gauntlet variants from being
	// rendered on a Redguard NPC just because they share the broad humanoid
	// effective-race set.
	constexpr uint32_t DEFAULT_RACE_FID = 0x00000019;
	renderPieceIdx_.clear();
	{
		auto pieceMatchesAny = [](const lldata::OutfitPiece& p, const std::set<lldata::FormId>& set) -> bool {
			if (set.count(p.armaRaceFormId))
				return true;
			for (lldata::FormId r : p.armaAdditionalRaceFormIds) {
				if (set.count(r))
					return true;
			}
			return false;
		};

		std::set<lldata::FormId> exactRaceSet;
		if (npcRaceFid != 0)
			exactRaceSet.insert(npcRaceFid);
		std::set<lldata::FormId> defaultRaceSet{DEFAULT_RACE_FID};

		std::map<lldata::FormId, std::vector<size_t>> byArmo;
		for (size_t i = 0; i < outfit.pieces.size(); ++i)
			byArmo[outfit.pieces[i].formId].push_back(i);

		for (auto& [armoId, indices] : byArmo) {
			// Try each tier in priority order; first tier that finds a match
			// wins for this ARMO and all non-matching ARMA variants get hidden.
			auto tryTier = [&](const std::set<lldata::FormId>& tierSet, const char* tierLabel) -> bool {
				if (tierSet.empty())
					return false;
				bool anyMatch = false;
				for (size_t i : indices) {
					if (pieceMatchesAny(outfit.pieces[i], tierSet)) {
						anyMatch = true;
						break;
					}
				}
				if (!anyMatch)
					return false;
				for (size_t i : indices) {
					if (pieceMatchesAny(outfit.pieces[i], tierSet))
						renderPieceIdx_.insert(i);
					else
						wxLogMessage("  Hiding ARMA variant of piece '%s' [%08X]: ARMA race %08X not in %s tier",
									 outfit.pieces[i].name,
									 outfit.pieces[i].formId,
									 outfit.pieces[i].armaRaceFormId,
									 tierLabel);
				}
				return true;
			};

			if (tryTier(exactRaceSet, "exact NPC race"))
				continue;
			if (tryTier(defaultRaceSet, "DefaultRace"))
				continue;
			if (tryTier(effectiveArmorRaces, "skin-ARMO race chain"))
				continue;
			// No tier matched (or no race info): keep everything.
			for (size_t i : indices)
				renderPieceIdx_.insert(i);
		}
	}

	// Build the "Use Any" variant group state from the OutfitEntry's pre-built
	// gate tree BEFORE loading any piece NIFs. Loading only walks pieces on
	// the active gate path (default activeIndex = 0 everywhere); other variants
	// are loaded lazily when the user switches to them via the tree UI. This
	// keeps initial-load cost bounded when an OTFT/LVLI chain has expanded to
	// thousands of mod-injected pieces (dungeon-filler NPCs whose DOFT
	// references the heavily-patched generic bandit-gear leveled lists).
	useAnyGroups_.clear();
	for (auto& gi : outfit.useAnyGroups) {
		UseAnyGroup group;
		group.groupId = gi.groupId;
		group.activeIndex = 0;
		group.label = gi.label;
		group.parentGroupId = gi.parentGroupId;
		group.parentVariantIdx = gi.parentVariantIdx;
		for (auto& vi : gi.variants) {
			UseAnyVariant variant;
			variant.variantIdx = vi.variantIdx;
			variant.label = !vi.label.empty() ? vi.label : ("Variant " + std::to_string(vi.variantIdx));
			variant.resolved = vi.resolved;
			variant.diagnostic = vi.diagnostic;
			group.variants.push_back(std::move(variant));
		}
		useAnyGroups_.push_back(std::move(group));
	}

	// Walk all eligible pieces and load the ones whose gates are currently
	// active. EnsurePiecesLoadedForActivePath records each loaded piece in
	// loadedOutfitPieces_ so the lazy-load path on variant switch skips it.
	EnsurePiecesLoadedForActivePath();

	// Apply initial visibility for every gated shape.
	ApplyUseAnyVisibility();

	for (auto& g : useAnyGroups_) {
		wxLogMessage("  Use Any group %d ('%s', parent=%d/%d): %zu variants", g.groupId, g.label, g.parentGroupId, g.parentVariantIdx, g.variants.size());
	}

	// Apply preset to outfit pieces
	if (!outfitGameVerts.empty() && !currentPresetName.empty() && currentPresetName != "(none)")
		ApplyPresetToOutfit(currentPresetName, true);

	actorAnim_.RestPoseChanged();

	// Head mesh is preserved across outfit changes — only reload if not yet loaded
	if (headShapeNames.empty() && !currentHeadEditorId.empty()) {
		auto& npcs = data.GetNPCs();
		for (auto& npc : npcs) {
			if (npc.editorId == currentHeadEditorId) {
				LoadHeadMesh(npc);

				// Resolve per-slot NPC skin + QNAM tint if not yet done
				if (!hasNpcSkinTextures && !hasNpcTint) {
					npcBodyPartTextures = data.ResolveNpcBodyPartTextures(npc.editorId);
					hasNpcSkinTextures = !npcBodyPartTextures.body[0].empty() || !npcBodyPartTextures.hands[0].empty() || !npcBodyPartTextures.feet[0].empty();
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
							shapeArmoBodySlots.erase(name);
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
	SetStatusText(wxString::Format(_("Outfit: %s — %zu meshes loaded"), outfit.name, outfitShapeNames.size()));

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

	group.activeIndex = newVariantIdx;
	wxLogMessage("  Switched Use Any group %d to variant %d ('%s')", group.groupId, newVariantIdx, group.variants[newVariantIdx].label);

	// The newly active variant's pieces may not have been loaded yet (initial
	// outfit load only walks the all-zero gate path). Pull them in now so
	// visibility flipping has shapes to act on.
	EnsurePiecesLoadedForActivePath();

	// Re-evaluate visibility for every gated shape — switching this group may
	// have also (de)activated dependent child groups deeper in the tree.
	ApplyUseAnyVisibility();

	// Use exactly the same algorithm as initial load for re-calculating slots and visibility
	UpdateActiveSlotsAndVisibility();

	gls.RenderOneFrame();
	RefreshMeshOverlay();
}

void LeveledListPreviewer::ActivateVariantChain(int groupId, int variantIdx, bool descend) {
	// Walk up ancestors activating the variant slot that gates each group.
	// `seen` guards against malformed cycles.
	std::set<int> seen;
	int curGid = groupId;
	int curVid = variantIdx;
	while (curGid >= 0 && seen.insert(curGid).second) {
		// Find group by id.
		size_t gi = 0;
		for (; gi < useAnyGroups_.size(); ++gi)
			if (useAnyGroups_[gi].groupId == curGid)
				break;
		if (gi >= useAnyGroups_.size())
			break;
		auto& group = useAnyGroups_[gi];
		// Translate variantIdx → index into group.variants vector.
		int newIdx = -1;
		for (size_t i = 0; i < group.variants.size(); ++i) {
			if (group.variants[i].variantIdx == curVid) {
				newIdx = static_cast<int>(i);
				break;
			}
		}
		if (newIdx >= 0 && newIdx != group.activeIndex) {
			group.activeIndex = newIdx;
			wxLogMessage("  Tree activated group %d variant %d ('%s')", group.groupId, curVid, group.variants[newIdx].label);
		}
		// Step up.
		curGid = group.parentGroupId;
		curVid = group.parentVariantIdx;
	}

	if (descend) {
		// For every group whose parent variant is now active, ensure its
		// activeIndex stays at a valid slot (default to 0 if unset).
		// Iterate to a fixed point — a newly activated child may itself enable
		// further descendants.
		bool changed = true;
		int guard = 0;
		while (changed && guard++ < 64) {
			changed = false;
			for (auto& g : useAnyGroups_) {
				if (g.parentGroupId < 0)
					continue;
				// Find parent group.
				const UseAnyGroup* parent = nullptr;
				for (auto& cand : useAnyGroups_) {
					if (cand.groupId == g.parentGroupId) {
						parent = &cand;
						break;
					}
				}
				if (!parent)
					continue;
				if (parent->activeIndex < 0 || parent->activeIndex >= static_cast<int>(parent->variants.size()))
					continue;
				if (parent->variants[parent->activeIndex].variantIdx != g.parentVariantIdx)
					continue;
				// This child group is reachable. If activeIndex is out of
				// range or currently 0 and that's also valid, nothing to do.
				if (g.activeIndex < 0 || g.activeIndex >= static_cast<int>(g.variants.size())) {
					if (!g.variants.empty()) {
						g.activeIndex = 0;
						changed = true;
					}
				}
			}
		}
	}

	// Lazy-load any piece whose gates just opened.
	EnsurePiecesLoadedForActivePath();

	ApplyUseAnyVisibility();
	UpdateActiveSlotsAndVisibility();
	gls.RenderOneFrame();
	RefreshMeshOverlay();
}

void LeveledListPreviewer::PopulateVariantSubtree(wxTreeCtrl* tree, wxTreeItemId parent) {
	if (!tree || !parent.IsOk())
		return;

	// Remove any previously-built variant children so repeated selects re-sync
	// with the freshly-loaded outfit.
	tree->DeleteChildren(parent);
	if (useAnyGroups_.empty())
		return;

	// Precompute "does any renderable piece exist on this gate slot?" to drive
	// the greyed-out status. A variant slot counts as available when at least
	// one race-tier-eligible piece with a NIF path walks through (groupId,
	// variantIdx). We deliberately read from `currentOutfit->pieces` (every
	// variant's pieces, loaded or not) rather than `shapeUseAnyPath_` (only
	// loaded shapes) because pieces on inactive variants are lazy-loaded and
	// wouldn't yet have registered any shape.
	std::set<std::pair<int, int>> slotsWithShapes;
	if (currentOutfit) {
		for (size_t i = 0; i < currentOutfit->pieces.size(); ++i) {
			auto& p = currentOutfit->pieces[i];
			if (p.nifPath.empty())
				continue;
			if (!renderPieceIdx_.empty() && !renderPieceIdx_.count(i))
				continue;
			for (auto& step : p.useAnyPath)
				slotsWithShapes.insert(step);
		}
	}

	// Build adjacency: children of each (groupId, variantIdx) slot.
	std::map<std::pair<int, int>, std::vector<const UseAnyGroup*>> childrenByGateSlot;
	std::vector<const UseAnyGroup*> topLevel;
	for (auto& g : useAnyGroups_) {
		if (g.parentGroupId < 0)
			topLevel.push_back(&g);
		else
			childrenByGateSlot[{g.parentGroupId, g.parentVariantIdx}].push_back(&g);
	}

	std::function<void(wxTreeItemId, const UseAnyGroup*)> addGroup;
	addGroup = [&](wxTreeItemId parentNode, const UseAnyGroup* g) {
		std::string groupLabel = !g->label.empty() ? g->label : ("Group " + std::to_string(g->groupId));
		auto* gdata = new LLTreeItemData();
		gdata->kind = LLTreeItemData::Kind::Group;
		gdata->groupId = g->groupId;
		wxTreeItemId groupNode = tree->AppendItem(parentNode, wxString::FromUTF8(groupLabel), -1, -1, gdata);

		for (size_t vi = 0; vi < g->variants.size(); ++vi) {
			auto& v = g->variants[vi];
			auto* vdata = new LLTreeItemData();
			vdata->kind = LLTreeItemData::Kind::Variant;
			vdata->groupId = g->groupId;
			vdata->variantIdx = v.variantIdx;

			// Renderability: variant is unavailable when no piece in the
			// resolved outfit walks through this gate slot. Sub-LVLI gates
			// don't carry their own shapes; for those we check whether ANY
			// descendant has shapes.
			bool hasShape = slotsWithShapes.count({g->groupId, v.variantIdx}) > 0;
			if (!hasShape) {
				// Walk descendants — gates that recurse may still produce
				// something downstream.
				std::function<bool(int, int)> anyShapeBelow = [&](int gid, int vid) -> bool {
					auto cit = childrenByGateSlot.find({gid, vid});
					if (cit == childrenByGateSlot.end())
						return false;
					for (const UseAnyGroup* child : cit->second) {
						for (auto& cv : child->variants) {
							if (slotsWithShapes.count({child->groupId, cv.variantIdx}))
								return true;
							if (anyShapeBelow(child->groupId, cv.variantIdx))
								return true;
						}
					}
					return false;
				};
				hasShape = anyShapeBelow(g->groupId, v.variantIdx);
			}
			vdata->unavailable = !hasShape;
			if (!hasShape) {
				// Collect every diagnostic in this subtree (the variant itself
				// plus all descendants reachable through nested gates). Each
				// problem-variant — unresolved ref, resolved-but-zero-pieces,
				// or piece-with-missing-NIF — carries its own diagnostic.
				std::function<void(int, int, std::set<std::string>&)> collectDiags = [&](int gid, int vid, std::set<std::string>& bag) {
					// NIF-load failures recorded at piece-load time.
					auto nifIt = variantNifLoadFailures_.find({gid, vid});
					if (nifIt != variantNifLoadFailures_.end()) {
						for (auto& missingPath : nifIt->second)
							bag.insert("NIF file not found on disk: " + missingPath);
					}
					auto cit = childrenByGateSlot.find({gid, vid});
					if (cit == childrenByGateSlot.end())
						return;
					for (const UseAnyGroup* child : cit->second) {
						for (auto& cv : child->variants) {
							if (!cv.diagnostic.empty())
								bag.insert(cv.diagnostic);
							collectDiags(child->groupId, cv.variantIdx, bag);
						}
					}
				};
				std::set<std::string> diags;
				if (!v.diagnostic.empty())
					diags.insert(v.diagnostic);
				collectDiags(g->groupId, v.variantIdx, diags);

				if (diags.empty()) {
					vdata->tooltip = "This variant has no resolvable pieces, but no per-variant diagnostic was captured. Likely cause: a piece was produced and a shape was registered, yet the shape didn't reach this variant's gate path (a tagging bug in shape registration).";
				}
				else if (diags.size() == 1) {
					vdata->tooltip = *diags.begin();
				}
				else {
					std::string aggregated = std::to_string(diags.size()) + " distinct problem(s) under this variant:\n";
					int n = 0;
					for (auto& d : diags) {
						if (++n > 6) {
							aggregated += "  • (and " + std::to_string(diags.size() - 6) + " more)";
							break;
						}
						aggregated += "  • " + d + "\n";
					}
					vdata->tooltip = aggregated;
				}
			}

			wxTreeItemId varNode = tree->AppendItem(groupNode, wxString::FromUTF8(v.label), -1, -1, vdata);
			if (vdata->unavailable)
				tree->SetItemTextColour(varNode, wxColour(150, 150, 150));
			if (static_cast<int>(vi) == g->activeIndex)
				tree->SetItemBold(varNode, true);

			auto cit = childrenByGateSlot.find({g->groupId, v.variantIdx});
			if (cit != childrenByGateSlot.end())
				for (const UseAnyGroup* child : cit->second)
					addGroup(varNode, child);
		}
		tree->Expand(groupNode);
	};

	for (const UseAnyGroup* g : topLevel)
		addGroup(parent, g);
}

void LeveledListPreviewer::OnTreeItemTooltip(wxTreeEvent& event) {
	wxTreeCtrl* tree = dynamic_cast<wxTreeCtrl*>(event.GetEventObject());
	if (!tree)
		return;
	wxTreeItemId id = event.GetItem();
	if (!id.IsOk())
		return;
	auto* d = dynamic_cast<LLTreeItemData*>(tree->GetItemData(id));
	if (!d || d->tooltip.empty())
		return;
	event.SetToolTip(wxString::FromUTF8(d->tooltip));
}

bool LeveledListPreviewer::IsUseAnyPathActive(const std::vector<std::pair<int, int>>& path) const {
	// Index lookup is linear (typical group count is small — order of magnitude
	// of LVLI depth, single digits in practice).
	for (auto& [gid, vid] : path) {
		const UseAnyGroup* g = nullptr;
		for (auto& cand : useAnyGroups_) {
			if (cand.groupId == gid) {
				g = &cand;
				break;
			}
		}
		if (!g)
			return false;
		if (g->activeIndex < 0 || g->activeIndex >= static_cast<int>(g->variants.size()))
			return false;
		if (g->variants[g->activeIndex].variantIdx != vid)
			return false;
	}
	return true;
}

bool LeveledListPreviewer::IsUseAnyPathActiveOrDefault(const std::vector<std::pair<int, int>>& path) const {
	if (path.empty())
		return true;
	if (useAnyGroups_.empty()) {
		// First-time load: useAnyGroups_ is populated only after the initial
		// piece sweep finishes, so during that sweep every gate's activeIndex
		// is conceptually 0. A piece is "active" iff its path is all-zero.
		for (auto& [gid, vid] : path) {
			(void)gid;
			if (vid != 0)
				return false;
		}
		return true;
	}
	return IsUseAnyPathActive(path);
}

void LeveledListPreviewer::EnsurePiecesLoadedForActivePath() {
	if (!currentOutfit)
		return;

	int loadedCount = static_cast<int>(outfitShapeNames.size());
	bool anyNew = false;
	for (size_t i = 0; i < currentOutfit->pieces.size(); ++i) {
		if (loadedOutfitPieces_.count(i))
			continue;
		if (!renderPieceIdx_.count(i))
			continue;
		if (!IsUseAnyPathActiveOrDefault(currentOutfit->pieces[i].useAnyPath))
			continue;
		LoadOutfitPieceShapes(*currentOutfit, i, /*baseGamePath*/ std::string{}, loadedCount);
		anyNew = true;
	}

	if (anyNew) {
		// Newly loaded shapes need the same downstream treatment a fresh outfit
		// load gets: re-skin body-chain bundled shapes from the NPC, re-apply
		// the active BodySlide preset morphs, and refresh the overlay panel.
		// SMP toggle / simulator sync is handled by the caller path that
		// invoked the variant switch (which already pumps these where needed).
		ReconcileBodyAndOutfitShapes();
		if (!outfitGameVerts.empty() && !currentPresetName.empty() && currentPresetName != "(none)")
			ApplyPresetToOutfit(currentPresetName, true);
	}
}

void LeveledListPreviewer::LoadOutfitPieceShapes(const lldata::OutfitEntry& outfit, size_t pieceIdx, const std::string& /*baseGamePath*/, int& loadedCount) {
	// Mark the piece as loaded up-front so a partial load (e.g. unresolved NIF)
	// still prevents endless re-attempts on subsequent variant switches.
	loadedOutfitPieces_.insert(pieceIdx);

	auto& piece = outfit.pieces[pieceIdx];
	if (piece.nifPath.empty()) {
		wxLogMessage("  Piece '%s' [%08X]: no model path, skipping", piece.name, piece.formId);
		return;
	}

	wxLogMessage("  Piece '%s' [%08X]: nifPath=%s (path depth=%zu)", piece.name, piece.formId, piece.nifPath, piece.useAnyPath.size());

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
		// Record the missing NIF against every gate slot in the piece's
		// useAnyPath so the variant-tree tooltip can attribute the failure
		// to the right (groupId, variantIdx). Empty path means an
		// unconditional piece (no gating), nothing to record.
		for (auto& slot : piece.useAnyPath)
			variantNifLoadFailures_[slot].insert(piecePath);
		return;
	}

	wxLogMessage("  Resolved to: %s", resolvedPath);

	// Load NIF file
	NifFile nif;
	std::string fullPath = resolvedPath;
	bool nifOk = false;

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
					if (nif.Load(stream) == 0) {
						nifOk = true;
						break;
					}
				}
			}
		}
		if (!nifOk)
			return; // Could not load from archive
	}
	else {
		// Loose file
		if (nif.Load(fullPath) != 0)
			return;
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

	std::vector<std::pair<NiShape*, Mesh*>> modelMeshes;
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
			// Track for the "Show untextured" toggle but do NOT hide the
			// shape: missing textures should be visible as the default
			// untextured material (mirrors BodySlide's preview behaviour).
			untexturedShapes.push_back(m->shapeName);
		}

		// Hide non-rendered helper shapes (SMP / collision / cloth-driver
		// rigging meshes). The game uses two independent gates and we
		// mirror both:
		//   1. Shader property absent — authors strip the shader so the
		//      shape never reaches the render pipeline.
		//   2. `BSDismemberSkinInstance` present AND all biped-range
		//      partitions (30..61) are outside the ARMA's BOD2/BODT
		//      flag set. The engine only consults dismember partitions
		//      when the shape carries one; shapes using plain
		//      `NiSkinInstance` (no partitions) bypass this gate and
		//      always render. This is why COCO Mysterious Mage shows
		//      every shape (all use plain `NiSkinInstance`) while
		//      Paragon Fey Lower's collision is hidden (uses
		//      `BSDismemberSkinInstance` with partition 32, which is
		//      not in the ARMA flags).
		// A shape is hidden when EITHER gate trips.
		bool hideShape = false;
		std::string hideReason;
		if (auto* nifShape = nif.FindBlockByName<NiShape>(shapeName)) {
			modelMeshes.emplace_back(nifShape, m);

			NiShader* shader = nif.GetShader(nifShape);
			if (!shader) {
				hideShape = true;
				hideReason = "no shader property";
			}
			else if (!piece.armaBodySlots.empty() && nifShape->HasSkinInstance()) {
				auto* skinRef = nifShape->SkinInstanceRef();
				BSDismemberSkinInstance* dismember = skinRef ? nif.GetHeader().GetBlock<BSDismemberSkinInstance>(*skinRef) : nullptr;
				if (dismember) {
					NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
					std::vector<int> triParts;
					if (nif.GetShapePartitions(nifShape, partInfo, triParts)) {
						std::set<int> armaSet(piece.armaBodySlots.begin(), piece.armaBodySlots.end());
						bool hasBipedPart = false;
						bool anyMatch = false;
						for (auto& pi : partInfo) {
							uint16_t p = pi.partID;
							if (p >= 30 && p <= 61) {
								hasBipedPart = true;
								if (armaSet.count(static_cast<int>(p))) {
									anyMatch = true;
									break;
								}
							}
						}
						if (hasBipedPart && !anyMatch) {
							hideShape = true;
							hideReason = "biped partitions not in ARMA slots";
						}
					}
				}
			}
		}
		if (hideShape) {
			wxLogMessage("  Hiding outfit shape '%s' (%s)", m->shapeName, hideReason);
			gls.SetMeshVisibility(m->shapeName, false);
		}
		if (std::find(outfitShapeNames.begin(), outfitShapeNames.end(), m->shapeName) == outfitShapeNames.end())
			outfitShapeNames.push_back(m->shapeName);
		shapeNifSource[m->shapeName] = piecePath;
		shapeArmoName[m->shapeName] = piece.name;
		shapeArmoBodySlots[m->shapeName] = piece.bodySlots;
		// Register this shape's full gate path so visibility evaluation can
		// AND across the chain. Empty path → unconditional, no entry needed.
		if (!piece.useAnyPath.empty())
			shapeUseAnyPath_[m->shapeName] = piece.useAnyPath;
		++loadedCount;

		// In Skyrim, NIF dismember partitions drive dismemberment effects
		// (decapitation, severed limbs) — they do NOT gate rendering. The
		// engine renders every shape in the equipped ARMA's NIF regardless
		// of whether its partition is in the ARMO's biped slot mask.
		//
		// The one exception we still want to mirror is "ARMO bundles a
		// duplicate body / hands / feet shape" (common in cloaks, hoods,
		// helmets that ship a rigged-reference body). Those would double
		// up with the body/hands/feet NIFs we load separately for the NPC
		// or default character. So we hide an outfit shape ONLY when:
		//   * its partition is one of the canonical body-slot IDs
		//     (32 = Body, 33 = Hands, 37 = Feet), AND
		//   * the piece's ARMO doesn't itself declare that body slot
		//     (i.e. it's not legitimately a body-replacing armor).
		// Other partition IDs (calf 38, head 30/31, helmet 42, custom
		// 130+ etc.) are dismember markers, not body bundles, so we leave
		// those shapes visible.
		if (auto* nifShape = nif.FindBlockByName<NiShape>(shapeName)) {
			NiVector<BSDismemberSkinInstance::PartitionInfo> partInfo;
			std::vector<int> triParts;
			if (nif.GetShapePartitions(nifShape, partInfo, triParts)) {
				std::set<uint16_t> partIds;
				std::string partList;
				for (size_t i = 0; i < partInfo.size(); ++i) {
					partIds.insert(partInfo[i].partID);
					if (!partList.empty())
						partList += ",";
					partList += std::to_string(partInfo[i].partID);
				}
				if (!partIds.empty()) {
					bodyShapePartMap[m->shapeName] = partIds;
					wxLogMessage("  Outfit shape '%s' partitions: [%s]", m->shapeName, partList);
				}
			}

			// Record the shape's Skin Tint flag (BSLightingShaderProperty).
			// Outfit shapes marked skin-tinted receive the NPC's slot-resolved
			// NAM1 skin TXST + QNAM tint in ReconcileBodyAndOutfitShapes; all
			// other outfit shapes keep their NIF-embedded textures.
			NiShader* outfitShader = nif.GetShader(nifShape);
			bool outfitSkinTinted = outfitShader && outfitShader->IsSkinTinted();
			shapeIsSkinTinted[m->shapeName] = outfitSkinTinted;
			if (outfitSkinTinted)
				wxLogMessage("  Outfit shape '%s' is skin-tinted", m->shapeName);
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

	// The actor wears the piece while its Use Any variants are active.
	fsmp::WornItem item;
	item.formId = piece.formId;
	item.name = piece.name;
	for (int slot : piece.bodySlots) {
		if (slot >= 30 && slot < 62)
			item.bipedSlots |= 1u << (slot - 30);
	}
	actorAnim_.RegisterModel(nif, piecePath, ActorAnimation::ModelKind::Armor, modelMeshes, [this, path = piece.useAnyPath]() {
		return path.empty() || IsUseAnyPathActive(path);
	}, item);
}

void LeveledListPreviewer::ApplyUseAnyVisibility() {
	std::set<std::string> untexturedSet(untexturedShapes.begin(), untexturedShapes.end());
	for (auto& [shapeName, path] : shapeUseAnyPath_) {
		bool active = IsUseAnyPathActive(path);
		// Respect the untextured-meshes toggle: a shape with no valid texture
		// stays hidden when `showUntextured` is off, even if its gate is open.
		if (active && !showUntextured && untexturedSet.count(shapeName))
			active = false;
		gls.SetMeshVisibility(shapeName, active);
	}
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
			if (base.find("Eye") != std::string::npos)
				return "Eyes (" + base + ")";
			if (base.find("Teeth") != std::string::npos || base.find("Mouth") != std::string::npos)
				return "Teeth (" + base + ")";
			if (base.find("Hair") != std::string::npos)
				return "Hair (" + base + ")";
			if (base.find("Brow") != std::string::npos)
				return "Brows (" + base + ")";
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
			if (it->second.count(33))
				handsOnly.push_back(n);
			else if (it->second.count(37))
				feetOnly.push_back(n);
			else
				bodyOnly.push_back(n);
		}
		else
			bodyOnly.push_back(n);
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

	// For outfit shapes, include non-gated shapes AND gated shapes whose full
	// path is currently active (so nested gates AND together — a shape behind
	// a Use Any LVLI whose chosen variant is itself another Use Any LVLI only
	// shows when both levels select it).
	std::vector<std::string> visibleOutfitShapes;
	for (auto& n : outfitShapeNames) {
		auto it = shapeUseAnyPath_.find(n);
		if (it == shapeUseAnyPath_.end()) {
			// Unconditional — always include.
			visibleOutfitShapes.push_back(n);
		}
		else if (IsUseAnyPathActive(it->second)) {
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
				// Re-evaluate race body / hair coverage now that the visible
				// outfit shape set changed (per-shape toggle may have just
				// uncovered or covered slots 31/32/33/37).
				ApplyRaceBodyPartVisibility();
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
						// Re-evaluate race body / hair visibility based on the
						// new set of visible outfit shapes.
						ApplyRaceBodyPartVisibility();
						gls.RenderOneFrame();
					});
					sizer->Add(cb, 0, wxLEFT | wxBOTTOM, 28);
					meshCheckboxes[entry.shapeName] = cb;
				}
			}
		}
	}

	// (Variant gate selection lives in the left-side outfit/NPC trees — the
	//  overlay is intentionally limited to per-shape enable/disable toggles.)

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
	// Re-hide race shapes covered by the currently visible outfit.
	ApplyRaceBodyPartVisibility();
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
	animRunning_ = false;
	smpRunning_ = false;
	actorTimer_.Stop();

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
		// Persist selected outfit name (column widths no longer apply — the
		// outfit list is now a wxTreeCtrl).
		wxTreeItemId selId = outfitList->GetSelection();
		if (selId.IsOk()) {
			auto* d = dynamic_cast<LLTreeItemData*>(outfitList->GetItemData(selId));
			if (d && d->kind == LLTreeItemData::Kind::Outfit && d->topIndex >= 0 && d->topIndex < static_cast<int>(filteredOutfits.size()))
				Config.SetValue("LLPreviewer/LastOutfit", filteredOutfits[d->topIndex]->name);
		}
	}

	// Persist head NPC, preset, and weight
	Config.SetValue("LLPreviewer/HeadEditorId", currentHeadEditorId);
	Config.SetValue("LLPreviewer/PresetName", currentPresetName);
	Config.SetBoolValue("LLPreviewer/HighWeight", useHighWeight);

	int ret = Config.SaveConfig(Config["AppDir"] + "/Config.xml");
	if (ret)
		wxLogWarning("LeveledListPreviewer: failed to save Config.xml (%d).", ret);

	actorAnim_.ForgetModels();
	actorAnim_.ClearActor();

	Destroy();
	if (eventHandler) {
		wxCommandEvent closeEvent(wxEVT_LEVELEDLIST_CLOSED);
		eventHandler->ProcessEvent(closeEvent);
	}
}

// ---------------------------------------------------------------------------
// Skeleton
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

// ---------------------------------------------------------------------------
// Actor motion: idle animation and FSMP physics
// ---------------------------------------------------------------------------

namespace {

// Data-relative model path ("meshes/...") back to the form a record stores it in.
std::string RecordModelPath(const std::string& meshesPath) {
	std::string path = meshesPath;
	if (path.compare(0, 7, "meshes/") == 0)
		path.erase(0, 7);
	std::replace(path.begin(), path.end(), '/', '\\');
	return path;
}

} // namespace

bool LeveledListPreviewer::LoadAnimationActor(bool& pending) {
	pending = false;
	lldata::LeveledListData::RaceAnimationFiles files;
	fsmp::ActorInfo info;

	if (!currentHeadEditorId.empty()) {
		const lldata::NPCEntry* npc = nullptr;
		for (auto& n : data.GetNPCs()) {
			if (n.editorId == currentHeadEditorId) {
				npc = &n;
				break;
			}
		}
		if (!npc) {
			// NPC records are loaded together with the first ESP.
			pending = data.GetNPCs().empty();
			if (!pending)
				wxLogError("LeveledListPreviewer: Actor: NPC '%s' not found", currentHeadEditorId);
			actorAnim_.ClearActor();
			return false;
		}

		auto race = data.GetEffectiveNpcRace(currentHeadEditorId);
		files = data.GetRaceAnimationFiles(race.formId, npc->isFemale);
		if (files.skeletonNif.empty()) {
			wxLogError("LeveledListPreviewer: Actor: race '%s' of NPC '%s' defines no %s skeleton (ANAM)", race.editorId, currentHeadEditorId, npc->isFemale ? "female" : "male");
			actorAnim_.ClearActor();
			return false;
		}
		if (files.behaviorProject.empty())
			wxLogError("LeveledListPreviewer: Actor: race '%s' of NPC '%s' defines no %s behavior graph (MODL); the idle animation is unavailable", race.editorId, currentHeadEditorId, npc->isFemale ? "female" : "male");

		info.npcFormId = npc->formId;
		info.name = npc->fullName;
		for (int sex = 0; sex < 2; sex++)
			info.raceSkeleton[sex] = RecordModelPath(data.GetRaceAnimationFiles(race.formId, sex == 1).skeletonNif);
	}
	else {
		// Without an NPC the outfit is shown on the vanilla female body (see
		// LoadBodyMeshes), so the vanilla female actor files drive it.
		files.skeletonNif = "meshes/actors/character/character assets female/skeleton_female.nif";
		files.behaviorProject = "meshes/actors/character/defaultfemale.hkx";
	}

	auto readFile = [this](const std::string& path, std::vector<char>& out) { return data.ReadGameFile(path, out); };

	// FSMP reads its configuration from the loose file in the game's Data folder.
	std::string baseGamePath = Config["GameDataPath"];
	if (!baseGamePath.empty() && baseGamePath.back() != '/' && baseGamePath.back() != '\\')
		baseGamePath += '/';
	std::string configsFile = lldata::LeveledListData::ResolveCaseInsensitive(baseGamePath, "SKSE/Plugins/hdtSkinnedMeshConfigs/configs.xml");
	ActorAnimation::InitialisePhysics(readFile, configsFile);

	bool loaded = actorAnim_.LoadActor(files.skeletonNif, files.behaviorProject, info, Config["AnimClip"], readFile);
	RefreshClipChoice();
	return loaded;
}

void LeveledListPreviewer::RefreshClipChoice() {
	if (!clipText_ || !clipButton_)
		return;
	clipText_->ChangeValue(wxString::FromUTF8(actorAnim_.ClipName()));
	clipButton_->Enable(!actorAnim_.ClipNames().empty());
}

void LeveledListPreviewer::OnChooseClip(wxCommandEvent& WXUNUSED(event)) {
	const auto& names = actorAnim_.ClipNames();
	if (names.empty())
		return;

	// A character lists hundreds of animations: a list with a search field.
	wxDialog dialog(this, wxID_ANY, _("Animation"), wxDefaultPosition, wxSize(520, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	auto* search = new wxSearchCtrl(&dialog, wxID_ANY);
	auto* list = new wxListBox(&dialog, wxID_ANY);
	sizer->Add(search, 0, wxEXPAND | wxALL, 5);
	sizer->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	sizer->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 5);
	dialog.SetSizer(sizer);

	const wxString current = wxString::FromUTF8(actorAnim_.ClipName());
	auto fill = [&]() {
		const wxString filter = search->GetValue().Lower();
		wxArrayString shown;
		for (auto& name : names) {
			wxString entry = wxString::FromUTF8(name);
			if (filter.empty() || entry.Lower().Contains(filter))
				shown.Add(entry);
		}
		list->Set(shown);
		list->SetStringSelection(current);
	};
	search->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { fill(); });
	list->Bind(wxEVT_LISTBOX_DCLICK, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
	fill();

	if (dialog.ShowModal() != wxID_OK || list->GetSelection() == wxNOT_FOUND)
		return;

	std::string name(list->GetStringSelection().ToUTF8());
	Config.SetValue("AnimClip", name);
	if (!actorAnim_.LoadClip(name))
		SetStatusText(_("The animation can't be played (see log)"));
	else if (!animRunning_ && animToggle_) {
		animToggle_->SetValue(true);
		animRunning_ = true;
		Config.SetValue("AnimEnabled", "true");
		UpdateActorMotion();
	}
	RefreshClipChoice();
}

void LeveledListPreviewer::OnTogglePause(wxCommandEvent& event) {
	actorAnim_.SetPaused(event.IsChecked());
}

void LeveledListPreviewer::UpdateActorMotion() {
	const bool running = animRunning_ || smpRunning_;
	if (running && !actorTimer_.IsRunning()) {
		animLastTick_ = std::chrono::steady_clock::now();
		actorTimer_.Start(16); // ~60 FPS
	}
	else if (!running && actorTimer_.IsRunning()) {
		actorTimer_.Stop();
		if (canvas && context)
			canvas->SetCurrent(*context);
		actorAnim_.QueueRestPoseUpload([this](const std::string& name) { return gls.GetMesh(name); });
		gls.RenderOneFrame();
	}

	if (animRunning_ && smpRunning_)
		SetStatusText(_("Idle animation and SMP physics active"));
	else if (animRunning_)
		SetStatusText(_("Idle animation active"));
	else if (smpRunning_)
		SetStatusText(_("SMP physics active"));
	else
		SetStatusText(_("Idle animation and SMP physics stopped"));
}

void LeveledListPreviewer::OnToggleSmp(wxCommandEvent& event) {
	Config.SetValue("SmpEnabled", event.IsChecked() ? "true" : "false");
	smpRunning_ = event.IsChecked();
	UpdateActorMotion();
}

void LeveledListPreviewer::OnToggleAnim(wxCommandEvent& event) {
	Config.SetValue("AnimEnabled", event.IsChecked() ? "true" : "false");
	animRunning_ = event.IsChecked();
	UpdateActorMotion();
}

void LeveledListPreviewer::OnActorTimer(wxTimerEvent& WXUNUSED(event)) {
	if ((!animRunning_ && !smpRunning_) || isLoadingESP_)
		return;

	auto getMesh = [this](const std::string& name) { return gls.GetMesh(name); };
	if (canvas && context)
		canvas->SetCurrent(*context);

	if (animActorDirty_ || animActorEditorId_ != currentHeadEditorId) {
		bool pending = false;
		bool ok = LoadAnimationActor(pending);
		if (!pending) {
			animActorDirty_ = false;
			animActorEditorId_ = currentHeadEditorId;
			if (!ok) {
				actorAnim_.QueueRestPoseUpload(getMesh);
				gls.RenderOneFrame();
				SetStatusText(_("The actor can't be set up (see log)"));
			}
			else if (animRunning_ && !actorAnim_.HasClip())
				SetStatusText(_("Idle animation unavailable for this actor (see log)"));
		}
		animLastTick_ = std::chrono::steady_clock::now();
	}
	if (!actorAnim_.IsLoaded())
		return;

	auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - animLastTick_).count();
	animLastTick_ = now;

	actorAnim_.Update(dt, animRunning_, smpRunning_, getMesh);
	gls.RenderOneFrame();
	actorAnim_.RestoreRestPose();
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
