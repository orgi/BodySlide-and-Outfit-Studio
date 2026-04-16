/*
Leveled List Viewer standalone app
*/

#pragma once

#include "LeveledListPreviewer.h"
#include "../utils/ConfigurationManager.h"
#include "../utils/Log.h"
#include "../FSEngine/FSEngine.h"
#include "../FSEngine/FSManager.h"

#include <wx/wx.h>
#include <wx/xrc/xmlres.h>
#include <wx/stdpaths.h>
#include <wx/intl.h>

class LeveledListApp : public wxApp {
	LeveledListPreviewer* llPreviewer = nullptr;
	wxLocale* locale = nullptr;
	Log logger;

public:
	virtual ~LeveledListApp();
	virtual bool OnInit() override;
	
	void InitLanguage();
	void InitArchives();
	bool ShowSetup();
	bool SetDefaultConfig();
	
	void OnPreviewerClosed(wxCommandEvent& event) { llPreviewer = nullptr; ExitMainLoop(); }
};
