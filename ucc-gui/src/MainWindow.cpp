/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "MainWindow.hpp"
#include "ProfileManager.hpp"
#include "SystemMonitor.hpp"
#include "MonitorTab.hpp"

#include "FanControlTab.hpp"
#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "../libucc-dbus/UccdClient.hpp"

#include "HardwareTab.hpp"

#include <QtWidgets/QTableWidget>
#include <QtWidgets/QHeaderView>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <optional>

// Helper widget for rotated y-axis label
class RotatedLabel : public QLabel
{
public:
  explicit RotatedLabel( const QString &text, QWidget *parent = nullptr )
    : QLabel( text, parent )
  {
  }

protected:
  void paintEvent( QPaintEvent *event ) override
  {
    ( void )event;
    QPainter p( this );
    p.setRenderHint( QPainter::Antialiasing );
    p.translate( width() / 2.0, height() / 2.0 );
    p.rotate( -90 );
    p.translate( -height() / 2.0, -width() / 2.0 );
    QRect r( 0, 0, height(), width() );
    p.setPen( QColor( "#bdbdbd" ) );
    QFont f = font();
    f.setPointSize( 11 );
    p.setFont( f );
    p.drawText( r, Qt::AlignCenter, "% Duty" );
  }

  QSize minimumSizeHint() const override
  {
    return QSize( 24, 80 );
  }

  QSize sizeHint() const override
  {
    return QSize( 24, 120 );
  }
};
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QTabWidget>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QScrollArea>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QPainter>

namespace ucc
{

MainWindow::MainWindow( QWidget *parent )
  : QMainWindow( parent )
  , m_profileManager( std::make_unique< ProfileManager >( this ) )
  , m_systemMonitor( std::make_unique< SystemMonitor >( this ) )
{
  m_UccdClient = std::make_unique< UccdClient >( this );

  // Query device capabilities from daemon
  if ( auto waterCooler = m_UccdClient->getWaterCoolerSupported() )
    m_waterCoolerSupported = *waterCooler;
  if ( auto ctgp = m_UccdClient->getCTGPAdjustmentSupported() )
    m_cTGPAdjustmentSupported = *ctgp;
  if ( auto gpuDefault = m_UccdClient->getNVIDIAPowerCTRLDefaultPowerLimit() )
    m_gpuDefaultPowerLimit = *gpuDefault;

  setWindowTitle( "Uniwill Control Center" );
  setGeometry( 100, 100, 900, 700 );

  setupUI();

  // Connect signals after UI elements are created but before loading data
  connectSignals();

  // Initialize status bar
  // Water cooler status indicator (left of the connection indicator)
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerStatusBarLabel = new QLabel( this );
    m_waterCoolerStatusBarLabel->setTextFormat( Qt::RichText );
    statusBar()->addPermanentWidget( m_waterCoolerStatusBarLabel );

    QFrame *wcSep = new QFrame( this );
    wcSep->setFrameShape( QFrame::VLine );
    wcSep->setFrameShadow( QFrame::Sunken );
    statusBar()->addPermanentWidget( wcSep );

    connect( m_dashboardTab, &DashboardTab::waterCoolerStatusChanged,
             m_waterCoolerStatusBarLabel, &QLabel::setText );
    // Populate the label with the current status immediately
    m_dashboardTab->refreshWaterCoolerStatus();
  }

  m_connectionLabel = new QLabel( this );
  m_connectionLabel->setTextFormat( Qt::RichText );
  statusBar()->addPermanentWidget( m_connectionLabel );
  // Set initial connection state
  onUccdConnectionChanged( m_UccdClient->isConnected() );
  statusBar()->showMessage( "Ready" );

  // Load initial data — refresh() emits signals that populate the UI.
  // Block the profile combo to avoid cascading loadProfileDetails calls.
  m_profileCombo->blockSignals( true );
  m_profileManager->refresh();
  m_profileCombo->blockSignals( false );

  // Now populate the combo and load the active profile exactly once.
  onAllProfilesChanged();

  // Initialize current fan profile ID to first available fan profile (if any)
  m_currentFanProfile = ( m_fanControlTab && m_fanControlTab->fanProfileCombo() && m_fanControlTab->fanProfileCombo()->count() > 0 )
    ? m_fanControlTab->fanProfileCombo()->currentData().toString() : QString();

  // Startup complete — allow hardware interaction from now on
  m_initializing = false;

  // Start monitoring since dashboard is the first tab
  m_systemMonitor->setMonitoringActive( true );
}

MainWindow::~MainWindow()
{
  // Destructor
}

void MainWindow::setupUI()
{
  // Create tab widget
  m_tabs = new QTabWidget( this );
  setCentralWidget( m_tabs );

  // Connect tab changes to control monitoring
  connect( m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged );

  // Fetch system hardware info from daemon and pass to DashboardTab
  QString laptopModel, cpuModel, dGpuModel, iGpuModel;
  if ( auto sysInfoJson = m_UccdClient->getSystemInfoJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *sysInfoJson ) );
    if ( doc.isObject() )
    {
      const QJsonObject obj = doc.object();
      laptopModel = obj.value( "laptopModel" ).toString();
      cpuModel    = obj.value( "cpuModel" ).toString();
      dGpuModel   = obj.value( "dGpuModel" ).toString();
      iGpuModel   = obj.value( "iGpuModel" ).toString();
    }
  }

  // Now create DashboardTab (daemon-backed water cooler; no controller pointer)
  m_dashboardTab = new DashboardTab( m_systemMonitor.get(), m_profileManager.get(), m_waterCoolerSupported,
                                     laptopModel, cpuModel, dGpuModel, iGpuModel, this );
  m_tabs->addTab( m_dashboardTab, "Dashboard" );
  setupProfilesPage();

  // Place the Fan Control tab directly after Profiles and rename it
  setupFanControlTab();

  // Place the GPU OC tab after Fan Control
  setupGpuProfileTab();

  // Add Monitoring graph tab
  m_monitorTab = new MonitorTab( m_UccdClient.get(), this );
  m_tabs->addTab( m_monitorTab, "Monitor" );

  setupKeyboardBacklightPage();
  setupHardwarePage();
}

void MainWindow::setupHardwarePage()
{
  //m_hardwareTab = new HardwareTab( m_systemMonitor.get(), this );
  //m_tabs->addTab( m_hardwareTab, "Hardware" );
}

void MainWindow::setupFanControlTab()
{
  m_fanControlTab = new FanControlTab( m_UccdClient.get(), m_profileManager.get(), m_waterCoolerSupported, this );
  connectFanControlTab();

  m_tabs->addTab( m_fanControlTab, "Profile Fan Control" );

  if ( m_fanControlTab->fanProfileCombo()->count() > 0 )
    onFanProfileChanged( m_fanControlTab->fanProfileCombo()->currentData().toString() );
}

void MainWindow::connectFanControlTab()
{
  connect( m_fanControlTab, &FanControlTab::fanProfileChanged,
           this, &MainWindow::onFanProfileChanged );
  connect( m_fanControlTab, &FanControlTab::cpuPointsChanged,
           this, &MainWindow::onCpuFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::gpuPointsChanged,
           this, &MainWindow::onGpuFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::wcFanPointsChanged,
           this, &MainWindow::onWaterCoolerFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::pumpPointsChanged,
           this, &MainWindow::onPumpPointsChanged );
  connect( m_fanControlTab, &FanControlTab::applyRequested,
           this, &MainWindow::onApplyFanProfilesClicked );
  connect( m_fanControlTab, &FanControlTab::saveRequested,
           this, &MainWindow::onSaveFanProfilesClicked );
  connect( m_fanControlTab, &FanControlTab::copyRequested,
           this, &MainWindow::onCopyFanProfileClicked );

  // Bidirectional water-cooler enable checkbox sync
  // FanControlTab toggle → D-Bus + sync dashboard checkbox
  connect( m_fanControlTab, &FanControlTab::waterCoolerEnableChanged,
           m_dashboardTab, &DashboardTab::setWaterCoolerEnabled );
  // DashboardTab toggle → D-Bus call + sync fan tab checkbox
  connect( m_dashboardTab, &DashboardTab::waterCoolerEnableChanged,
           this, [this]( bool enabled ) {
             m_fanControlTab->setWaterCoolerEnabled( enabled );
             m_fanControlTab->sendWaterCoolerEnable( enabled );
           } );
  connect( m_fanControlTab, &FanControlTab::removeRequested,
           this, &MainWindow::onRemoveFanProfileClicked );

  // Sync fan profile rename from fan tab to profile page fan combo
  connect( m_fanControlTab, &FanControlTab::fanProfileRenamed,
           this, [this]( const QString &oldName, const QString &newName ) {
    if ( m_profileFanProfileCombo ) {
      if ( int idx = m_profileFanProfileCombo->findText( oldName ); idx != -1 )
        m_profileFanProfileCombo->setItemText( idx, newName );
    }
  } );
}

void MainWindow::setupGpuProfileTab()
{
  m_gpuProfileTab = new GpuProfileTab( m_UccdClient.get(), m_profileManager.get(), this );
  connectGpuProfileTab();
  m_tabs->addTab( m_gpuProfileTab, "GPU Overclocking" );
}

void MainWindow::connectGpuProfileTab()
{
  connect( m_gpuProfileTab, &GpuProfileTab::applyRequested,
           this, &MainWindow::onApplyGpuProfileClicked );
  connect( m_gpuProfileTab, &GpuProfileTab::saveRequested,
           this, &MainWindow::onSaveGpuProfileClicked );
  connect( m_gpuProfileTab, &GpuProfileTab::copyRequested,
           this, &MainWindow::onCopyGpuProfileClicked );
  connect( m_gpuProfileTab, &GpuProfileTab::removeRequested,
           this, &MainWindow::onRemoveGpuProfileClicked );
  connect( m_gpuProfileTab, &GpuProfileTab::gpuProfileChanged,
           this, &MainWindow::onGpuProfileChanged );

  // Sync GPU profile rename to the profile-page GPU combo
  connect( m_gpuProfileTab, &GpuProfileTab::gpuProfileRenamed,
           this, [this]( const QString &oldName, const QString &newName ) {
    if ( m_profileGpuProfileCombo ) {
      if ( int idx = m_profileGpuProfileCombo->findText( oldName ); idx != -1 )
        m_profileGpuProfileCombo->setItemText( idx, newName );
    }
  } );

  // When custom GPU profiles change in ProfileManager, refresh the combos
  connect( m_profileManager.get(), &ProfileManager::customGpuProfilesChanged,
           this, [this]() {
    if ( m_gpuProfileTab )
      m_gpuProfileTab->reloadGpuProfiles();

    // Also update the profile page combo
    if ( m_profileGpuProfileCombo )
    {
      QString prevId = m_profileGpuProfileCombo->currentData().toString();
      m_profileGpuProfileCombo->blockSignals( true );
      m_profileGpuProfileCombo->clear();
      m_profileGpuProfileCombo->addItem( "(None)", QString() );
      for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
      {
        if ( v.isObject() )
        {
          QJsonObject o = v.toObject();
          m_profileGpuProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
        }
      }
      for ( const auto &v : m_profileManager->customGpuProfilesData() )
      {
        QJsonObject o = v.toObject();
        m_profileGpuProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
      }
      // Restore selection
      for ( int i = 0; i < m_profileGpuProfileCombo->count(); ++i )
      {
        if ( m_profileGpuProfileCombo->itemData( i ).toString() == prevId )
        { m_profileGpuProfileCombo->setCurrentIndex( i ); break; }
      }
      m_profileGpuProfileCombo->blockSignals( false );
    }
  } );
}



void MainWindow::setupProfilesPage()
{
  QWidget *profilesWidget = new QWidget();
  QVBoxLayout *mainLayout = new QVBoxLayout( profilesWidget );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // Create scroll area for the profile content
  QScrollArea *scrollArea = new QScrollArea();
  scrollArea->setWidgetResizable( true );

  QWidget *scrollWidget = new QWidget();
  QVBoxLayout *scrollLayout = new QVBoxLayout( scrollWidget );
  scrollLayout->setContentsMargins( 20, 20, 20, 20 );
  scrollLayout->setSpacing( 15 );

  // Profile Selection ComboBox (in top layout)
  QHBoxLayout *selectLayout = new QHBoxLayout();

  m_profileCombo = new QComboBox();
  m_profileCombo->setEditable( true );
  m_profileCombo->setInsertPolicy( QComboBox::NoInsert );
  // Don't populate here - will be done by onAllProfilesChanged signal
  m_profileCombo->setCurrentIndex( m_profileManager->activeProfileIndex() );
  m_selectedProfileIndex = m_profileManager->activeProfileIndex();
  m_applyButton = new QPushButton( "Apply" );
  m_applyButton->setMaximumWidth( 80 );

  m_saveButton = new QPushButton( "Save" );
  m_saveButton->setMaximumWidth( 80 );
  m_saveButton->setEnabled( false );

  m_copyProfileButton = new QPushButton( "Copy" );
  m_copyProfileButton->setMaximumWidth( 60 );

  m_removeProfileButton = new QPushButton( "Remove" );
  m_removeProfileButton->setMaximumWidth( 70 );

  selectLayout->addWidget( m_profileCombo, 1 );
  selectLayout->addWidget( m_applyButton );
  selectLayout->addWidget( m_saveButton );
  selectLayout->addWidget( m_copyProfileButton );
  selectLayout->addWidget( m_removeProfileButton );
  mainLayout->addLayout( selectLayout );

  // Add a separator line
  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  separator->setStyleSheet( "color: #cccccc;" );
  mainLayout->addWidget( separator );

  // Now use grid layout for the details
  scrollLayout->setContentsMargins( 15, 10, 15, 10 );
  QGridLayout *detailsLayout = new QGridLayout();
  detailsLayout->setSpacing( 12 );
  detailsLayout->setColumnStretch( 0, 0 );  // Labels column - minimal width
  detailsLayout->setColumnStretch( 1, 1 );  // Controls column - expand

  int row = 0;

  // === DESCRIPTION ===
  QLabel *descLabel = new QLabel( "Description" );
  descLabel->setStyleSheet( "font-weight: bold;" );
  m_descriptionEdit = new QTextEdit();
  m_descriptionEdit->setPlainText( "Edit profile to change behaviour" );
  m_descriptionEdit->setMaximumHeight( 60 );
  detailsLayout->addWidget( descLabel, row, 0, Qt::AlignTop );
  detailsLayout->addWidget( m_descriptionEdit, row, 1 );
  row++;

  // === ACTIVATE PROFILE AUTOMATICALLY ON ===
  QLabel *autoActivateLabel = new QLabel( "Activate profile automatically on" );
  autoActivateLabel->setStyleSheet( "font-weight: bold;" );
  QHBoxLayout *buttonLayout = new QHBoxLayout();
  m_mainsButton = new QPushButton( "Mains" );
  m_mainsButton->setCheckable( true );
  m_batteryButton = new QPushButton( "Battery" );
  m_batteryButton->setCheckable( true );
  m_waterCoolerButton = new QPushButton( "Water Cooler" );
  m_waterCoolerButton->setCheckable( true );
  m_waterCoolerButton->setCheckable( true );
  m_mainsButton->setMaximumWidth( 100 );
  m_batteryButton->setMaximumWidth( 100 );
  m_waterCoolerButton->setMaximumWidth( 100 );
  buttonLayout->addWidget( m_mainsButton );
  buttonLayout->addWidget( m_batteryButton );
  buttonLayout->addWidget( m_waterCoolerButton );
  buttonLayout->addStretch();
  detailsLayout->addWidget( autoActivateLabel, row, 0, Qt::AlignTop );
  detailsLayout->addLayout( buttonLayout, row, 1 );
  row++;

  // Hide water cooler profile activation button if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerButton->setVisible( false );
  }

  // Add spacer/separator
  detailsLayout->addItem( new QSpacerItem( 0, 15 ), row, 0, 1, 2 );
  row++;

  // === CHARGING SECTION (visible only when hardware supports it) ===
  QLabel *chargingHeader = new QLabel( "Charging" );
  chargingHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( chargingHeader, row, 0, 1, 2 );
  row++;

  QLabel *chargingProfileLabel = new QLabel( "Charging profile" );
  m_profileChargingProfileCombo = new QComboBox();
  detailsLayout->addWidget( chargingProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargingProfileCombo, row, 1 );
  row++;

  QLabel *chargingPriorityLabel = new QLabel( "Charging priority" );
  m_profileChargingPriorityCombo = new QComboBox();
  detailsLayout->addWidget( chargingPriorityLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargingPriorityCombo, row, 1 );
  row++;

  QLabel *chargeLimitLabel = new QLabel( "Charge limit" );
  m_profileChargeLimitCombo = new QComboBox();
  m_profileChargeLimitCombo->addItem( "Full Capacity (100%)", "full" );
  m_profileChargeLimitCombo->addItem( "Reduced (~90%)", "reduced" );
  m_profileChargeLimitCombo->addItem( "Stationary (~80%)", "stationary" );
  detailsLayout->addWidget( chargeLimitLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargeLimitCombo, row, 1 );
  row++;

  auto setChargingSectionVisible = [chargingHeader, chargingProfileLabel,
                                    chargingPriorityLabel, chargeLimitLabel, this]( bool visible )
  {
    chargingHeader->setVisible( visible );
    chargingProfileLabel->setVisible( visible );
    m_profileChargingProfileCombo->setVisible( visible );

    // Charging priority is only visible if priorities are available
    bool hasPriorities = m_systemMonitor && !m_systemMonitor->chargingPrioritiesAvailable().isEmpty();
    chargingPriorityLabel->setVisible( visible && hasPriorities );
    m_profileChargingPriorityCombo->setVisible( visible && hasPriorities );

    // Charge limit is visible when thresholds are available
    bool hasThresholds = m_systemMonitor && m_systemMonitor->chargeThresholdsAvailable();
    chargeLimitLabel->setVisible( visible && hasThresholds );
    m_profileChargeLimitCombo->setVisible( visible && hasThresholds );
  };

  // Populate and show/hide based on hardware support
  if ( m_systemMonitor )
  {
    auto populateChargingProfiles = [this, setChargingSectionVisible]()
    {
      const QStringList profiles = m_systemMonitor->chargingProfilesAvailable();
      m_profileChargingProfileCombo->blockSignals( true );
      m_profileChargingProfileCombo->clear();

      static const QMap< QString, QString > displayNames = {
          { "high_capacity", "High Capacity (Full charge)" },
          { "balanced", "Balanced (~90%)" },
          { "stationary", "Stationary (~80%)" } };

      for ( const auto &p : profiles )
      {
        QString display = displayNames.value( p, p );
        m_profileChargingProfileCombo->addItem( display, p );
      }
      m_profileChargingProfileCombo->blockSignals( false );
      setChargingSectionVisible( !profiles.isEmpty() );
    };

    auto populateChargingPriorities = [this, setChargingSectionVisible]()
    {
      const QStringList priorities = m_systemMonitor->chargingPrioritiesAvailable();
      m_profileChargingPriorityCombo->blockSignals( true );
      m_profileChargingPriorityCombo->clear();

      static const QMap< QString, QString > priorityDisplayNames = {
          { "charge_battery", "Charge Battery" },
          { "performance", "Performance" } };

      for ( const auto &p : priorities )
      {
        QString display = priorityDisplayNames.value( p, p );
        m_profileChargingPriorityCombo->addItem( display, p );
      }
      m_profileChargingPriorityCombo->blockSignals( false );

      // Re-evaluate visibility since priority availability changed
      bool hasProfiles = !m_systemMonitor->chargingProfilesAvailable().isEmpty();
      setChargingSectionVisible( hasProfiles );
    };

    populateChargingProfiles();
    populateChargingPriorities();
    connect( m_systemMonitor.get(), &SystemMonitor::chargingProfilesAvailableChanged, this,
             populateChargingProfiles );
    connect( m_systemMonitor.get(), &SystemMonitor::chargingPrioritiesAvailableChanged, this,
             populateChargingPriorities );
    connect( m_systemMonitor.get(), &SystemMonitor::chargeThresholdsAvailableChanged, this,
             [this, setChargingSectionVisible]()
             {
               bool hasProfiles = !m_systemMonitor->chargingProfilesAvailable().isEmpty();
               setChargingSectionVisible( hasProfiles );
             } );
  }
  else
  {
    setChargingSectionVisible( false );
  }

  // Add spacer before Display section
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === DISPLAY SECTION ===
  QLabel *displayHeader = new QLabel( "Display and Keyboard" );
  displayHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( displayHeader, row, 0, 1, 2 );
  row++;

  QLabel *keyboardProfileLabel = new QLabel( "Keyboard profile" );
  m_profileKeyboardProfileCombo = new QComboBox();

  for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
  {
    QJsonObject o = v.toObject();
    m_profileKeyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
  }

  detailsLayout->addWidget( keyboardProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileKeyboardProfileCombo, row, 1 );
  row++;

  QLabel *backlightLabel = new QLabel( "Backlight brightness" );
  QHBoxLayout *backlightLayout = new QHBoxLayout();
  m_brightnessSlider = new QSlider( Qt::Horizontal );
  m_brightnessSlider->setMinimum( 0 );
  m_brightnessSlider->setMaximum( 100 );
  m_brightnessSlider->setValue( 100 );
  m_brightnessValueLabel = new QLabel( "100%" );
  m_brightnessValueLabel->setMinimumWidth( 40 );
  backlightLayout->addWidget( m_brightnessSlider, 1 );
  backlightLayout->addWidget( m_brightnessValueLabel );
  detailsLayout->addWidget( backlightLabel, row, 0 );
  detailsLayout->addLayout( backlightLayout, row, 1 );
  row++;

  QLabel *setBrightnessLabel = new QLabel( "Set brightness on profile activation" );
  m_setBrightnessCheckBox = new QCheckBox();
  m_setBrightnessCheckBox->setChecked( false );
  detailsLayout->addWidget( setBrightnessLabel, row, 0 );
  detailsLayout->addWidget( m_setBrightnessCheckBox, row, 1, Qt::AlignLeft );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === FAN CONTROL SECTION ===
  QLabel *fanHeader = new QLabel( "Fan control" );
  fanHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( fanHeader, row, 0, 1, 2 );
  row++;

  QLabel *fanProfileLabel = new QLabel( "Fan profile" );
  m_profileFanProfileCombo = new QComboBox();
  // Add built-in fan profiles from daemon (id + name)
  for ( const auto &v : m_profileManager->builtinFanProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileFanProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }
  // Append persisted custom fan profiles loaded from settings
  for ( const auto &v : m_profileManager->customFanProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      QString name = o["name"].toString();
      if ( m_profileFanProfileCombo->findText( name ) == -1 )
        m_profileFanProfileCombo->addItem( name, o["id"].toString() );
    }
  }
  detailsLayout->addWidget( fanProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileFanProfileCombo, row, 1 );
  row++;

  QLabel *sameSpeedLabel = new QLabel( "Same fan speed for all fans" );
  detailsLayout->addWidget( sameSpeedLabel, row, 0 );
  // Reuse the shared checkbox created in the dashboard (create if not present)
  if ( !m_sameFanSpeedCheckBox ) {
    m_sameFanSpeedCheckBox = new QCheckBox();
    m_sameFanSpeedCheckBox->setChecked( true );
  }
  detailsLayout->addWidget( m_sameFanSpeedCheckBox, row, 1, Qt::AlignLeft );
  row++;

  QLabel *autoWaterLabel = new QLabel( "Water cooler auto control" );
  m_autoWaterControlCheckBox = new QCheckBox();
  m_autoWaterControlCheckBox->setChecked( true );
  m_autoWaterControlCheckBox->setToolTip( tr( "When enabled the daemon will control the water cooler automatically" ) );
  detailsLayout->addWidget( autoWaterLabel, row, 0 );
  detailsLayout->addWidget( m_autoWaterControlCheckBox, row, 1, Qt::AlignLeft );
  row++;

  // Hide water cooler auto control if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    autoWaterLabel->setVisible( false );
    m_autoWaterControlCheckBox->setVisible( false );
  }

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === GPU OC PROFILE SECTION ===
  QLabel *gpuProfileHeader = new QLabel( "GPU Overclocking" );
  gpuProfileHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( gpuProfileHeader, row, 0, 1, 2 );
  row++;

  QLabel *gpuProfileLabel = new QLabel( "GPU OC profile" );
  m_profileGpuProfileCombo = new QComboBox();
  m_profileGpuProfileCombo->addItem( "(None)", QString() );
  for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileGpuProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }
  for ( const auto &v : m_profileManager->customGpuProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileGpuProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }
  detailsLayout->addWidget( gpuProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileGpuProfileCombo, row, 1 );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === SYSTEM PERFORMANCE SECTION ===
  QLabel *sysHeader = new QLabel( "System performance" );
  sysHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( sysHeader, row, 0, 1, 2 );
  row++;

  QLabel *odmPowerHeader = new QLabel( "CPU power limit control" );
  detailsLayout->addWidget( odmPowerHeader, row, 0, 1, 2 );
  row++;

  // TDP Limit 1
  QLabel *tdp1Label = new QLabel( "Sustained TDP" );  // Sustained Power Limit
  QHBoxLayout *tdp1Layout = new QHBoxLayout();
  m_odmPowerLimit1Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit1Slider->setMinimum( 0 );
  m_odmPowerLimit1Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit1Slider->setValue( 0 );
  m_odmPowerLimit1Value = new QLabel( "0 W" );
  m_odmPowerLimit1Value->setMinimumWidth( 50 );
  tdp1Layout->addWidget( m_odmPowerLimit1Slider, 1 );
  tdp1Layout->addWidget( m_odmPowerLimit1Value );
  detailsLayout->addWidget( tdp1Label, row, 0 );
  detailsLayout->addLayout( tdp1Layout, row, 1 );
  row++;

  // TDP Limit 2
  QLabel *tdp2Label = new QLabel( "Boost TDP" );
  QHBoxLayout *tdp2Layout = new QHBoxLayout();
  m_odmPowerLimit2Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit2Slider->setMinimum( 0 );
  m_odmPowerLimit2Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit2Slider->setValue( 0 );
  m_odmPowerLimit2Value = new QLabel( "0 W" );
  m_odmPowerLimit2Value->setMinimumWidth( 50 );
  tdp2Layout->addWidget( m_odmPowerLimit2Slider, 1 );
  tdp2Layout->addWidget( m_odmPowerLimit2Value );
  detailsLayout->addWidget( tdp2Label, row, 0 );
  detailsLayout->addLayout( tdp2Layout, row, 1 );
  row++;

  // TDP Limit 3
  QLabel *tdp3Label = new QLabel( "Peak TDP" );
  QHBoxLayout *tdp3Layout = new QHBoxLayout();
  m_odmPowerLimit3Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit3Slider->setMinimum( 0 );
  m_odmPowerLimit3Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit3Slider->setValue( 0 );
  m_odmPowerLimit3Value = new QLabel( "0 W" );
  m_odmPowerLimit3Value->setMinimumWidth( 50 );
  tdp3Layout->addWidget( m_odmPowerLimit3Slider, 1 );
  tdp3Layout->addWidget( m_odmPowerLimit3Value );
  detailsLayout->addWidget( tdp3Label, row, 0 );
  detailsLayout->addLayout( tdp3Layout, row, 1 );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 5 ), row, 0, 1, 2 );
  row++;

  QLabel *cpuFreqHeader = new QLabel( "CPU frequency control" );
  detailsLayout->addWidget( cpuFreqHeader, row, 0, 1, 2 );
  row++;

  QLabel *coresLabel = new QLabel( "Number of logical cores" );
  QHBoxLayout *coresLayout = new QHBoxLayout();
  const int nCores = m_UccdClient->getCpuCoreCount().value_or( 1 );
  m_cpuCoresSlider = new QSlider( Qt::Horizontal );
  m_cpuCoresSlider->setMinimum( 1 );
  m_cpuCoresSlider->setMaximum( nCores > 0 ? nCores : 1 );
  m_cpuCoresSlider->setValue( nCores > 0 ? nCores : 1 );
  m_cpuCoresValue = new QLabel( QString::number( nCores > 0 ? nCores : 1 ) );
  m_cpuCoresValue->setMinimumWidth( 35 );
  coresLayout->addWidget( m_cpuCoresSlider, 1 );
  coresLayout->addWidget( m_cpuCoresValue );
  detailsLayout->addWidget( coresLabel, row, 0 );
  detailsLayout->addLayout( coresLayout, row, 1 );
  row++;

  QLabel *maxPerfLabel = new QLabel( "CPU Governor" );
  m_governorCombo = new QComboBox();
  detailsLayout->addWidget( maxPerfLabel, row, 0 );
  detailsLayout->addWidget( m_governorCombo, row, 1, Qt::AlignLeft );
  row++;

  QLabel *eppLabel = new QLabel( "Energy Performance Preference" );
  m_eppCombo = new QComboBox();
  detailsLayout->addWidget( eppLabel, row, 0 );
  detailsLayout->addWidget( m_eppCombo, row, 1, Qt::AlignLeft );
  row++;

  QLabel *minFreqLabel = new QLabel( "Minimum frequency" );
  QHBoxLayout *minFreqLayout = new QHBoxLayout();
  m_minFrequencySlider = new QSlider( Qt::Horizontal );
  m_minFrequencySlider->setSingleStep( 100000 ); // 100 MHz steps (in kHz)

  // Get hardware frequency limits and initialize slider with actual values
  int minFreqKHz = 400000;  // fallback
  int maxFreqKHz = 6000000; // fallback
  if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
    if ( doc.isObject() )
    {
      QJsonObject limitsObj = doc.object();
      minFreqKHz = limitsObj["min"].toInt( 400000 );
      maxFreqKHz = limitsObj["max"].toInt( 6000000 );
      m_cpuMinFreqKHz = minFreqKHz;
      m_cpuMaxFreqKHz = maxFreqKHz;
    }
  }
  m_minFrequencySlider->setMinimum( minFreqKHz );
  m_minFrequencySlider->setMaximum( maxFreqKHz );
  m_minFrequencySlider->setValue( minFreqKHz );

  m_minFrequencyValue = new QLabel();
  m_minFrequencyValue->setMinimumWidth( 60 );
  double freqGHz = minFreqKHz / 1000000.0;
  m_minFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );

  minFreqLayout->addWidget( m_minFrequencySlider, 1 );
  minFreqLayout->addWidget( m_minFrequencyValue );
  detailsLayout->addWidget( minFreqLabel, row, 0 );
  detailsLayout->addLayout( minFreqLayout, row, 1 );
  row++;

  QLabel *maxFreqLabel = new QLabel( "Maximum frequency" );
  QHBoxLayout *maxFreqLayout = new QHBoxLayout();
  m_maxFrequencySlider = new QSlider( Qt::Horizontal );
  m_maxFrequencySlider->setSingleStep( 100000 ); // 100 MHz steps (in kHz)
  m_maxFrequencySlider->setMinimum( minFreqKHz );
  m_maxFrequencySlider->setMaximum( maxFreqKHz );
  m_maxFrequencySlider->setValue( maxFreqKHz );

  m_maxFrequencyValue = new QLabel();
  m_maxFrequencyValue->setMinimumWidth( 60 );
  freqGHz = maxFreqKHz / 1000000.0;
  m_maxFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
  maxFreqLayout->addWidget( m_maxFrequencySlider, 1 );
  maxFreqLayout->addWidget( m_maxFrequencyValue );
  detailsLayout->addWidget( maxFreqLabel, row, 0 );
  detailsLayout->addLayout( maxFreqLayout, row, 1 );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  detailsLayout->addItem( new QSpacerItem( 0, 20, QSizePolicy::Minimum, QSizePolicy::Expanding ), row, 0, 1, 2 );

  scrollLayout->addLayout( detailsLayout );

  scrollArea->setWidget( scrollWidget );
  mainLayout->addWidget( scrollArea );

  m_tabs->addTab( profilesWidget, "Profiles" );
}

void MainWindow::connectSignals()
{
  // Profile page connections

  connect( m_profileManager.get(), &ProfileManager::allProfilesChanged,
           this, &MainWindow::onAllProfilesChanged );

  connect( m_profileManager.get(), &ProfileManager::activeProfileIndexChanged,
           this, &MainWindow::onActiveProfileIndexChanged );

  connect( m_profileManager.get(), &ProfileManager::activeProfileChanged,
           this, [this]() {
    if ( m_initializing ) return;  // defer to onAllProfilesChanged after init
    qDebug() << "activeProfileChanged signal received, updating UI";
    loadProfileDetails( m_profileManager->activeProfileId() );
  } );

  connect( m_profileManager.get(), &ProfileManager::customKeyboardProfilesChanged,
           this, &MainWindow::onCustomKeyboardProfilesChanged );

  // Sub-profile sync: when a remote client (e.g. tray) changes the
  // keyboard or fan profile, update the combo boxes and editors without
  // writing back to hardware (the hardware already has the new state).
  connect( m_profileManager.get(), &ProfileManager::activeKeyboardProfileChanged,
           this, [this]( const QString &kbId ) {
    if ( m_initializing ) return;
    updateKeyboardEditorFromProfile( kbId );
  } );
  connect( m_profileManager.get(), &ProfileManager::activeFanProfileChanged,
           this, [this]( const QString &fpId ) {
    if ( m_initializing ) return;
    updateFanEditorFromProfile( fpId );
  } );
  connect( m_profileManager.get(), &ProfileManager::activeGpuProfileChanged,
           this, [this]( const QString &gpId ) {
    if ( m_initializing || gpId.isEmpty() )
      return;

    if ( m_gpuProfileTab && m_gpuProfileTab->gpuProfileCombo() )
    {
      auto *combo = m_gpuProfileTab->gpuProfileCombo();
      if ( int idx = combo->findData( gpId ); idx >= 0 && combo->currentIndex() != idx )
        combo->setCurrentIndex( idx );
    }

    if ( m_profileGpuProfileCombo )
    {
      if ( int idx = m_profileGpuProfileCombo->findData( gpId ); idx >= 0
           && m_profileGpuProfileCombo->currentIndex() != idx )
      {
        m_profileGpuProfileCombo->setCurrentIndex( idx );
      }
    }

    onGpuProfileChanged( gpId );
  } );

  connect( m_profileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, &MainWindow::onProfileIndexChanged );

  // Display controls

  connect( m_brightnessSlider, &QSlider::valueChanged,
           this, &MainWindow::onBrightnessSliderChanged );

  // ODM Power Limit controls

  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit1Changed );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit2Changed );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit3Changed );

  // CPU frequency controls

  connect( m_cpuCoresSlider, &QSlider::valueChanged,
           this, &MainWindow::onCpuCoresChanged );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, &MainWindow::onMaxFrequencyChanged );

  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    double freqGHz = value / 1000000.0;  // Convert kHz to GHz for display
    m_minFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
  } );

  // Enforce min <= max for frequency sliders
  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value > m_maxFrequencySlider->value() )
      m_maxFrequencySlider->setValue( value );
  } );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_minFrequencySlider->value() )
      m_minFrequencySlider->setValue( value );
  } );

  // Enforce TDP ordering: sustained <= boost <= peak
  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value > m_odmPowerLimit2Slider->value() )
      m_odmPowerLimit2Slider->setValue( value );
  } );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_odmPowerLimit1Slider->value() )
      m_odmPowerLimit1Slider->setValue( value );
    if ( value > m_odmPowerLimit3Slider->value() )
      m_odmPowerLimit3Slider->setValue( value );
  } );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_odmPowerLimit2Slider->value() )
      m_odmPowerLimit2Slider->setValue( value );
  } );

  // Apply and Save buttons

  connect( m_applyButton, &QPushButton::clicked,
           this, &MainWindow::onApplyClicked );

  connect( m_saveButton, &QPushButton::clicked,
           this, &MainWindow::onSaveClicked );

  connect( m_copyProfileButton, &QPushButton::clicked,
           this, &MainWindow::onCopyProfileClicked );

  connect( m_removeProfileButton, &QPushButton::clicked,
           this, &MainWindow::onRemoveProfileClicked );

  // Connect all profile controls to mark changes

  connect( m_descriptionEdit, &QTextEdit::textChanged,
           this, &MainWindow::markChanged );

  // Profile combo rename handling
  connect( m_profileCombo->lineEdit(), &QLineEdit::editingFinished,
           this, &MainWindow::onProfileComboRenamed );

  connect( m_setBrightnessCheckBox, &QCheckBox::toggled,
           this, &MainWindow::markChanged );

  connect( m_brightnessSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_profileFanProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this](int index) {
             markChanged();
             // Update fan profile tab to match profile tab selection
             m_fanControlTab->fanProfileCombo()->blockSignals(true);
             m_fanControlTab->fanProfileCombo()->setCurrentIndex(index);
             m_fanControlTab->fanProfileCombo()->blockSignals(false);
             // Load the fan curves for the new profile
             onFanProfileChanged(m_profileFanProfileCombo->currentData().toString());
           } );

  connect( m_autoWaterControlCheckBox, &QCheckBox::toggled,
           this, &MainWindow::markChanged );
  connect( m_autoWaterControlCheckBox, &QCheckBox::toggled,
           this, [this]( bool autoControl ) {
             if ( m_fanControlTab )
               m_fanControlTab->setWaterCoolerAutoControl( autoControl );
           } );

  connect( m_cpuCoresSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_governorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
           this, &MainWindow::markChanged );

  connect( m_eppCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
           this, &MainWindow::markChanged );

  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_profileKeyboardProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this](int index) {
             markChanged();
             // Keep the keyboard tab combo in sync so it reflects the selection,
             // but do NOT apply to hardware — that happens when the profile is saved/applied.
             m_keyboardProfileCombo->blockSignals(true);
             m_keyboardProfileCombo->setCurrentIndex(index);
             m_keyboardProfileCombo->blockSignals(false);
           } );

  if ( m_profileChargingProfileCombo )
  {
    connect( m_profileChargingProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }
  if ( m_profileChargingPriorityCombo )
  {
    connect( m_profileChargingPriorityCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }
  if ( m_profileChargeLimitCombo )
  {
    connect( m_profileChargeLimitCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }

  // Mains/Battery/Water Cooler activation buttons
  connect( m_mainsButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  connect( m_batteryButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  connect( m_waterCoolerButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  // Error handling

  connect( m_profileManager.get(), QOverload< const QString & >::of( &ProfileManager::error ),
           this, [this]( const QString &msg ) {
    statusBar()->showMessage( "Error: " + msg );
    m_saveInProgress = false;
    updateButtonStates();
  } );

  // React to DBus client connection status so we can (re)load built-in fan profiles
  connect( m_UccdClient.get(), &UccdClient::connectionStatusChanged,
           this, &MainWindow::onUccdConnectionChanged );

  // Update connection status label when active profile changes
  connect( m_profileManager.get(), &ProfileManager::activeProfileIndexChanged,
           this, &MainWindow::updateConnectionStatusLabel );

  connectKeyboardBacklightPageWidgets();

  // Sync profile page keyboard combo (profiles may have been added before
  // the customKeyboardProfilesChanged signal was connected)
  onCustomKeyboardProfilesChanged();

  // Initial load of fan profiles (may be empty if service not yet available)
  reloadFanProfiles();

  // Populate governor combo
  populateGovernorCombo();

  // Populate EPP combo
  populateEppCombo();

  // ── Live crosshair tracking on fan curve editors ──
  connect( m_systemMonitor.get(), &SystemMonitor::cpuTempChanged,              this, &MainWindow::updateFanCrosshairs );
  connect( m_systemMonitor.get(), &SystemMonitor::fanSpeedChanged,             this, &MainWindow::updateFanCrosshairs );
  connect( m_systemMonitor.get(), &SystemMonitor::gpuTempChanged,              this, &MainWindow::updateFanCrosshairs );
  connect( m_systemMonitor.get(), &SystemMonitor::gpuFanSpeedChanged,          this, &MainWindow::updateFanCrosshairs );
  connect( m_systemMonitor.get(), &SystemMonitor::waterCoolerFanSpeedChanged,  this, &MainWindow::updateFanCrosshairs );
  connect( m_systemMonitor.get(), &SystemMonitor::waterCoolerPumpLevelChanged,  this, &MainWindow::updateFanCrosshairs );
}

void MainWindow::populateGovernorCombo()
{
  if ( !m_governorCombo )
    return;

  m_governorCombo->clear();

  if ( auto governors = m_UccdClient->getAvailableCpuGovernors(); governors && !governors->empty() )
  {
    for ( const auto &gov : *governors )
      m_governorCombo->addItem( QString::fromStdString( gov ), QString::fromStdString( gov ) );
  }
}

void MainWindow::populateEppCombo()
{
  if ( !m_eppCombo )
    return;

  m_eppCombo->clear();

  if ( auto epps = m_UccdClient->getAvailableEPPs(); epps && !epps->empty() )
  {
    for ( const auto &epp : *epps )
      m_eppCombo->addItem( QString::fromStdString( epp ), QString::fromStdString( epp ) );
  }
}

std::optional< double > MainWindow::parseMonitorValue( const QString &str )
{
  QString s = str.trimmed();
  if ( s == "--" || s.isEmpty() )
    return std::nullopt;

  // Strip known suffixes (use QString for correct QChar-level chop)
  static const QStringList suffixes = { QStringLiteral( "°C" ),
                                        QStringLiteral( " %" ),
                                        QStringLiteral( " MHz" ),
                                        QStringLiteral( " W" ),
                                        QStringLiteral( " RPM" ) };
  for ( const auto &suffix : suffixes )
  {
    if ( s.endsWith( suffix ) )
    {
      s.chop( suffix.size() );
      break;
    }
  }

  bool ok = false;
  double v = s.toDouble( &ok );
  return ok ? std::optional< double >( v ) : std::nullopt;
}

void MainWindow::updateFanCrosshairs()
{
  const int fanTabIndex = m_tabs->indexOf( m_fanControlTab );
  if ( !m_fanControlTab || m_tabs->currentIndex() != fanTabIndex )
    return;

  // CPU fan editor
  {
    auto temp = parseMonitorValue( m_systemMonitor->cpuTemp() );
    auto duty = parseMonitorValue( m_systemMonitor->cpuFanSpeed() );
    qDebug() << "[Crosshair] CPU raw:" << m_systemMonitor->cpuTemp() << m_systemMonitor->cpuFanSpeed()
             << "parsed temp:" << ( temp ? *temp : -1 ) << "duty:" << ( duty ? *duty : -1 );
    if ( temp && duty )
      m_fanControlTab->cpuEditor()->setCrosshair( *temp, *duty );
    else
      m_fanControlTab->cpuEditor()->clearCrosshair();
  }

  // GPU fan editor
  {
    auto temp = parseMonitorValue( m_systemMonitor->gpuTemp() );
    auto duty = parseMonitorValue( m_systemMonitor->gpuFanSpeed() );
    if ( temp && duty )
      m_fanControlTab->gpuEditor()->setCrosshair( *temp, *duty );
    else
      m_fanControlTab->gpuEditor()->clearCrosshair();
  }

  // Water cooler fan editor (uses CPU temp as temperature source)
  if ( m_fanControlTab->wcFanEditor() )
  {
    auto temp = parseMonitorValue( m_systemMonitor->cpuTemp() );
    auto duty = parseMonitorValue( m_systemMonitor->waterCoolerFanSpeed() );
    if ( temp && duty )
      m_fanControlTab->wcFanEditor()->setCrosshair( *temp, *duty );
    else
      m_fanControlTab->wcFanEditor()->clearCrosshair();
  }

  // Pump curve editor (uses CPU temp and pump voltage level)
  if ( m_fanControlTab->pumpEditor() )
  {
    auto temp = parseMonitorValue( m_systemMonitor->cpuTemp() );
    int pumpLevel = -1;
    const QString &lvlStr = m_systemMonitor->waterCoolerPumpLevel();
    if ( lvlStr == "Off" )       pumpLevel = 0;
    else if ( lvlStr == "Low" )  pumpLevel = 1;
    else if ( lvlStr == "Med" )  pumpLevel = 2;
    else if ( lvlStr == "High" ) pumpLevel = 3;

    if ( temp && pumpLevel >= 0 )
      m_fanControlTab->pumpEditor()->setCrosshair( *temp, pumpLevel );
    else
      m_fanControlTab->pumpEditor()->clearCrosshair();
  }
}

void MainWindow::onTabChanged( int index )
{
  const int fanTabIndex = m_fanControlTab ? m_tabs->indexOf( m_fanControlTab ) : -1;
  const int monitorTabIndex = m_monitorTab ? m_tabs->indexOf( m_monitorTab ) : -1;

  // Enable monitoring when dashboard (0) or fan control tab is visible
  bool needsMonitoring = ( index == 0 || index == fanTabIndex );
  qDebug() << "Tab changed to" << index << "- Monitoring active:" << needsMonitoring
           << "(fan tab =" << fanTabIndex << ")";
  m_systemMonitor->setMonitoringActive( needsMonitoring );

  // Activate / deactivate the Monitor tab's incremental fetch
  if ( m_monitorTab )
    m_monitorTab->setMonitoringActive( index == monitorTabIndex );

  // Update or clear fan curve crosshairs
  if ( index == fanTabIndex )
    updateFanCrosshairs();
  else if ( m_fanControlTab )
  {
    m_fanControlTab->cpuEditor()->clearCrosshair();
    m_fanControlTab->gpuEditor()->clearCrosshair();
    if ( m_fanControlTab->wcFanEditor() )
      m_fanControlTab->wcFanEditor()->clearCrosshair();
    if ( m_fanControlTab->pumpEditor() )
      m_fanControlTab->pumpEditor()->clearCrosshair();
  }

  // Load current keyboard backlight states when keyboard tab (index 4) is activated
  if ( index == 4 )
  {
    if ( m_keyboardVisualizer )
    {
      if ( auto states = m_UccdClient->getKeyboardBacklightStates() )
      {
        // Block visualizer signals to prevent hardware write during state refresh
        m_keyboardVisualizer->blockSignals( true );
        m_keyboardVisualizer->loadCurrentStates( *states );

        // Read brightness from hardware states and sync slider
        if ( QJsonDocument statesDoc = QJsonDocument::fromJson( QString::fromStdString( *states ).toUtf8() );
             statesDoc.isArray() && !statesDoc.array().isEmpty() )
        {
          int hwBrightness = statesDoc.array()[0].toObject()["brightness"].toInt( -1 );
          qDebug() << "[KBD TAB] hw brightness:" << hwBrightness
                   << "slider current:" << m_keyboardBrightnessSlider->value()
                   << "slider max:" << m_keyboardBrightnessSlider->maximum();
          if ( hwBrightness >= 0 && m_keyboardBrightnessSlider )
          {
            m_keyboardBrightnessSlider->blockSignals( true );
            m_keyboardBrightnessSlider->setValue( hwBrightness );
            m_keyboardBrightnessSlider->blockSignals( false );
            m_keyboardBrightnessValueLabel->setText( QString::number( hwBrightness ) );
          }
        }

        // ALWAYS override per-zone brightness with slider value to guarantee sync
        m_keyboardVisualizer->setGlobalBrightness( m_keyboardBrightnessSlider->value() );
        m_keyboardVisualizer->blockSignals( false );
      }
    }

    // Reload keyboard profiles
    reloadKeyboardProfiles();

    // Auto-load the keyboard profile from the active profile's settings
    if ( QString activeProfileId = m_profileManager->activeProfileId(); !activeProfileId.isEmpty() )
    {
      if ( QString profileJson = m_profileManager->getProfileDetails( activeProfileId );
           !profileJson.isEmpty() )
      {
        if ( QJsonDocument doc = QJsonDocument::fromJson( profileJson.toUtf8() ); doc.isObject() )
        {
          QJsonObject obj = doc.object();
          QString keyboardProfileId;

          if ( obj.contains( "selectedKeyboardProfile" ) )
            keyboardProfileId = obj["selectedKeyboardProfile"].toString();

          if ( !keyboardProfileId.isEmpty() )
          {
            // Find by ID in combo userData
            int kbIdx = -1;
            for ( int i = 0; i < m_keyboardProfileCombo->count(); ++i )
            {
              if ( m_keyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
              { kbIdx = i; break; }
            }
            if ( kbIdx >= 0 )
            {
              m_keyboardProfileCombo->blockSignals( true );
              m_keyboardProfileCombo->setCurrentIndex( kbIdx );
              m_keyboardProfileCombo->blockSignals( false );
              // Explicitly load the profile data (setCurrentIndex won't emit
              // if the index was already restored by reloadKeyboardProfiles)
              onKeyboardProfileChanged( m_keyboardProfileCombo->itemData( kbIdx ).toString() );
            }
          }
        }
      }
    }
  }
}

// Profile page slots
void MainWindow::onCustomKeyboardProfilesChanged()
{
  // Remember current selection so we can restore it after rebuild
  QString prevId = m_profileKeyboardProfileCombo->currentData().toString();

  // Repopulate keyboard profile combos with ID userData
  m_profileKeyboardProfileCombo->blockSignals( true );
  m_profileKeyboardProfileCombo->clear();
  for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileKeyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }

  // Restore previous selection by ID
  if ( !prevId.isEmpty() )
  {
    for ( int i = 0; i < m_profileKeyboardProfileCombo->count(); ++i )
    {
      if ( m_profileKeyboardProfileCombo->itemData( i ).toString() == prevId )
      { m_profileKeyboardProfileCombo->setCurrentIndex( i ); break; }
    }
  }
  m_profileKeyboardProfileCombo->blockSignals( false );

  reloadKeyboardProfiles();
}

void MainWindow::onProfileIndexChanged( int index )
{
  if ( index >= 0 )
  {
    QString profileName = m_profileCombo->currentText();
    QString profileId = m_profileCombo->currentData().toString();
    qDebug() << "Profile selected:" << profileName << "at index" << index;
    m_selectedProfileIndex = index;
    loadProfileDetails( profileId );
    m_removeProfileButton->setEnabled( m_profileManager->isCustomProfile( profileId ) );
    m_copyProfileButton->setEnabled( true );
    m_saveButton->setEnabled( true );
    statusBar()->showMessage( "Profile selected: " + profileName + " (click Apply to activate)" );
  }
}

void MainWindow::onAllProfilesChanged()
{
  // Block combo signals to prevent cascading loadProfileDetails calls
  // while we repopulate the list.
  m_profileCombo->blockSignals( true );
  m_profileCombo->clear();
  // Populate combo with name + ID userData
  const QStringList &names = m_profileManager->allProfiles();
  const QJsonArray &defaultData = m_profileManager->defaultProfilesData();
  const QJsonArray &customData = m_profileManager->customProfilesData();
  for ( const auto &p : defaultData )
  {
    if ( p.isObject() )
      m_profileCombo->addItem( p.toObject()["name"].toString(), p.toObject()["id"].toString() );
  }
  for ( const auto &p : customData )
  {
    if ( p.isObject() )
      m_profileCombo->addItem( p.toObject()["name"].toString(), p.toObject()["id"].toString() );
  }
  m_profileCombo->setCurrentIndex( m_profileManager->activeProfileIndex() );
  m_profileCombo->blockSignals( false );
  m_selectedProfileIndex = m_profileManager->activeProfileIndex();

  // Load the active profile details
  if ( QString activeProfileId = m_profileManager->activeProfileId(); !activeProfileId.isEmpty() )
  {
    loadProfileDetails( activeProfileId );
  }
  // Custom profiles may have changed; reload fan profiles (adds custom entries to the fan combo)
  reloadFanProfiles();

  // Override stored keyboard/fan profile with the daemon's live state.
  // A remote client (e.g. tray applet) may have changed the sub-profile
  // while the GUI was not running, or the ProfileChanged signal may have
  // arrived with updated sub-profile IDs.
  {
    QString liveKbId = m_profileManager->activeKeyboardProfileId();
    QString liveFpId = m_profileManager->activeFanProfileId();
    if ( !liveKbId.isEmpty() )
      updateKeyboardEditorFromProfile( liveKbId );
    if ( !liveFpId.isEmpty() )
      updateFanEditorFromProfile( liveFpId );
  }

  // If we were in the middle of saving, mark as complete
  if ( m_saveInProgress )
  {
    m_saveInProgress = false;
    m_profileChanged = false;
    statusBar()->showMessage( "Profile saved successfully" );
    updateButtonStates();
  }

  // Ensure buttons reflect current profile set (remove button availability etc.)
  updateButtonStates();
  m_saveButton->setEnabled( true );
}

void MainWindow::updateConnectionStatusLabel()
{
  if ( !m_connectionLabel )
    return;

  // Only update if currently connected
  if ( !m_UccdClient->isConnected() )
    return;

  QString profileName = m_profileManager->activeProfileName();
  if ( profileName.isEmpty() )
    profileName = QStringLiteral( "Unknown" );

  m_connectionLabel->setText(
    QString( "<span style='color: green;'>●</span> %1" ).arg( profileName ) );
}

void MainWindow::onActiveProfileIndexChanged()
{
  int activeIndex = m_profileManager->activeProfileIndex();
  if ( m_profileCombo->currentIndex() != activeIndex )
  {
    m_profileCombo->blockSignals( true );
    m_profileCombo->setCurrentIndex( activeIndex );
    m_profileCombo->blockSignals( false );
  }
  m_selectedProfileIndex = activeIndex;
}

// Profile detail control slot implementations
void MainWindow::onBrightnessSliderChanged( int value )
{
  m_brightnessValueLabel->setText( QString::number( value ) + "%" );
}

void MainWindow::onCpuCoresChanged( int value )
{
  m_cpuCoresValue->setText( QString::number( value ) );
}

// ---------------------------------------------------------------------------
// Sub-profile sync helpers: update combo + editor without writing to hardware
// ---------------------------------------------------------------------------

void MainWindow::updateKeyboardEditorFromProfile( const QString &keyboardProfileId )
{
  // Update keyboard tab combo
  if ( m_keyboardProfileCombo )
  {
    m_keyboardProfileCombo->blockSignals( true );
    for ( int i = 0; i < m_keyboardProfileCombo->count(); ++i )
    {
      if ( m_keyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
      {
        m_keyboardProfileCombo->setCurrentIndex( i );
        break;
      }
    }
    m_keyboardProfileCombo->blockSignals( false );
  }

  // Update profile-page keyboard combo
  if ( m_profileKeyboardProfileCombo )
  {
    m_profileKeyboardProfileCombo->blockSignals( true );
    for ( int i = 0; i < m_profileKeyboardProfileCombo->count(); ++i )
    {
      if ( m_profileKeyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
      {
        m_profileKeyboardProfileCombo->setCurrentIndex( i );
        break;
      }
    }
    m_profileKeyboardProfileCombo->blockSignals( false );
  }

  // Update the keyboard visualizer without writing to hardware
  QString json = m_profileManager->getKeyboardProfile( keyboardProfileId );
  if ( json.isEmpty() || json == "{}" )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() )
    return;

  QJsonObject obj = doc.object();
  int brightness = obj.value( "brightness" ).toInt( -1 );
  QJsonArray statesArray = obj.value( "states" ).toArray();

  if ( !statesArray.isEmpty() && m_keyboardVisualizer )
  {
    m_keyboardVisualizer->blockSignals( true );
    m_keyboardVisualizer->updateFromJSON( statesArray );
    m_keyboardVisualizer->blockSignals( false );
  }

  if ( brightness >= 0 && m_keyboardBrightnessSlider )
  {
    m_keyboardBrightnessSlider->blockSignals( true );
    m_keyboardBrightnessSlider->setValue( brightness );
    m_keyboardBrightnessSlider->blockSignals( false );
  }

  updateKeyboardProfileButtonStates();
}

void MainWindow::updateFanEditorFromProfile( const QString &fanProfileId )
{
  // Update fan tab combo
  if ( m_fanControlTab && m_fanControlTab->fanProfileCombo() )
  {
    auto *combo = m_fanControlTab->fanProfileCombo();
    combo->blockSignals( true );
    for ( int i = 0; i < combo->count(); ++i )
    {
      if ( combo->itemData( i ).toString() == fanProfileId )
      {
        combo->setCurrentIndex( i );
        break;
      }
    }
    combo->blockSignals( false );
  }

  // Update profile-page fan combo
  if ( m_profileFanProfileCombo )
  {
    m_profileFanProfileCombo->blockSignals( true );
    for ( int i = 0; i < m_profileFanProfileCombo->count(); ++i )
    {
      if ( m_profileFanProfileCombo->itemData( i ).toString() == fanProfileId )
      {
        m_profileFanProfileCombo->setCurrentIndex( i );
        break;
      }
    }
    m_profileFanProfileCombo->blockSignals( false );
  }

  // Load fan curve data into editors without writing to hardware
  onFanProfileChanged( fanProfileId );
}

void MainWindow::onMaxFrequencyChanged( int value )
{
  double freqGHz = value / 1000000.0;  // Convert kHz to GHz for display
  m_maxFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
}

void MainWindow::onODMPowerLimit1Changed( int value )
{
  m_odmPowerLimit1Value->setText( QString::number( value ) + " W" );
}

void MainWindow::onODMPowerLimit2Changed( int value )
{
  m_odmPowerLimit2Value->setText( QString::number( value ) + " W" );
}

void MainWindow::onODMPowerLimit3Changed( int value )
{
  m_odmPowerLimit3Value->setText( QString::number( value ) + " W" );
}

void MainWindow::loadProfileDetails( const QString &profileId )
{
  // Reset change flag when loading a new profile
  m_profileChanged = false;
  m_currentLoadedProfile = profileId;
  updateButtonStates();


  if ( profileId.isEmpty() )
  {
    return;
  }

  // Get the profile JSON from ProfileManager using the profile ID
  QString profileJson = m_profileManager->getProfileDetails( profileId );


  if ( profileJson.isEmpty() )
  {
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson( profileJson.toUtf8() );

  if ( !doc.isObject() )
  {
    return;
  }

  QJsonObject obj = doc.object();
  // Block signals while updating to avoid triggering slot updates
  m_brightnessSlider->blockSignals( true );
  m_setBrightnessCheckBox->blockSignals( true );
  m_profileFanProfileCombo->blockSignals( true );
  m_fanControlTab->fanProfileCombo()->blockSignals( true );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->blockSignals( true );
  m_cpuCoresSlider->blockSignals( true );
  m_governorCombo->blockSignals( true );
  m_minFrequencySlider->blockSignals( true );
  m_maxFrequencySlider->blockSignals( true );
  m_odmPowerLimit1Slider->blockSignals( true );
  m_odmPowerLimit2Slider->blockSignals( true );
  m_odmPowerLimit3Slider->blockSignals( true );
  m_profileKeyboardProfileCombo->blockSignals( true );
  m_keyboardProfileCombo->blockSignals( true );
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->blockSignals( true );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->blockSignals( true );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->blockSignals( true );
  m_mainsButton->blockSignals( true );
  m_batteryButton->blockSignals( true );
  m_waterCoolerButton->blockSignals( true );
  if ( m_descriptionEdit ) m_descriptionEdit->blockSignals( true );

  // Load Display settings (nested in display object)

  if ( obj.contains( "display" ) && obj["display"].isObject() )
  {
    QJsonObject displayObj = obj["display"].toObject();


    if ( displayObj.contains( "brightness" ) )
    {
      int brightness = displayObj["brightness"].toInt( 100 );
      m_brightnessSlider->setValue( brightness );
    }


    if ( displayObj.contains( "useBrightness" ) )
    {
      bool useBrightness = displayObj["useBrightness"].toBool( false );
      m_setBrightnessCheckBox->setChecked( useBrightness );
    }
  }

  // Load Fan Control settings (nested in fan object)

  // Load description
  if ( m_descriptionEdit )
    m_descriptionEdit->setPlainText( obj["description"].toString() );

  QString loadedFanProfile;
  bool fanProfileNotFound = false;
  QString missingFanProfile;
  if ( obj.contains( "fan" ) && obj["fan"].isObject() )
  {
    QJsonObject fanObj = obj["fan"].toObject();


    if ( fanObj.contains( "fanProfile" ) )
    {
      QString fanProfileRef = fanObj["fanProfile"].toString( "fan-balanced" );
      int idx = -1;
      for ( int i = 0; i < m_profileFanProfileCombo->count(); ++i )
      {
        if ( m_profileFanProfileCombo->itemData( i ).toString() == fanProfileRef )
        {
          idx = i;
          break;
        }
      }

      if ( idx >= 0 )
      {
        m_profileFanProfileCombo->setCurrentIndex( idx );
        m_fanControlTab->fanProfileCombo()->setCurrentIndex( idx );
        loadedFanProfile = m_profileFanProfileCombo->itemData( idx ).toString();
      }
      else
      {
        // Referenced fan profile was not found
        fanProfileNotFound = true;
        missingFanProfile = fanProfileRef;
      }
    }

    // Load sameSpeed from profile (default true)
    if ( fanObj.contains( "sameSpeed" ) )
      m_sameFanSpeedCheckBox->setChecked( fanObj["sameSpeed"].toBool( true ) );
    else
      m_sameFanSpeedCheckBox->setChecked( true );

    if ( fanObj.contains( "autoControlWC" ) )
    {
      bool autoControl = fanObj["autoControlWC"].toBool( true );
      m_autoWaterControlCheckBox->setChecked( autoControl );
      if ( m_fanControlTab )
        m_fanControlTab->setWaterCoolerAutoControl( autoControl );
    }
    else
    {
      m_autoWaterControlCheckBox->setChecked( true );
      if ( m_fanControlTab )
        m_fanControlTab->setWaterCoolerAutoControl( true );
    }

    // Query the daemon directly for the runtime water-cooler enable state
    bool wcEnable = m_UccdClient->isWaterCoolerEnabled().value_or(
        fanObj["enableWaterCooler"].toBool( true ) );
    m_fanControlTab->setWaterCoolerEnabled( wcEnable );
    m_dashboardTab->setWaterCoolerEnabled( wcEnable );
  }

  // Load CPU settings (nested in cpu object)

  if ( obj.contains( "cpu" ) && obj["cpu"].isObject() )
  {
    QJsonObject cpuObj = obj["cpu"].toObject();


    if ( cpuObj.contains( "onlineCores" ) )
      m_cpuCoresSlider->setValue( cpuObj["onlineCores"].toInt( m_cpuCoresSlider->maximum() ) );

    if ( cpuObj.contains( "governor" ) )
    {
      if ( int index = m_governorCombo->findData( cpuObj["governor"].toString() ); index >= 0 )
        m_governorCombo->setCurrentIndex( index );
      else
        m_governorCombo->setCurrentIndex( 0 ); // default to first
    }

    if ( cpuObj.contains( "energyPerformancePreference" ) && m_eppCombo )
    {
      if ( int index = m_eppCombo->findData( cpuObj["energyPerformancePreference"].toString() ); index >= 0 )
        m_eppCombo->setCurrentIndex( index );
      else
        m_eppCombo->setCurrentIndex( 0 );
    }

    // Get hardware frequency limits and set slider ranges
    if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
      if ( doc.isObject() )
      {
        QJsonObject limitsObj = doc.object();
        int minFreqKHz = limitsObj["min"].toInt( 400000 );   // hardware min frequency in kHz
        int maxFreqKHz = limitsObj["max"].toInt( 6000000 );  // hardware max frequency in kHz

        m_cpuMinFreqKHz = minFreqKHz;
        m_cpuMaxFreqKHz = maxFreqKHz;
        m_minFrequencySlider->setMinimum( minFreqKHz );
        m_minFrequencySlider->setMaximum( maxFreqKHz );
        m_maxFrequencySlider->setMinimum( minFreqKHz );
        m_maxFrequencySlider->setMaximum( maxFreqKHz );
      }
    }

    // Load frequency values in MHz (convert from kHz stored in profile)
    if ( cpuObj.contains( "scalingMinFrequency" ) )
    {
      int requestedKHz = cpuObj["scalingMinFrequency"].toInt( 1000000 );
      m_minFrequencySlider->setValue( requestedKHz );
    }

    if ( cpuObj.contains( "scalingMaxFrequency" ) )
    {
      int requestedKHz = cpuObj["scalingMaxFrequency"].toInt( 5000000 );
      m_maxFrequencySlider->setValue( requestedKHz );
    }
  }
  else
  {
    // Profile loading failed, still try to set slider limits from hardware
    if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
      if ( doc.isObject() )
      {
        QJsonObject limitsObj = doc.object();
        int minFreqKHz = limitsObj["min"].toInt( 400000 );
        int maxFreqKHz = limitsObj["max"].toInt( 6000000 );
        m_minFrequencySlider->setMinimum( minFreqKHz );
        m_minFrequencySlider->setMaximum( maxFreqKHz );
        m_maxFrequencySlider->setMinimum( minFreqKHz );
        m_maxFrequencySlider->setMaximum( maxFreqKHz );
      }
    }
  }

  // Load ODM Power Limits (TDP) settings (nested in odmPowerLimits object)
  // First, set slider ranges from hardware limits
  std::vector< int > hardwareLimits = m_profileManager->getHardwarePowerLimits();
  if ( hardwareLimits.size() > 0 )
  {
    m_odmPowerLimit1Slider->setMaximum( hardwareLimits[0] );
  }

  if ( hardwareLimits.size() > 1 )
  {
    m_odmPowerLimit2Slider->setMaximum( hardwareLimits[1] );
  }

  if ( hardwareLimits.size() > 2 )
  {
    m_odmPowerLimit3Slider->setMaximum( hardwareLimits[2] );
  }

  // Then, set slider values from profile

  if ( obj.contains( "odmPowerLimits" ) && obj["odmPowerLimits"].isObject() )
  {
    QJsonObject odmLimitsObj = obj["odmPowerLimits"].toObject();


    if ( odmLimitsObj.contains( "tdpValues" ) && odmLimitsObj["tdpValues"].isArray() )
    {
      QJsonArray tdpArray = odmLimitsObj["tdpValues"].toArray();

      // Load actual values from profile - these are the current settings

      if ( tdpArray.size() > 0 )
      {
        int val0 = tdpArray[0].toInt();
        m_odmPowerLimit1Slider->setValue( val0 );
      }


      if ( tdpArray.size() > 1 )
      {
        int val1 = tdpArray[1].toInt();
        m_odmPowerLimit2Slider->setValue( val1 );
      }


      if ( tdpArray.size() > 2 )
      {
        int val2 = tdpArray[2].toInt();
        m_odmPowerLimit3Slider->setValue( val2 );
      }
    }
  }
  // Load GPU OC profile reference
  if ( m_profileGpuProfileCombo )
  {
    m_profileGpuProfileCombo->blockSignals( true );
    QString gpuProfileId;
    int idx = -1;
    if ( obj.contains( "gpuProfileId" ) )
    {
      gpuProfileId = obj["gpuProfileId"].toString();
      for ( int i = 0; i < m_profileGpuProfileCombo->count(); ++i )
      {
        if ( m_profileGpuProfileCombo->itemData( i ).toString() == gpuProfileId )
        { idx = i; break; }
      }
      if ( idx >= 0 )
        m_profileGpuProfileCombo->setCurrentIndex( idx );
      else
        m_profileGpuProfileCombo->setCurrentIndex( 0 ); // (None)
    }
    else
    {
      m_profileGpuProfileCombo->setCurrentIndex( 0 ); // (None)
    }
    m_profileGpuProfileCombo->blockSignals( false );

    if ( m_gpuProfileTab && m_gpuProfileTab->gpuProfileCombo() )
    {
      auto *gpuCombo = m_gpuProfileTab->gpuProfileCombo();
      gpuCombo->blockSignals( true );

      if ( !gpuProfileId.isEmpty() )
      {
        int gpuTabIdx = gpuCombo->findData( gpuProfileId );
        if ( gpuTabIdx >= 0 )
          gpuCombo->setCurrentIndex( gpuTabIdx );
      }

      gpuCombo->blockSignals( false );

      if ( !gpuProfileId.isEmpty() )
        onGpuProfileChanged( gpuProfileId );
    }
  }

  // Load Charging profile setting
  if ( m_profileChargingProfileCombo && obj.contains( "chargingProfile" ) )
  {
    if ( int idx = m_profileChargingProfileCombo->findData( obj["chargingProfile"].toString() ); idx >= 0 )
      m_profileChargingProfileCombo->setCurrentIndex( idx );
    else
      m_profileChargingProfileCombo->setCurrentIndex( 0 );
  }
  else if ( m_profileChargingProfileCombo )
  {
    m_profileChargingProfileCombo->setCurrentIndex( 0 );
  }

  // Load Charging priority setting
  if ( m_profileChargingPriorityCombo && obj.contains( "chargingPriority" ) )
  {
    if ( int idx = m_profileChargingPriorityCombo->findData( obj["chargingPriority"].toString() ); idx >= 0 )
      m_profileChargingPriorityCombo->setCurrentIndex( idx );
    else
      m_profileChargingPriorityCombo->setCurrentIndex( 0 );
  }
  else if ( m_profileChargingPriorityCombo )
  {
    m_profileChargingPriorityCombo->setCurrentIndex( 0 );
  }

  // Load Charge limit setting (maps chargeType + thresholds to combo selection)
  if ( m_profileChargeLimitCombo )
  {
    QString chargeType = obj.value( "chargeType" ).toString();
    int startThr = obj.value( "chargeStartThreshold" ).toInt( -1 );
    int endThr = obj.value( "chargeEndThreshold" ).toInt( -1 );

    if ( chargeType == "Custom" && startThr == 60 && endThr == 90 )
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "reduced" ) );
    else if ( chargeType == "Custom" && startThr == 40 && endThr == 80 )
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "stationary" ) );
    else
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "full" ) );
  }

  // Load Keyboard settings - check for embedded keyboard profile name
  QString loadedKeyboardProfile;
  bool keyboardProfileNotFound = false;
  QString missingKeyboardProfile;
  if ( obj.contains( "selectedKeyboardProfile" ) )
  {
    QString keyboardProfileId = obj["selectedKeyboardProfile"].toString();
    // Find by ID in combo userData
    int idx = -1;
    for ( int i = 0; i < m_profileKeyboardProfileCombo->count(); ++i )
    {
      if ( m_profileKeyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
      { idx = i; break; }
    }
    if ( idx >= 0 )
    {
      m_profileKeyboardProfileCombo->setCurrentIndex( idx );
      m_keyboardProfileCombo->setCurrentIndex( idx );
      loadedKeyboardProfile = m_profileKeyboardProfileCombo->itemData( idx ).toString();
    }
    else
    {
      // Referenced keyboard profile was not found
      keyboardProfileNotFound = true;
      missingKeyboardProfile = keyboardProfileId;
    }
  }
  // Keyboard brightness and colors are managed by the keyboard profile system
  // (via selectedKeyboardProfile), not directly from system profile data.
  // "keyboard" object in profiles is ignored to avoid overriding
  // the hardware brightness with stale saved values.

  // Load power state activation settings
  if ( QString settingsJson = m_profileManager->getSettingsJSON(); !settingsJson.isEmpty() )
  {
    if ( QJsonDocument settingsDoc = QJsonDocument::fromJson( settingsJson.toUtf8() );
         settingsDoc.isObject() )
    {
      QJsonObject settingsObj = settingsDoc.object();
      if ( settingsObj.contains( "stateMap" ) && settingsObj["stateMap"].isObject() )
      {
        QJsonObject stateMap = settingsObj["stateMap"].toObject();
        QString mainsProfile = stateMap["power_ac"].toString();
        QString batteryProfile = stateMap["power_bat"].toString();
        QString wcProfile = stateMap["power_wc"].toString();

        m_mainsButton->setChecked( mainsProfile == profileId );
        m_batteryButton->setChecked( batteryProfile == profileId );
        m_waterCoolerButton->setChecked( wcProfile == profileId );

        // Store the loaded power state assignments
        m_loadedMainsAssignment = (mainsProfile == profileId);
        m_loadedBatteryAssignment = (batteryProfile == profileId);
        m_loadedWaterCoolerAssignment = (wcProfile == profileId);
      }
    }
  }

  // Unblock signals
  m_brightnessSlider->blockSignals( false );
  m_setBrightnessCheckBox->blockSignals( false );
  m_profileFanProfileCombo->blockSignals( false );
  m_fanControlTab->fanProfileCombo()->blockSignals( false );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->blockSignals( false );

  // Set initial auto control state for water cooler
  if ( m_fanControlTab && m_autoWaterControlCheckBox )
    m_fanControlTab->setWaterCoolerAutoControl( m_autoWaterControlCheckBox->isChecked() );
  m_cpuCoresSlider->blockSignals( false );
  m_governorCombo->blockSignals( false );
  m_minFrequencySlider->blockSignals( false );
  m_maxFrequencySlider->blockSignals( false );
  m_odmPowerLimit1Slider->blockSignals( false );
  m_odmPowerLimit2Slider->blockSignals( false );
  m_odmPowerLimit3Slider->blockSignals( false );
  m_profileKeyboardProfileCombo->blockSignals( false );
  m_keyboardProfileCombo->blockSignals( false );
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->blockSignals( false );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->blockSignals( false );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->blockSignals( false );
  m_mainsButton->blockSignals( false );
  m_batteryButton->blockSignals( false );
  m_waterCoolerButton->blockSignals( false );
  if ( m_descriptionEdit ) m_descriptionEdit->blockSignals( false );

  // Trigger label updates by calling the slots directly
  onBrightnessSliderChanged( m_brightnessSlider->value() );
  onCpuCoresChanged( m_cpuCoresSlider->value() );
  onMaxFrequencyChanged( m_maxFrequencySlider->value() );
  onODMPowerLimit1Changed( m_odmPowerLimit1Slider->value() );
  onODMPowerLimit2Changed( m_odmPowerLimit2Slider->value() );
  onODMPowerLimit3Changed( m_odmPowerLimit3Slider->value() );
  // Trigger fan profile change if one was loaded (loads fan curve data for display only)
  if ( !loadedFanProfile.isEmpty() )
  {
    onFanProfileChanged( loadedFanProfile );
  }

  // Load keyboard profile for display only — must NOT write to hardware.
  // onKeyboardProfileChanged() pushes states to the daemon, which would revert
  // live changes made by the tray applet while the GUI was not running.
  // Use updateKeyboardEditorFromProfile() which only updates the UI widgets.
  if ( loadedKeyboardProfile.isEmpty() && m_keyboardProfileCombo->count() > 0 )
    loadedKeyboardProfile = m_keyboardProfileCombo->currentData().toString();

  if ( !loadedKeyboardProfile.isEmpty() )
    updateKeyboardEditorFromProfile( loadedKeyboardProfile );


  // Enable/disable editing widgets based on whether profile is custom
  const bool isCustom = m_profileManager ? m_profileManager->isCustomProfile( profileId ) : false;
  updateProfileEditingWidgets( isCustom );

  // Warn user if any referenced profiles were not found (deleted after profile creation)
  if ( fanProfileNotFound || keyboardProfileNotFound )
  {
    QString profileName = m_profileCombo->currentText();
    QString message = QString( "The profile '%1' references the following missing subprofiles:\n\n" ).arg( profileName );

    if ( keyboardProfileNotFound )
      message += QString( "Keyboard profile: %1\n" ).arg( missingKeyboardProfile );

    if ( fanProfileNotFound )
      message += QString( "Fan profile: %1\n" ).arg( missingFanProfile );

    message += "\nNew profiles for the missing subprofile(s) have been assigned for this session but it is not saved.";
    QMessageBox::warning( this, "Missing Subprofile References", message );
  }

}

void MainWindow::updateProfileEditingWidgets( bool isCustom )
{
  // Enable/disable editing widgets based on whether profile is custom

  // Description edit
  if ( m_descriptionEdit ) {
    m_descriptionEdit->setEnabled( isCustom );
    m_descriptionEdit->setReadOnly( !isCustom );
  }

  // Profile combo - allow renaming only for custom profiles
  if ( m_profileCombo && m_profileCombo->lineEdit() ) {
    m_profileCombo->lineEdit()->setReadOnly( !isCustom );
  }

  // Auto-activate buttons (always enabled for power state assignment)
  if ( m_mainsButton ) m_mainsButton->setEnabled( true );
  if ( m_batteryButton ) m_batteryButton->setEnabled( true );

  // Display controls
  if ( m_setBrightnessCheckBox ) m_setBrightnessCheckBox->setEnabled( isCustom );
  if ( m_brightnessSlider ) m_brightnessSlider->setEnabled( isCustom );

  // Fan controls
  if ( m_profileFanProfileCombo ) m_profileFanProfileCombo->setEnabled( isCustom );
  if ( m_sameFanSpeedCheckBox ) m_sameFanSpeedCheckBox->setEnabled( isCustom );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->setEnabled( isCustom );

  // CPU controls
  if ( m_cpuCoresSlider ) m_cpuCoresSlider->setEnabled( isCustom );
  if ( m_governorCombo ) m_governorCombo->setEnabled( isCustom );
  if ( m_eppCombo ) m_eppCombo->setEnabled( isCustom );
  if ( m_minFrequencySlider ) m_minFrequencySlider->setEnabled( isCustom );
  if ( m_maxFrequencySlider ) m_maxFrequencySlider->setEnabled( isCustom );

  // Keyboard profile
  if ( m_profileKeyboardProfileCombo ) m_profileKeyboardProfileCombo->setEnabled( isCustom );

  // ODM Power controls
  // Built-in profiles are immutable on disk, but TDP values can still be
  // adjusted and applied temporarily to the active hardware.
  const std::vector< int > tdpLimits = m_profileManager
    ? m_profileManager->getHardwarePowerLimits()
    : std::vector< int >{};
  if ( m_odmPowerLimit1Slider ) m_odmPowerLimit1Slider->setEnabled( tdpLimits.size() > 0 );
  if ( m_odmPowerLimit2Slider ) m_odmPowerLimit2Slider->setEnabled( tdpLimits.size() > 1 );
  if ( m_odmPowerLimit3Slider ) m_odmPowerLimit3Slider->setEnabled( tdpLimits.size() > 2 );

  // Charging profile
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->setEnabled( isCustom );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->setEnabled( isCustom );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->setEnabled( isCustom );
}

void MainWindow::markChanged()
{
  m_profileChanged = true;
  updateButtonStates();
}

void MainWindow::updateButtonStates( void)
{
  // Update profile page buttons if available
  if ( profileTopWidgetsAvailable() )
  {
    m_removeProfileButton->setEnabled( m_profileManager->isCustomProfile( m_profileCombo->currentData().toString() ) );
  }

  // Delegate fan profile button states to FanControlTab
  if ( m_fanControlTab )
    m_fanControlTab->updateButtonStates( m_UccdClient->isConnected() );
}

QString MainWindow::buildProfileJSON() const
{
  QString profileId   = m_profileCombo->currentData().toString();
  QString profileName = m_profileCombo->currentText();

  QJsonObject profileObj;
  profileObj["id"]          = profileId;
  profileObj["name"]        = profileName;
  profileObj["description"] = m_descriptionEdit->toPlainText();

  // Brightness
  QJsonObject displayObj;
  if ( m_setBrightnessCheckBox->isChecked() )
    displayObj["brightness"] = m_brightnessSlider->value();
  profileObj["display"] = displayObj;

  // Fan — embed complete fan profile tables
  QJsonObject fanObj;
  QString fanProfileId  = m_profileFanProfileCombo->currentData().toString();
  QString fanProfileJSON = m_profileManager->getFanProfile( fanProfileId );
  if ( !fanProfileJSON.isEmpty() && fanProfileJSON != "{}" )
  {
    QJsonDocument fanDoc = QJsonDocument::fromJson( fanProfileJSON.toUtf8() );
    if ( fanDoc.isObject() )
    {
      QJsonObject fp = fanDoc.object();
      if ( fp.contains( "tableCPU" ) )           fanObj["tableCPU"]           = fp["tableCPU"];
      if ( fp.contains( "tableGPU" ) )           fanObj["tableGPU"]           = fp["tableGPU"];
      if ( fp.contains( "tablePump" ) )          fanObj["tablePump"]          = fp["tablePump"];
      if ( fp.contains( "tableWaterCoolerFan" ) ) fanObj["tableWaterCoolerFan"] = fp["tableWaterCoolerFan"];
    }
  }
  fanObj["fanProfile"]       = fanProfileId;
  fanObj["sameSpeed"]        = m_sameFanSpeedCheckBox   ? m_sameFanSpeedCheckBox->isChecked()   : true;
  fanObj["autoControlWC"]    = m_autoWaterControlCheckBox ? m_autoWaterControlCheckBox->isChecked() : true;
  fanObj["enableWaterCooler"] = m_fanControlTab          ? m_fanControlTab->isWaterCoolerEnabled() : true;
  profileObj["fan"] = fanObj;

  // CPU
  QJsonObject cpuObj;
  cpuObj["onlineCores"]                = m_cpuCoresSlider->value();
  cpuObj["governor"]                   = m_governorCombo->currentData().toString();
  cpuObj["energyPerformancePreference"] = m_eppCombo ? m_eppCombo->currentData().toString() : QString();
  cpuObj["scalingMinFrequency"]         = std::clamp( m_minFrequencySlider->value(), m_cpuMinFreqKHz, m_cpuMaxFreqKHz );
  cpuObj["scalingMaxFrequency"]         = std::clamp( m_maxFrequencySlider->value(), m_cpuMinFreqKHz, m_cpuMaxFreqKHz );
  profileObj["cpu"] = cpuObj;

  // ODM Power Limits (TDP)
  QJsonObject odmObj;
  QJsonArray tdpArray;
  tdpArray.append( m_odmPowerLimit1Slider->value() );
  tdpArray.append( m_odmPowerLimit2Slider->value() );
  tdpArray.append( m_odmPowerLimit3Slider->value() );
  odmObj["tdpValues"] = tdpArray;
  profileObj["odmPowerLimits"] = odmObj;

  // GPU OC profile — embed complete GPU OC data (like keyboard data)
  if ( m_profileGpuProfileCombo )
  {
    QString gpuProfileId = m_profileGpuProfileCombo->currentData().toString();
    if ( !gpuProfileId.isEmpty() )
    {
      profileObj["gpuProfileId"] = gpuProfileId;

      // Resolve and embed full GPU OC profile data so the daemon can apply it
      // at startup / power-state change without needing the GUI.
      QString gpuProfileJSON = m_profileManager->getGpuProfile( gpuProfileId );
      if ( !gpuProfileJSON.isEmpty() && gpuProfileJSON != "{}" )
      {
        QJsonDocument gpuDoc = QJsonDocument::fromJson( gpuProfileJSON.toUtf8() );
        if ( gpuDoc.isObject() )
        {
          profileObj["gpuOCProfileData"] = gpuDoc.object();
        }
      }
    }
  }

  // Keyboard — embed complete keyboard profile data
  QJsonObject keyboardObj;
  QString keyboardProfileId  = m_profileKeyboardProfileCombo->currentData().toString();
  QString keyboardProfileJSON = m_profileManager->getKeyboardProfile( keyboardProfileId );
  if ( !keyboardProfileJSON.isEmpty() && keyboardProfileJSON != "{}" )
  {
    QJsonDocument kbDoc = QJsonDocument::fromJson( keyboardProfileJSON.toUtf8() );
    if ( kbDoc.isObject() )
      keyboardObj = kbDoc.object();
    else if ( kbDoc.isArray() )
      keyboardObj["states"] = kbDoc.array();
  }
  else
  {
    if ( auto keyboardStates = m_UccdClient->getKeyboardBacklightStates() )
      keyboardObj["states"] = QJsonDocument::fromJson( QString::fromStdString( *keyboardStates ).toUtf8() ).array();
  }
  keyboardObj["keyboardProfileName"] = m_profileKeyboardProfileCombo->currentText();
  if ( m_keyboardBrightnessSlider )
    keyboardObj["brightness"] = m_keyboardBrightnessSlider->value();
  profileObj["keyboard"]               = keyboardObj;
  profileObj["selectedKeyboardProfile"] = keyboardProfileId;

  // Charging
  if ( m_profileChargingProfileCombo )
  {
    QString v = m_profileChargingProfileCombo->currentData().toString();
    if ( !v.isEmpty() ) profileObj["chargingProfile"] = v;
  }
  if ( m_profileChargingPriorityCombo )
  {
    QString v = m_profileChargingPriorityCombo->currentData().toString();
    if ( !v.isEmpty() ) profileObj["chargingPriority"] = v;
  }
  if ( m_profileChargeLimitCombo )
  {
    QString limitPreset = m_profileChargeLimitCombo->currentData().toString();
    if ( limitPreset == "full" )
    {
      profileObj["chargeType"] = "Standard";
    }
    else if ( limitPreset == "reduced" )
    {
      profileObj["chargeType"]           = "Custom";
      profileObj["chargeStartThreshold"] = 60;
      profileObj["chargeEndThreshold"]   = 90;
    }
    else if ( limitPreset == "stationary" )
    {
      profileObj["chargeType"]           = "Custom";
      profileObj["chargeStartThreshold"] = 40;
      profileObj["chargeEndThreshold"]   = 80;
    }
  }

  return QString::fromUtf8( QJsonDocument( profileObj ).toJson() );
}

void MainWindow::onApplyClicked()
{
  if ( m_selectedProfileIndex < 0 )
  {
    statusBar()->showMessage( "No profile selected" );
    return;
  }

  QString profileJSON = buildProfileJSON();
  m_profileManager->getClient()->applyProfile( profileJSON.toStdString() );

  // Re-send the current water cooler enable state so that
  // the profile apply doesn't override the user's checkbox state.
  if ( m_fanControlTab )
    m_fanControlTab->sendWaterCoolerEnable( m_fanControlTab->isWaterCoolerEnabled() );

  statusBar()->showMessage( "Profile applied: " + m_profileCombo->currentText() );
}

void MainWindow::onSaveClicked()
{
  QString profileName = m_profileCombo->currentText();
  QString profileId = m_profileCombo->currentData().toString();
  const bool isCustom = m_profileManager->isCustomProfile( profileId );

  if ( isCustom )
  {
    m_profileManager->saveProfile( buildProfileJSON() );
  }

  // For both custom and built-in profiles, update stateMap based on mains/battery button states
  // Batch all stateMap changes into a single D-Bus call (single backup + write)
  std::map< QString, QString > stateMapUpdates;
  if ( m_mainsButton->isChecked() )
    stateMapUpdates["power_ac"] = profileId;
  if ( m_batteryButton->isChecked() )
    stateMapUpdates["power_bat"] = profileId;
  if ( m_waterCoolerButton->isChecked() )
    stateMapUpdates["power_wc"] = profileId;

  if ( !stateMapUpdates.empty() )
    m_profileManager->setBatchStateMap( stateMapUpdates );

  // Indicate saving; actual success will be reflected when ProfileManager signals
  m_saveInProgress = true;
  statusBar()->showMessage( "Saving profile..." );
  updateButtonStates();
}

void MainWindow::onSaveFanProfilesClicked()
{
  // Save fan profiles via DBus
  saveFanPoints();
  statusBar()->showMessage( "Fan profiles saved" );
}

void MainWindow::onAddProfileClicked()
{
  // Generate a unique profile name
  QString baseName = "New Profile";
  QString profileName;
  int counter = 1;

  do {
    profileName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  } while (m_profileManager->allProfiles().contains(profileName));

  // Create profile from default
  QString profileJson = m_profileManager->createProfileFromDefault(profileName);
  if (!profileJson.isEmpty()) {
    statusBar()->showMessage( QString("Profile '%1' created successfully").arg(profileName) );

    // Switch to the newly created profile
    if (int newIndex = m_profileCombo->findText(profileName); newIndex != -1) {
      m_profileCombo->setCurrentIndex(newIndex);
    }
  }
  else
    QMessageBox::warning(this, "Error", "Failed to create new profile.");
}
void MainWindow::onCopyProfileClicked()
{
  QString current = m_profileCombo->currentText();

  // Strip " [Built-in]" suffix if present (built-in profiles shouldn't keep this marker when copied)
  if ( current.endsWith( " [Built-in]" ) )
    current = current.left( current.size() - 11 ); // 11 is length of " [Built-in]"

  // Allow copying any profile (built-in or custom)
  QString profileId = m_profileCombo->currentData().toString();
  QString json = m_profileManager->getProfileDetails(profileId);
  if (json.isEmpty()) {
    QMessageBox::warning(this, "Error", "Failed to get profile data.");
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (!doc.isObject()) {
    QMessageBox::warning(this, "Error", "Invalid profile data.");
    return;
  }

  QJsonObject obj = doc.object();

  // Generate a new unique ID for the copied profile
  obj["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

  // Generate new name: "New {name}" with optional incrementing number
  QString baseName = QString("New %1").arg(current);
  QString newName = baseName;
  int counter = 1;
  while (m_profileManager->allProfiles().contains(newName)) {
    newName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  }

  // Set new name
  obj["name"] = newName;

  // Save
  QString newJson = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  m_profileManager->saveProfile(newJson);

  // Switch
  if (int newIndex = m_profileCombo->findText(newName); newIndex != -1) {
    m_profileCombo->setCurrentIndex(newIndex);
  }

  statusBar()->showMessage( QString("Profile '%1' copied to '%2'").arg(current).arg(newName) );
}

void MainWindow::onRemoveProfileClicked()
{
  QString currentProfile = m_profileCombo->currentText();
  QString currentProfileId = m_profileCombo->currentData().toString();

  // Check if it's a built-in profile
  if (!m_profileManager->isCustomProfile(currentProfileId)) {
    QMessageBox::information(this, "Cannot Remove",
                            "Built-in profiles cannot be removed.");
    return;
  }

  // Confirm deletion
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove Profile",
    QString("Are you sure you want to remove the profile '%1'?").arg(currentProfile),
    QMessageBox::Yes | QMessageBox::No
  );

  if (reply == QMessageBox::Yes) {
    m_profileManager->deleteProfile(currentProfileId);
    statusBar()->showMessage( QString("Profile '%1' removed").arg(currentProfile) );
  }
}

void MainWindow::onProfileComboRenamed()
{
  if ( !m_profileCombo || !m_profileCombo->lineEdit() ) return;

  int idx = m_profileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString oldName = m_profileCombo->itemText( idx );
  QString newName = m_profileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    // Revert if empty or unchanged
    m_profileCombo->setEditText( oldName );
    return;
  }

  if ( !m_profileManager->isCustomProfile( m_profileCombo->itemData( idx ).toString() ) ) {
    // Cannot rename built-in profiles
    m_profileCombo->setEditText( oldName );
    return;
  }

  // Get profile data and update the name
  QString profileId = m_profileCombo->itemData( idx ).toString();
  QString json = m_profileManager->getProfileDetails( profileId );
  if ( json.isEmpty() ) {
    m_profileCombo->setEditText( oldName );
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() ) {
    m_profileCombo->setEditText( oldName );
    return;
  }

  QJsonObject obj = doc.object();
  obj["name"] = newName;

  // Save with new name (ProfileManager matches by ID and updates the name)
  QJsonDocument out( obj );
  m_profileManager->saveProfile( QString::fromUtf8( out.toJson( QJsonDocument::Compact ) ) );

  // Select the renamed profile after the combo gets repopulated
  int newIdx = m_profileCombo->findText( newName );
  if ( newIdx != -1 )
    m_profileCombo->setCurrentIndex( newIdx );

  statusBar()->showMessage( QString("Profile renamed from '%1' to '%2'").arg( oldName, newName ) );
}

void MainWindow::onKeyboardProfileComboRenamed()
{
  if ( !m_keyboardProfileCombo || !m_keyboardProfileCombo->lineEdit() ) return;

  int idx = m_keyboardProfileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString oldName = m_keyboardProfileCombo->itemText( idx );
  QString newName = m_keyboardProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    m_keyboardProfileCombo->setEditText( oldName );
    return;
  }

  // "Default" is built-in
  QString keyboardProfileId = m_keyboardProfileCombo->itemData( idx ).toString();
  if ( keyboardProfileId.isEmpty() || !m_profileManager->customKeyboardProfiles().contains( oldName ) ) {
    m_keyboardProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameKeyboardProfile( keyboardProfileId, newName ) ) {
    m_keyboardProfileCombo->setItemText( idx, newName );
    statusBar()->showMessage( QString("Keyboard profile renamed from '%1' to '%2'").arg( oldName, newName ) );
    updateKeyboardProfileButtonStates();
  } else {
    m_keyboardProfileCombo->setEditText( oldName );
    statusBar()->showMessage( "Failed to rename keyboard profile", 3000 );
  }
}

void MainWindow::onRemoveFanProfileClicked()
{
  QString currentProfile = m_fanControlTab->fanProfileCombo()->currentText();
  QString fanProfileId = m_fanControlTab->fanProfileCombo()->currentData().toString();

  // Check if it's a built-in profile
  if ( m_fanControlTab->builtinFanProfiles().contains( currentProfile ) ) {
    QMessageBox::information(this, "Cannot Remove",
                            "Built-in fan profiles cannot be removed.");
    return;
  }

  // Check if any system profiles reference this fan profile
  QStringList referencingProfiles;
  auto checkProfiles = [&]( const QJsonArray &profiles ) {
    for ( const auto &p : profiles )
    {
      if ( !p.isObject() ) continue;
      QJsonObject obj = p.toObject();
      QString name = obj["name"].toString();
      if ( obj.contains( "fan" ) && obj["fan"].isObject() )
      {
        QString ref = obj["fan"].toObject()["fanProfile"].toString();
        if ( ref == fanProfileId || ref == currentProfile )
          referencingProfiles << name;
      }
    }
  };
  checkProfiles( m_profileManager->defaultProfilesData() );
  checkProfiles( m_profileManager->customProfilesData() );

  // Build confirmation message
  QString confirmMessage;
  if ( !referencingProfiles.isEmpty() )
  {
    confirmMessage = QString( "The fan profile '%1' is referenced by the following system profiles:\n\n" ).arg( currentProfile );
    for ( const QString &name : referencingProfiles )
      confirmMessage += QString( "  - %1\n" ).arg( name );
    confirmMessage += "\nAre you sure you want to remove this fan profile?";
  }
  else
  {
    confirmMessage = QString( "Are you sure you want to remove the fan profile '%1'?" ).arg( currentProfile );
  }

  // Confirm deletion
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove Fan Profile",
    confirmMessage,
    QMessageBox::Yes | QMessageBox::No
  );

  if (reply == QMessageBox::Yes) {
    // Remove from persistent storage and UI
    if ( m_profileManager->deleteFanProfile( fanProfileId ) ) {
      // Remove from both fan profile lists
      int idx = m_fanControlTab->fanProfileCombo()->currentIndex();
      if ( idx >= 0 ) m_fanControlTab->fanProfileCombo()->removeItem( idx );
      if ( m_profileFanProfileCombo ) {
        int idx2 = m_profileFanProfileCombo->findText( currentProfile );
        if ( idx2 != -1 ) m_profileFanProfileCombo->removeItem( idx2 );
      }
      statusBar()->showMessage( QString("Fan profile '%1' removed").arg(currentProfile) );
    } else {
      QMessageBox::warning(this, "Remove Failed", "Failed to remove custom fan profile.");
    }
  }
}

void MainWindow::onFanProfileChanged(const QString& fanProfileId)
{
  QString json = m_profileManager->getFanProfile(fanProfileId);
  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (doc.isObject()) {
    QJsonObject obj = doc.object();

    // Load CPU points
    if (obj.contains("tableCPU")) {
      QJsonArray arr = obj["tableCPU"].toArray();
      QVector<FanCurveEditorWidget::Point> cpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        cpuPoints.append({temp, speed});
      }
      if (m_fanControlTab->cpuEditor() && !cpuPoints.isEmpty()) {
        m_fanControlTab->cpuEditor()->setPoints(cpuPoints);
      }
    }

    // Load GPU points
    if (obj.contains("tableGPU")) {
      QJsonArray arr = obj["tableGPU"].toArray();
      QVector<FanCurveEditorWidget::Point> gpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        gpuPoints.append({temp, speed});
      }
      if (m_fanControlTab->gpuEditor() && !gpuPoints.isEmpty()) {
        m_fanControlTab->gpuEditor()->setPoints(gpuPoints);
      }
    }

    // Load water cooler fan points
    if (obj.contains("tableWaterCoolerFan")) {
      QJsonArray arr = obj["tableWaterCoolerFan"].toArray();
      QVector<FanCurveEditorWidget::Point> wcFanPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        wcFanPoints.append({temp, speed});
      }
      if (m_fanControlTab->wcFanEditor() && !wcFanPoints.isEmpty()) {
        m_fanControlTab->wcFanEditor()->setPoints(wcFanPoints);
      }
    }

    // Load pump threshold points
    if (obj.contains("tablePump")) {
      QJsonArray arr = obj["tablePump"].toArray();
      QVector<PumpCurveEditorWidget::Point> pumpPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        int speed = o["speed"].toInt();
        pumpPoints.append({temp, speed});
      }
      if (m_fanControlTab->pumpEditor() && !pumpPoints.isEmpty()) {
        m_fanControlTab->pumpEditor()->setPoints(pumpPoints);
      }
    }
  }

  // Update the current fan profile ID
  m_currentFanProfile = fanProfileId;

  // Synchronize profile tab fan profile combo with fan tab selection (match by ID userData)
  m_profileFanProfileCombo->blockSignals(true);
  int idx = -1;
  for ( int i = 0; i < m_profileFanProfileCombo->count(); ++i )
  {
    if ( m_profileFanProfileCombo->itemData( i ).toString() == fanProfileId )
    {
      idx = i;
      break;
    }
  }
  if (idx >= 0) {
    m_profileFanProfileCombo->setCurrentIndex(idx);
  }
  m_profileFanProfileCombo->blockSignals(false);

  // Set editors editable only for custom profiles (those not in built-ins)
  bool isEditable = !m_fanControlTab->builtinFanProfiles().contains( fanProfileId );
  m_fanControlTab->setEditorsEditable( isEditable );

  // Update button states
  updateButtonStates();
}

void MainWindow::onCpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_cpuFanPoints.clear();

  for (const auto& p : points) {
    m_cpuFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::reloadFanProfiles()
{
  // Delegate to FanControlTab which owns the combo and builtin list
  m_fanControlTab->reloadFanProfiles();

  // Mirror into profile page combo if present
  if ( m_profileFanProfileCombo )
  {
    m_profileFanProfileCombo->blockSignals(true);
    m_profileFanProfileCombo->clear();
    for ( int i = 0; i < m_fanControlTab->fanProfileCombo()->count(); ++i )
      m_profileFanProfileCombo->addItem( m_fanControlTab->fanProfileCombo()->itemText(i),
                                         m_fanControlTab->fanProfileCombo()->itemData(i) );
    m_profileFanProfileCombo->blockSignals(false);
  }

  // Trigger change handler to update editors/buttons
  if ( m_fanControlTab->fanProfileCombo() && m_fanControlTab->fanProfileCombo()->count() > 0 )
    onFanProfileChanged( m_fanControlTab->fanProfileCombo()->currentData().toString() );
  else
    updateButtonStates();
}



void MainWindow::onUccdConnectionChanged( bool connected )
{
  qDebug() << "[MainWindow] uccd connection changed:" << connected;

  if ( !connected )
  {
    // Display disconnected status
    if ( m_connectionLabel )
      m_connectionLabel->setText(
        QStringLiteral( "<span style='color: red;'>●</span> Disconnected" ) );
  }
  else
  {
    // When connected, display the active profile name
    updateConnectionStatusLabel();
  }

  if ( !connected )
    return;

  // Re-query capabilities that are only set at startup
  if ( auto waterCooler = m_UccdClient->getWaterCoolerSupported() )
    m_waterCoolerSupported = *waterCooler;
  if ( auto ctgp = m_UccdClient->getCTGPAdjustmentSupported() )
    m_cTGPAdjustmentSupported = *ctgp;
  if ( auto gpuDefault = m_UccdClient->getNVIDIAPowerCTRLDefaultPowerLimit() )
    m_gpuDefaultPowerLimit = *gpuDefault;

  // Repopulate driver-reported option lists
  reloadFanProfiles();
  populateGovernorCombo();
  populateEppCombo();

  // Refresh button/widget enabled states
  updateButtonStates();
}

void MainWindow::onGpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_gpuFanPoints.clear();
  for (const auto& p : points) {
    m_gpuFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::onWaterCoolerFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_waterCoolerFanPoints.clear();
  for (const auto& p : points) {
    m_waterCoolerFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::onPumpPointsChanged(const QVector<PumpCurveEditorWidget::Point>& /*points*/)
{
  // Pump points changed – mark the fan profile as modified so it can be saved
  if ( m_fanControlTab )
  {
    m_fanControlTab->saveButton()->setEnabled( true );
    m_fanControlTab->applyButton()->setEnabled( true );
  }
}

void MainWindow::onCopyFanProfileClicked()
{
  QString currentProfileId = m_fanControlTab->fanProfileCombo()->currentData().toString();

  if ( currentProfileId.isEmpty() )
    return;

  // Get the current profile name and strip " [Built-in]" suffix if present
  QString currentName = m_fanControlTab->fanProfileCombo()->currentText();

  if ( currentName.endsWith(" [Built-in]") )
    currentName = currentName.left( currentName.size() - 11 ); // 11 is length of " [Built-in]"

  // Get the current profile data
  QString json = m_profileManager->getFanProfile( currentProfileId );
  if ( json.isEmpty() ) {
    QMessageBox::warning(this, "Error", "Failed to get fan profile data.");
    return;
  }

  // Generate new name: "New {name}" with optional incrementing number
  QString baseName = QString("New %1").arg(currentName);
  QString profileName = baseName;
  int counter = 1;
  while ( m_fanControlTab->fanProfileCombo()->findText( profileName ) != -1 ) {
    profileName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  }

  // Save it under the new name with a new ID
  QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  if ( m_profileManager->setFanProfile( newId, profileName, json ) ) {
    m_fanControlTab->fanProfileCombo()->addItem( profileName, newId );
    if ( m_profileFanProfileCombo && m_profileFanProfileCombo->findText( profileName ) == -1 )
      m_profileFanProfileCombo->addItem( profileName, newId );
    // Select by index so currentIndexChanged fires and editability is updated
    int newIdx = m_fanControlTab->fanProfileCombo()->findData( newId );
    if ( newIdx >= 0 )
      m_fanControlTab->fanProfileCombo()->setCurrentIndex( newIdx );
    statusBar()->showMessage( QString("Copied fan profile to '%1'").arg(profileName) );
  }
  else {
    QMessageBox::warning(this, "Error", "Failed to copy profile to new custom profile.");
  }
}

void MainWindow::onApplyFanProfilesClicked()
{
  if ( not m_fanControlTab->cpuEditor() and not m_fanControlTab->gpuEditor() )
  {
    QMessageBox::warning( this, "No Editors", "No fan curve editors available to apply fan profiles." );
    return;
  }

  if ( !m_UccdClient || !m_UccdClient->isConnected() )
  {
    QMessageBox::warning( this, "Not connected", "Not connected to system service; cannot apply fan profiles." );
    return;
  }

  const auto &cpuPoints = m_fanControlTab->cpuEditor()->points();
  const auto &gpuPoints = m_fanControlTab->gpuEditor()->points();
  QJsonObject root;
  QJsonArray cpuArr;
  QJsonArray gpuArr;

  for ( const auto &p : cpuPoints )
  {
    QJsonObject o;
    o["temp"] = p.temp;
    o["speed"] = p.duty;
    cpuArr.append( o );
  }

  for ( const auto &p : gpuPoints )
  {
    QJsonObject o;
    o["temp"] = p.temp;
    o["speed"] = p.duty;
    gpuArr.append( o );
  }

  // Water cooler fan points
  QJsonArray wcFanArr;
  if ( m_fanControlTab->wcFanEditor() )
  {
    const auto &wcFanPoints = m_fanControlTab->wcFanEditor()->points();
    for ( const auto &p : wcFanPoints )
    {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      wcFanArr.append( o );
    }
  }

  // Pump threshold points
  QJsonArray pumpArr;
  if ( m_fanControlTab->pumpEditor() )
  {
    const auto &pumpPoints = m_fanControlTab->pumpEditor()->points();
    for ( const auto &p : pumpPoints )
    {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.level;
      pumpArr.append( o );
    }
  }

  root[ "cpu" ] = cpuArr;
  root[ "gpu" ] = gpuArr;
  root[ "waterCoolerFan" ] = wcFanArr;
  root[ "pump" ] = pumpArr;
  if ( !m_currentFanProfile.isEmpty() )
    root[ "fanProfileId" ] = m_currentFanProfile;

  QJsonDocument doc( root );
  QString json = QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );

  if ( m_UccdClient->applyFanProfiles( json.toStdString() ) )
  {
    statusBar()->showMessage( "Temporary fan profiles applied" );

    // Keep an internal copy so UI actions like revert have the current values
    m_cpuFanPoints.clear();
    for ( const auto &p : m_fanControlTab->cpuEditor()->points() )
      m_cpuFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );

    m_gpuFanPoints.clear();
    for ( const auto &p : m_fanControlTab->gpuEditor()->points() )
      m_gpuFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );

    m_waterCoolerFanPoints.clear();
    if ( m_fanControlTab->wcFanEditor() )
      for ( const auto &p : m_fanControlTab->wcFanEditor()->points() )
        m_waterCoolerFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );

    // Re-send the current water cooler enable state to the daemon so that
    // the profile apply (which may set enableWaterCooler from profile data)
    // doesn't override the user's current checkbox state.
    if ( m_fanControlTab )
      m_fanControlTab->sendWaterCoolerEnable( m_fanControlTab->isWaterCoolerEnabled() );
  }
  else
  {
    QMessageBox::warning( this, "Apply Failed", "Failed to apply fan profiles. Check service logs or connection." );
  }
}

void MainWindow::onRevertFanProfilesClicked()
{
  loadFanPoints();
}

void MainWindow::loadFanPoints()
{
  // Load fan profile JSON for the currently selected fan profile (if custom)
  if ( m_currentFanProfile.isEmpty() ) return;

  QString json = m_profileManager->getFanProfile( m_currentFanProfile );
  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (doc.isObject()) {
    QJsonObject obj = doc.object();
    if (obj.contains("tableCPU")) {
      QJsonArray arr = obj["tableCPU"].toArray();
      m_cpuFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> cpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_cpuFanPoints.append({temp, speed});
        cpuPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->cpuEditor()) {
        m_fanControlTab->cpuEditor()->setPoints(cpuPoints);
      }
    }
    if (obj.contains("tableGPU")) {
      QJsonArray arr = obj["tableGPU"].toArray();
      m_gpuFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> gpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_gpuFanPoints.append({temp, speed});
        gpuPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->gpuEditor()) {
        m_fanControlTab->gpuEditor()->setPoints(gpuPoints);
      }
    }
    if (obj.contains("tableWaterCoolerFan")) {
      QJsonArray arr = obj["tableWaterCoolerFan"].toArray();
      m_waterCoolerFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> wcFanPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_waterCoolerFanPoints.append({temp, speed});
        wcFanPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->wcFanEditor()) {
        m_fanControlTab->wcFanEditor()->setPoints(wcFanPoints);
      }
    }
    if (obj.contains("tablePump")) {
      QJsonArray arr = obj["tablePump"].toArray();
      QVector<PumpCurveEditorWidget::Point> pumpPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        int speed = o["speed"].toInt();
        pumpPoints.append({temp, speed});
      }
      if (m_fanControlTab->pumpEditor() && !pumpPoints.isEmpty()) {
        m_fanControlTab->pumpEditor()->setPoints(pumpPoints);
      }
    }
  }
}


void MainWindow::saveFanPoints()
{
  QJsonObject obj;

  // Get CPU points from the editor
  QJsonArray cpuArr;
  if ( m_fanControlTab->cpuEditor() )
  {
    const auto& cpuPoints = m_fanControlTab->cpuEditor()->points();
    for (const auto &p : cpuPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      cpuArr.append(o);
    }
  }
  obj["tableCPU"] = cpuArr;

  // Get GPU points from the editor
  QJsonArray gpuArr;
  if (m_fanControlTab->gpuEditor()) {
    const auto& gpuPoints = m_fanControlTab->gpuEditor()->points();
    for (const auto &p : gpuPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      gpuArr.append(o);
    }
  }
  obj["tableGPU"] = gpuArr;

  // Get water cooler fan points from the editor
  QJsonArray wcFanArr;
  if (m_fanControlTab->wcFanEditor()) {
    const auto& wcFanPoints = m_fanControlTab->wcFanEditor()->points();
    for (const auto &p : wcFanPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      wcFanArr.append(o);
    }
  }

  obj["tableWaterCoolerFan"] = wcFanArr;

  // Get pump points from the editor
  QJsonArray pumpArr;
  if (m_fanControlTab->pumpEditor()) {
    const auto& pumpPoints = m_fanControlTab->pumpEditor()->points();
    for (const auto &p : pumpPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.level;
      pumpArr.append(o);
    }
  }
  obj["tablePump"] = pumpArr;

  QJsonDocument doc(obj);
  QString json = doc.toJson(QJsonDocument::Compact);

  const QString currentId = m_fanControlTab->fanProfileCombo() ? m_fanControlTab->fanProfileCombo()->currentData().toString() : QString();
  const QString currentName = m_fanControlTab->fanProfileCombo() ? m_fanControlTab->fanProfileCombo()->currentText() : QString();
  if ( currentId.isEmpty() ) {
    QMessageBox::warning(this, "Save Failed", "No fan profile selected to save to.");
    return;
  }

  if ( m_fanControlTab->builtinFanProfiles().contains( currentId ) ) {
    QMessageBox::warning(this, "Save Failed", "Cannot overwrite built-in fan profile. Copy it to a custom profile first.");
    return;
  }

  // Save into selected custom profile (by ID)
  m_profileManager->setFanProfile( currentId, currentName, json );
}

// ---------------------------------------------------------------------------
// GPU OC profile handlers
// ---------------------------------------------------------------------------

void MainWindow::onGpuProfileChanged( const QString &gpuProfileId )
{
  if ( gpuProfileId.isEmpty() || !m_gpuProfileTab )
    return;

  QString json = m_profileManager->getGpuProfile( gpuProfileId );
  m_gpuProfileTab->loadProfile( json );
  m_gpuProfileTab->updateButtonStates( m_UccdClient && m_UccdClient->isConnected() );
}

void MainWindow::onApplyGpuProfileClicked()
{
  if ( !m_gpuProfileTab || !m_UccdClient || !m_UccdClient->isConnected() )
  {
    QMessageBox::warning( this, "Not connected", "Not connected to system service; cannot apply GPU OC profile." );
    return;
  }

  QString profileJson = m_gpuProfileTab->buildProfileJSON();

  QString selectedGpuProfileId;
  QString selectedGpuProfileName;
  if ( m_gpuProfileTab->gpuProfileCombo() )
  {
    selectedGpuProfileId = m_gpuProfileTab->gpuProfileCombo()->currentData().toString();
    selectedGpuProfileName = m_gpuProfileTab->gpuProfileCombo()->currentText();
  }

  qDebug() << "[GPU-CTGP] Apply clicked"
           << "selectedGpuProfileId=" << selectedGpuProfileId
           << "selectedGpuProfileName=" << selectedGpuProfileName
           << "payload=" << profileJson;

  if ( m_UccdClient->applyNvidiaGpuOCProfile( profileJson.toStdString() ) )
  {
    statusBar()->showMessage( "GPU OC settings applied (temporary; use Save to persist)" );

    const auto offsetAfterApply = m_UccdClient->getNVIDIAPowerOffset();
    const auto ocStateAfterApply = m_UccdClient->getNvidiaOCState( 0 );
    qDebug() << "[GPU-CTGP] Apply success"
             << "offsetAfterApply=" << ( offsetAfterApply ? QString::number( *offsetAfterApply ) : QString( "<none>" ) )
             << "ocStateAfterApply=" << ( ocStateAfterApply ? QString::fromStdString( *ocStateAfterApply ) : QString( "<none>" ) );

    m_gpuProfileTab->refreshOCState();
  }
  else
  {
    QMessageBox::warning( this, "Apply Failed", "Failed to apply GPU OC profile. Check service logs." );
  }
}

void MainWindow::onSaveGpuProfileClicked()
{
  if ( !m_gpuProfileTab || !m_gpuProfileTab->gpuProfileCombo() )
    return;

  QString currentId = m_gpuProfileTab->gpuProfileCombo()->currentData().toString();
  QString currentName = m_gpuProfileTab->gpuProfileCombo()->currentText();

  if ( currentId.isEmpty() )
  {
    QMessageBox::warning( this, "Save Failed", "No GPU OC profile selected." );
    return;
  }

  for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
  {
    if ( v.isObject() && v.toObject().value( "id" ).toString() == currentId )
    {
      QMessageBox::information( this, "Built-in Profile",
                                "Built-in GPU OC profiles cannot be modified. Copy it to a custom profile first." );
      return;
    }
  }

  QString json = m_gpuProfileTab->buildProfileJSON();
  m_profileManager->setGpuProfile( currentId, currentName, json );
  statusBar()->showMessage( QString( "GPU OC profile '%1' saved" ).arg( currentName ) );
}

void MainWindow::onCopyGpuProfileClicked()
{
  if ( !m_gpuProfileTab || !m_gpuProfileTab->gpuProfileCombo() )
    return;

  // Get current profile data (either from combo selection or from current widget state)
  QString json = m_gpuProfileTab->buildProfileJSON();

  // Determine a base name for the copy
  QString currentName = m_gpuProfileTab->gpuProfileCombo()->currentText();
  if ( currentName.isEmpty() )
    currentName = "GPU Profile";

  QString baseName = QString( "New %1" ).arg( currentName );
  QString profileName = baseName;
  int counter = 1;
  while ( m_gpuProfileTab->gpuProfileCombo()->findText( profileName ) != -1 )
  {
    profileName = QString( "%1 %2" ).arg( baseName ).arg( counter );
    counter++;
  }

  QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  if ( m_profileManager->setGpuProfile( newId, profileName, json ) )
  {
    m_gpuProfileTab->reloadGpuProfiles();
    // Select the newly created profile
    for ( int i = 0; i < m_gpuProfileTab->gpuProfileCombo()->count(); ++i )
    {
      if ( m_gpuProfileTab->gpuProfileCombo()->itemData( i ).toString() == newId )
      {
        m_gpuProfileTab->gpuProfileCombo()->setCurrentIndex( i );
        break;
      }
    }
    statusBar()->showMessage( QString( "Copied GPU OC profile to '%1'" ).arg( profileName ) );
  }
  else
  {
    QMessageBox::warning( this, "Error", "Failed to copy GPU OC profile." );
  }
}

void MainWindow::onRemoveGpuProfileClicked()
{
  if ( !m_gpuProfileTab || !m_gpuProfileTab->gpuProfileCombo() )
    return;

  QString currentId = m_gpuProfileTab->gpuProfileCombo()->currentData().toString();
  QString currentName = m_gpuProfileTab->gpuProfileCombo()->currentText();

  if ( currentId.isEmpty() )
    return;

  for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
  {
    if ( v.isObject() && v.toObject().value( "id" ).toString() == currentId )
    {
      QMessageBox::information( this, "Built-in Profile",
                                "Built-in GPU OC profiles cannot be removed." );
      return;
    }
  }

  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove GPU OC Profile",
    QString( "Are you sure you want to delete the GPU OC profile '%1'?" ).arg( currentName ),
    QMessageBox::Yes | QMessageBox::No );

  if ( reply == QMessageBox::Yes )
  {
    m_profileManager->deleteGpuProfile( currentId );
    statusBar()->showMessage( QString( "GPU OC profile '%1' removed" ).arg( currentName ) );
  }
}

} // namespace ucc
