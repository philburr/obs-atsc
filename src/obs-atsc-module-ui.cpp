#include "obs-atsc-module-ui.h"
#include <obs-module.h>
#include <util/platform.h>
#include <util/util.hpp>

ATSCOutputUI::ATSCOutputUI(QWidget *parent)
		: QDialog(parent),
		  ui(new Ui_Output)
{
	ui->setupUi(this);

	setSizeGripEnabled(true);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	propertiesView = nullptr;

	connect(ui->startOutput, SIGNAL(released()), this, SLOT(StartOutput()));
	connect(ui->stopOutput, SIGNAL(released()), this, SLOT(StopOutput()));

}

void ATSCOutputUI::ShowHideDialog()
{
	if (propertiesView == nullptr)
		SetupPropertiesView();
	if (!isVisible())
		propertiesView->RefreshProperties();
	setVisible(!isVisible());
}

OBSData load_settings();
void ATSCOutputUI::SetupPropertiesView()
{
	obs_data_t *settings = obs_data_create();

	OBSData data = load_settings();
	if (data)
		obs_data_apply(settings, data);

	propertiesView = new OBSPropertiesView(settings,
			"atsc_output",
			(PropertiesReloadCallback) obs_get_output_properties,
			170);

	ui->propertiesLayout->addWidget(propertiesView);
	obs_data_release(settings);

	connect(propertiesView, SIGNAL(Changed()), this, SLOT(PropertiesChanged()));
}

void ATSCOutputUI::SaveSettings()
{
	BPtr<char> modulePath = obs_module_get_config_path(obs_current_module(), "");

	os_mkdirs(modulePath);

	BPtr<char> path = obs_module_get_config_path(obs_current_module(),
			"obs-atsc.json");

	obs_data_t *settings = propertiesView->GetSettings();
	if (settings) {
		obs_data_save_json_safe(settings, path, "tmp", "bak");
	}
}

void output_start();
void output_stop();
void ATSCOutputUI::StartOutput()
{
	SaveSettings();
	output_start();
}

void ATSCOutputUI::StopOutput()
{
	output_stop();
}

void ATSCOutputUI::PropertiesChanged()
{
	SaveSettings();
}

void ATSCOutputUI::RefreshProperties()
{
	if (isVisible())
		propertiesView->RefreshProperties();
}
