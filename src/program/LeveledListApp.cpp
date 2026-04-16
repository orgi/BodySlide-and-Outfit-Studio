/*
Leveled List Viewer standalone app
*/

#include "LeveledListApp.h"
#include "../utils/PlatformUtil.h"
#include "../files/wxDDSImage.h"

#include <wx/intl.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/xrc/xmlres.h>
#include <wx/tokenzr.h>

#include <algorithm>

using namespace nifly;

ConfigurationManager Config;
ConfigurationManager BodySlideConfig;

const std::array<wxString, 10> TargetGames
	= {"Fallout3", "FalloutNewVegas", "Skyrim", "Fallout4", "SkyrimSpecialEdition", "Fallout4VR", "SkyrimVR", "Fallout76", "Oblivion", "Starfield"};
const std::array<wxLanguage, 37> SupportedLangs = {wxLANGUAGE_ENGLISH,	  wxLANGUAGE_AFRIKAANS,		   wxLANGUAGE_ARABIC,  wxLANGUAGE_CATALAN,	  wxLANGUAGE_CZECH,
												   wxLANGUAGE_DANISH,	  wxLANGUAGE_GERMAN,		   wxLANGUAGE_GREEK,   wxLANGUAGE_SPANISH,	  wxLANGUAGE_BASQUE,
												   wxLANGUAGE_FINNISH,	  wxLANGUAGE_FRENCH,		   wxLANGUAGE_HINDI,   wxLANGUAGE_HUNGARIAN,  wxLANGUAGE_INDONESIAN,
												   wxLANGUAGE_ITALIAN,	  wxLANGUAGE_JAPANESE,		   wxLANGUAGE_KOREAN,  wxLANGUAGE_LITHUANIAN, wxLANGUAGE_LATVIAN,
												   wxLANGUAGE_MALAY,	  wxLANGUAGE_NORWEGIAN_BOKMAL, wxLANGUAGE_NEPALI,  wxLANGUAGE_DUTCH,	  wxLANGUAGE_POLISH,
												   wxLANGUAGE_PORTUGUESE, wxLANGUAGE_ROMANIAN,		   wxLANGUAGE_RUSSIAN, wxLANGUAGE_SLOVAK,	  wxLANGUAGE_SLOVENIAN,
												   wxLANGUAGE_ALBANIAN,	  wxLANGUAGE_SWEDISH,		   wxLANGUAGE_TAMIL,   wxLANGUAGE_TURKISH,	  wxLANGUAGE_UKRAINIAN,
												   wxLANGUAGE_VIETNAMESE, wxLANGUAGE_CHINESE};

wxIMPLEMENT_APP(LeveledListApp);

LeveledListApp::~LeveledListApp() {
	delete locale;
	locale = nullptr;

	FSManager::del();
}

bool LeveledListApp::OnInit() {
	if (!wxApp::OnInit())
		return false;

	wxString dataDir;
#ifndef _WINDOWS
	if (!wxGetEnv("WX_BODYSLIDE_DATA_DIR", &dataDir)) {
		dataDir = wxGetCwd();
		if (!wxFileExists(dataDir + "/Config.xml")) {
			dataDir = wxPathOnly(wxStandardPaths::Get().GetExecutablePath());
			if (!wxFileExists(dataDir + "/Config.xml"))
				dataDir = wxStandardPaths::Get().GetDataDir();
		}
	}
#else
	dataDir = wxGetCwd();
#ifdef _DEBUG
	if (!wxFileExists(dataDir + "/Config.xml"))
		dataDir = wxPathOnly(wxStandardPaths::Get().GetExecutablePath());
#else
	if (!wxFileExists(dataDir + "/Config.xml")) {
		dataDir = wxPathOnly(wxStandardPaths::Get().GetExecutablePath());
		if (!wxFileExists(dataDir + "/Config.xml"))
			dataDir = wxStandardPaths::Get().GetDataDir();
	}
#endif
#endif
	std::string dataDirStr = dataDir.ToUTF8().data();

	int err1 = Config.LoadConfig(dataDirStr + "/Config.xml");
	int err2 = BodySlideConfig.LoadConfig(dataDirStr + "/BodySlide.xml", "BodySlideConfig");

	if (err1) wxLogWarning("Failed to load Config.xml from %s (error %d)", dataDirStr, err1);
	if (err2) wxLogWarning("Failed to load BodySlide.xml from %s (error %d)", dataDirStr, err2);

	Config.SetValue("AppDir", dataDirStr);

	logger.Initialize(Config.GetIntValue("LogLevel", -1), dataDir + "/Log_LLV.txt");
	wxLogMessage("Initializing Leveled List Viewer...");

#ifdef NDEBUG
	wxHandleFatalExceptions();
#endif

	wxString appDirUri = wxString::FromUTF8(dataDirStr);
	appDirUri.Replace("#", "%23");
	wxSetEnv("AppDir", appDirUri);

	wxXmlResource* xrc = wxXmlResource::Get();
	xrc->SetFlags(wxXRC_USE_LOCALE | wxXRC_USE_ENVVARS);
	xrc->InitAllHandlers();
	wxInitAllImageHandlers();
	wxImage::AddHandler(new wxDDSHandler);

	wxLogMessage("Data directory: %s", dataDirStr);

	if (!SetDefaultConfig())
		return false;

	InitLanguage();
	InitArchives();

	llPreviewer = new LeveledListPreviewer(this);
	llPreviewer->Bind(wxEVT_LEVELEDLIST_CLOSED, &LeveledListApp::OnPreviewerClosed, this);
	llPreviewer->Show();

	SetTopWindow(llPreviewer);

	return true;
}

void LeveledListApp::InitLanguage() {
	int langId = Config.GetIntValue("Language", 0);
	if (langId < 0 || langId >= SupportedLangs.size())
		langId = 0;

	if (SupportedLangs[langId] == wxLANGUAGE_ENGLISH) {
		delete locale;
		locale = nullptr;
		return;
	}

	if (!locale || locale->GetLanguage() != SupportedLangs[langId]) {
		delete locale;
		locale = new wxLocale();
		locale->Init(SupportedLangs[langId]);
		locale->AddCatalogLookupPathPrefix(wxString::FromUTF8(Config["AppDir"]) + "/lang");
		locale->AddCatalog("BodySlide");

		if (!locale->IsOk()) {
			delete locale;
			locale = nullptr;
		}
	}
}

void LeveledListApp::InitArchives() {
	std::string dataPath = Config["GameDataPath"];
	if (dataPath.empty())
		return;

	FSManager::del();

	std::vector<std::string> fileList;
	int targetGame = Config.GetIntValue("TargetGame");
	if (targetGame < 0 || targetGame >= TargetGames.size())
		targetGame = 2; // Default to Skyrim

	std::string cp = "GameDataFiles/" + TargetGames[targetGame].ToStdString();
	wxString activatedFiles = Config[cp];

	wxStringTokenizer tokenizer(activatedFiles, ";");
	std::map<wxString, bool> fsearch;
	while (tokenizer.HasMoreTokens()) {
		wxString val = tokenizer.GetNextToken().Trim(false);
		val = val.Trim().MakeLower();
		fsearch[val] = true;
	}

	wxArrayString files;
	wxDir::GetAllFiles(wxString::FromUTF8(dataPath), &files, "*.ba2", wxDIR_FILES);
	wxDir::GetAllFiles(wxString::FromUTF8(dataPath), &files, "*.bsa", wxDIR_FILES);
	for (auto& f : files) {
		wxString nameOnly = f.AfterLast('/').AfterLast('\\');
		if (fsearch.find(nameOnly.Lower()) == fsearch.end())
			fileList.push_back(f.ToUTF8().data());
	}

	FSManager::addArchives(fileList);
}

bool LeveledListApp::ShowSetup() {
	wxXmlResource* xrc = wxXmlResource::Get();
	bool loaded = xrc->Load(wxString::FromUTF8(Config["AppDir"]) + "/res/xrc/Setup.xrc");
	if (!loaded) {
		wxMessageBox("Failed to load Setup.xrc file!", _("Error"), wxICON_ERROR);
		return false;
	}

	wxDialog* setup = xrc->LoadDialog(nullptr, "dlgSetup");
	if (setup) {
		setup->SetSize(setup->FromDIP(wxSize(700, -1)));
		setup->CenterOnScreen();

		int result = setup->ShowModal();
		if (result >= 0 && result < TargetGames.size()) {
			Config.SetValue("TargetGame", result);
			Config.SetValue("TargetGameName", TargetGames[result].ToStdString());
			Config.SaveConfig(Config["AppDir"] + "/Config.xml");
			
			// Re-init archives after target game change
			InitArchives();
			return true;
		}
		setup->Destroy();
	}
	return false;
}

bool LeveledListApp::SetDefaultConfig() {
	int xborder = wxSystemSettings::GetMetric(wxSYS_FRAMESIZE_X);
	if (xborder < 0) xborder = 0;
	int yborder = wxSystemSettings::GetMetric(wxSYS_FRAMESIZE_Y);
	if (yborder < 0) yborder = 0;

	int currentTarget = -1;
	Config.SetDefaultValue("TargetGame", currentTarget);
	currentTarget = Config.GetIntValue("TargetGame");

	Config.SetDefaultBoolValue("WarnMissingGamePath", true);
	Config.SetDefaultBoolValue("BSATextureScan", true);
	Config.SetDefaultValue("LogLevel", "3");
	Config.SetDefaultBoolValue("UseSystemLanguage", false);
	BodySlideConfig.SetDefaultValue("SelectedOutfit", "");
	BodySlideConfig.SetDefaultValue("SelectedPreset", "");

	Config.SetDefaultValue("Lights/Ambient", 20);
	Config.SetDefaultValue("Lights/Frontal", 20);
	Config.SetDefaultValue("Lights/Directional0", 60);
	Config.SetDefaultValue("Lights/Directional0.x", -90);
	Config.SetDefaultValue("Lights/Directional0.y", 10);
	Config.SetDefaultValue("Lights/Directional0.z", 100);
	Config.SetDefaultValue("Lights/Directional1", 60);
	Config.SetDefaultValue("Lights/Directional1.x", 70);
	Config.SetDefaultValue("Lights/Directional1.y", 10);
	Config.SetDefaultValue("Lights/Directional1.z", 100);
	Config.SetDefaultValue("Lights/Directional2", 85);
	Config.SetDefaultValue("Lights/Directional2.x", 30);
	Config.SetDefaultValue("Lights/Directional2.y", 20);
	Config.SetDefaultValue("Lights/Directional2.z", -100);

	// Target game not set, show setup dialog
	if (currentTarget == -1) {
		if (!ShowSetup())
			return false;
	}

	if (Config["GameDataPath"].empty()) {
		if (Config["WarnMissingGamePath"] == "true") {
			wxLogWarning("Failed to find GameDataPath in the config.");
			wxMessageBox(_("Failed to find GameDataPath in the config. Please set it in the settings."), _("Warning"), wxICON_WARNING);
		}
	}
	else
		wxLogMessage("Game data path: %s", Config["GameDataPath"]);

	return true;
}
