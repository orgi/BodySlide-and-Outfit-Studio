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
#include <map>
#include <set>
#include <wx/combobox.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/textcompleter.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
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
	// shapeName → ARMO BOD2 slot list (e.g. {31} for hair, {32} for body).
	// Used by ApplyRaceBodyPartVisibility to hide race body parts / hair only
	// when a currently-visible outfit shape's piece actually claims that slot.
	// Mirrors `OutfitPiece.bodySlots` for each loaded shape of the piece.
	std::unordered_map<std::string, std::vector<int>> shapeArmoBodySlots;
	// Resolved texture file paths (slot 0 = diffuse, ..., 9 = MAX) per shape,
	// captured from BSShaderTextureSet + BGSM/BGEM + ESP TXST overrides at the
	// moment AddNifShapeTextures built the GLMaterial. Used by
	// ReconcileBodyAndOutfitShapes to retarget the race-skin textures onto
	// outfit-bundled body shapes when the outfit's NIF would otherwise use
	// vanilla / wrong textures.
	std::unordered_map<std::string, std::vector<std::string>> shapeTexFiles;
	std::set<std::string> expandedNifGroups; // NIF paths currently expanded in overlay

	// "Use Any" variant groups: only one variant per group is shown at a time.
	// Groups form a tree: a group whose parent gate is not currently active
	// is itself dormant. `UseAnyVariant::shapeNames` only lists the shapes
	// whose deepest gate is THIS variant — visibility evaluation walks the
	// full path stored on each piece so nested gates AND together.
	struct UseAnyVariant {
		int variantIdx = -1;				 // original variant index from ESP
		std::string label;					 // display name for this variant
		std::vector<std::string> shapeNames; // shapes whose deepest gate is this variant
		bool resolved = true;				 // mirrors OutfitVariantInfo::resolved
		std::string diagnostic;				 // populated when !resolved (FormID + decoded plugin slot)
	};
	struct UseAnyGroup {
		int groupId = -1;
		std::string label;		   // source LVLI editor id (from OutfitEntry::useAnyGroupNames)
		int parentGroupId = -1;	   // outer gate this group depends on (-1 = top-level)
		int parentVariantIdx = -1; // variant index in the parent group that activates this group
		std::vector<UseAnyVariant> variants;
		int activeIndex = 0; // which variant is currently shown
	};
	std::vector<UseAnyGroup> useAnyGroups_;
	// Shape name → full gating path (list of (groupId, variantIdx)). Empty path
	// means the shape is unconditional.
	std::unordered_map<std::string, std::vector<std::pair<int, int>>> shapeUseAnyPath_;

	// Per (groupId, variantIdx) record of NIF paths the loader couldn't find
	// on disk for pieces gated by this variant. Used by the variant tooltip
	// to explain "the piece resolved but its mesh file is missing" cases —
	// distinct from "the ARMO didn't resolve in cache". Cleared per outfit
	// load alongside useAnyGroups_.
	std::map<std::pair<int, int>, std::set<std::string>> variantNifLoadFailures_;

	// Body part tracking: shape name → set of NIF partition body part IDs
	// (e.g. 32=SBP_32_BODY, 33=SBP_33_HANDS, 37=SBP_37_FEET)
	std::unordered_map<std::string, std::set<uint16_t>> bodyShapePartMap;

	// Skin-tint marker: shape name → true if the shape's BSLightingShaderProperty
	// has Skin Tint enabled (bslspShaderType == BSLSP_SKINTINT). The game engine
	// intercepts these shapes and forces the NPC's NAM1 skin textures + QNAM tint
	// onto them; non-skin-tinted shapes keep their NIF-embedded BSShaderTextureSet.
	std::unordered_map<std::string, bool> shapeIsSkinTinted;

	// Slot the body-chain NIF was loaded for (32=body, 33=hands, 37=feet).
	// Outfit shapes are not in this map — their slot is derived from their
	// dismember partition at retexture time.
	std::unordered_map<std::string, int> bodyShapeSourceSlot;

	// UI controls
	wxSplitterWindow* splitter = nullptr;
	wxSearchCtrl* searchCtrl = nullptr;
	wxSpinCtrl* levelMinSpin = nullptr;
	wxSpinCtrl* levelMaxSpin = nullptr;
	wxTextCtrl* headCtrl = nullptr;		   // NPC head selector (with autocomplete)
	wxComboBox* presetCombo = nullptr;	   // BodySlide preset selector
	wxCheckBox* highWeightCheck = nullptr; // high (_1) vs low (_0) weight
	// Outfit tree (Outfits tab). Top-level nodes are LVLI/OTFT outfits; each
	// expands lazily into a variant gate tree mirroring the leveled-list
	// hierarchy of that outfit.
	wxTreeCtrl* outfitList = nullptr;
	// NPC mode (second notebook tab). The NPC tree is sorted by displayName
	// and lets the user pick any NPC; each NPC node expands into the variant
	// gate tree for that NPC's default outfit (DOFT). The selected NPC's
	// default outfit is rendered alongside the NPC's body / head.
	wxNotebook* viewNotebook = nullptr;
	wxTreeCtrl* npcList = nullptr;
	wxSearchCtrl* npcSearchCtrl = nullptr;
	wxButton* loadAllNpcsBtn = nullptr;
	wxCheckBox* npcHumanoidOnlyCheck = nullptr;
	wxCheckBox* npcFemaleOnlyCheck = nullptr;
	std::vector<size_t> filteredNpcIndices; // indices into data.GetNPCs()

	// Data
	lldata::LeveledListData data;
	const lldata::OutfitEntry* currentOutfit = nullptr;
	std::vector<const lldata::OutfitEntry*> filteredOutfits;
	wxString lastESPDirectory;
	std::string lastESPFilepath;
	std::unique_ptr<wxFileSystemWatcher> fsWatcher;
	// Re-entry guard: wxFileSystemWatcher can fire multiple events for a single
	// ESP rewrite, and wxLog::FlushActive() inside LoadOutfitMeshes pumps the
	// event loop. Without this guard a queued CallAfter could re-enter
	// LoadESPFile while LoadOutfitMeshes is still using the old OutfitEntry,
	// corrupting body/outfit shape state and hiding hands/feet.
	bool isLoadingESP_ = false;
	bool reloadPending_ = false;

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

	// Head preset morphing via .tri files. Keeps body and head necks aligned
	// when a BodySlide preset is applied: morphs whose name matches a slider
	// in the active preset are applied to the head shape's neck/upper-body
	// vertices. Heads typically only ship a small subset of slider morphs
	// (neck/shoulders); morphs the head .tri doesn't carry are silently
	// skipped, matching how the in-game BodySlide build pipeline behaves.
	std::unordered_map<std::string, std::vector<nifly::Vector3>> headGameVerts; // displayName ("_head_<shape>") → game verts
	std::unordered_map<std::string, std::string> headShapeNifName_;				 // displayName → original NIF shape name

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
	void OnOutfitSelected(wxTreeEvent& event);
	void OnTreeItemTooltip(wxTreeEvent& event);

	// NPC mode events
	void OnNpcSearchChanged(wxCommandEvent& event);
	void OnNpcSelected(wxTreeEvent& event);
	void OnLoadAllNpcs(wxCommandEvent& event);
	void OnViewTabChanged(wxBookCtrlEvent& event);

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
	void RefreshNpcList();
	// Apply an NPC's head + body skin + (optionally) default outfit. Shared
	// between OnNpcSelected and OnHeadEntered; the legacy head-selector path
	// passes loadDefaultOutfit=false because in Outfit mode the outfit is
	// driven by the outfit list, not by the NPC.
	void SelectNpc(const lldata::NPCEntry& npc, bool loadDefaultOutfit);
	void LoadOutfitMeshes(const lldata::OutfitEntry& outfit);
	bool AddNifShapeTextures(nifly::NifFile* nif, const std::string& shapeName, const std::vector<lldata::TextureOverride>* overrides = nullptr, const std::string& meshName = {});
	bool LoadNifFromPath(const std::string& relativePath, const std::string& prefix = "", int sourceSlot = 0);
	void LoadBodyMeshes();
	void LoadHeadMesh(const lldata::NPCEntry& npc);
	void ClearHeadMeshes();
	// Reconcile race-loaded body parts with outfit-bundled body shapes:
	// - Hide race body shapes whose dismember slot is already covered by an outfit shape.
	// - Apply the NPC's WNAM/race skin TXST to outfit body shapes that occupy body slots.
	void ReconcileBodyAndOutfitShapes();
	// Show/hide race-loaded body parts (body, hands, feet) and the NPC hair
	// based on which outfit shapes are CURRENTLY VISIBLE. Decisions follow
	// what is shown — not what the outfit declares — so toggling an outfit
	// piece's shape off via the overlay re-reveals the corresponding race
	// body part (and toggling it back on re-hides it).
	void ApplyRaceBodyPartVisibility();
	void UpdateHeadVisibility();
	void UpdateActiveSlotsAndVisibility();
	void LoadPresetList();
	void ApplyPresetToBody(const std::string& presetName, bool deferredRender = false);
	void ApplyPresetToOutfit(const std::string& presetName, bool deferredRender = false);
	void ApplyPresetToHead(const std::string& presetName, bool deferredRender = false);
	TriFile* GetOrLoadTriFile(const std::string& nifRelativePath);

	// Mesh overlay helpers
	void RefreshMeshOverlay();
	void SyncMeshOverlayStates();
	void RepositionMeshOverlay();
	void SwitchUseAnyVariant(size_t groupIdx, int newVariantIdx);
	// Activate the given (groupId, variantIdx) and recursively walk up the
	// ancestor chain, activating the parent variant that gates this group at
	// every level. Used by the tree-view variant selector so clicking any
	// node automatically selects the chain that makes that node visible.
	// `descend` true also defaults any sub-groups gated by the newly-active
	// variant to their first variant — matching the user's mental model of
	// "select a parent → first child becomes active".
	void ActivateVariantChain(int groupId, int variantIdx, bool descend);
	// Build (or rebuild) the variant gate subtree under a top-level outfit
	// or NPC node. Reads the currently-loaded outfit's `useAnyGroups_` and
	// `shapeUseAnyPath_` to derive per-variant renderability — variants that
	// resolve to no visible pieces are added as greyed-out items carrying a
	// tooltip explaining why. Removes any pre-existing children first.
	void PopulateVariantSubtree(wxTreeCtrl* tree, wxTreeItemId parent);
	// Walk every shape registered in shapeUseAnyPath_ and set its visibility
	// to AND(every gate in the path is currently selected). Honors the
	// untextured-meshes toggle and the per-shape user override so users can
	// still hide individual shapes manually.
	void ApplyUseAnyVisibility();
	// True iff every (groupId, variantIdx) in `path` matches the current
	// activeIndex of the corresponding group. Groups whose own parent gate is
	// inactive count as inactive (so a piece is hidden if any ancestor gate is
	// closed, regardless of the local gate's selection).
	bool IsUseAnyPathActive(const std::vector<std::pair<int, int>>& path) const;

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
		ID_NpcList,
		ID_NpcSearch,
		ID_LoadAllNpcs,
		ID_NpcHumanoidOnly,
		ID_NpcFemaleOnly,
		ID_ViewNotebook,
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
