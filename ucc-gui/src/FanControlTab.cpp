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

#include "FanControlTab.hpp"

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QColorDialog>
#include <QMainWindow>
#include <QStatusBar>
#include <QDBusReply>
#include <QDebug>
#include "CommonTypes.hpp"
#include "AsyncRead.hpp"
#include <QSignalBlocker>

namespace ucc
{

FanControlTab::FanControlTab( UccdClient *client,
                              ProfileManager *profileManager,
                              bool waterCoolerSupported,
                              QWidget *parent )
  : QWidget( parent )
  , m_uccdClient( client )
  , m_profileManager( profileManager )
  , m_waterCoolerSupported( waterCoolerSupported )
{
  // DBus interface for water cooler hardware controls (only if water cooler supported)
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerDbus = new QDBusInterface(
      QStringLiteral( "com.uniwill.uccd" ),
      QStringLiteral( "/com/uniwill/uccd" ),
      QStringLiteral( "com.uniwill.uccd" ),
      QDBusConnection::systemBus(), this );

    m_waterCoolerPollTimer = new QTimer( this );
    connect(m_waterCoolerPollTimer, &QTimer::timeout, this, &FanControlTab::pollWaterCoolerStatus);
    // Don't start timer immediately - wait for water cooler to be enabled
  }

  setupUI();
  connectSignals();
}

void FanControlTab::pollWaterCoolerStatus()
{
  if (!isVisible() || window()->isMinimized() || !isWaterCoolerEnabled() || m_waterCoolerReadPending) return;
  m_waterCoolerReadPending = true;
  readUccdBatch(this, {"GetWaterCoolerConnected", "GetWaterCoolerPumpLevel", "GetWaterCoolerFanSpeed"},
    [this](const QVariantMap &values) {
      m_waterCoolerReadPending = false;
      if (!isVisible() || window()->isMinimized() || !isWaterCoolerEnabled() ||
          !values.contains("GetWaterCoolerConnected")) return;
      if (!values.value("GetWaterCoolerConnected").toBool()) { onDisconnected(); return; }
      onConnected();
      // Reflect the running cooler; never send commands while hydrating the UI.
      if (values.contains("GetWaterCoolerPumpLevel") && !m_pumpVoltageCombo->hasFocus()) {
        const int index = m_pumpVoltageCombo->findData(values.value("GetWaterCoolerPumpLevel"));
        if (index >= 0) {
          const QSignalBlocker blocked(m_pumpVoltageCombo);
          m_pumpVoltageCombo->setCurrentIndex(index);
        }
      }
      const int speed = values.value("GetWaterCoolerFanSpeed", -1).toInt();
      if (speed >= 0 && speed <= 100 && !m_fanSpeedSlider->isSliderDown() && !m_fanSpeedSlider->hasFocus()) {
        const QSignalBlocker blocked(m_fanSpeedSlider);
        m_fanSpeedSlider->setValue(speed);
      }
    });
}

// UI construction

void FanControlTab::setupUI()
{
  QVBoxLayout *mainLayout = new QVBoxLayout( this );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // Top bar: fan profile selection
  QHBoxLayout *selectLayout = new QHBoxLayout();
  m_fanProfileCombo = new QComboBox();
  m_fanProfileCombo->setEditable( true );
  m_fanProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  for ( const auto &v : m_profileManager->builtinFanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_fanProfileCombo->addItem( name, id );
    m_builtinFanProfiles.append( id );
  }

  for ( const auto &v : m_profileManager->customFanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    if ( !m_builtinFanProfiles.contains( id ) )
      m_fanProfileCombo->addItem( name, id );
  }

  m_applyFanProfilesButton = new QPushButton( "Apply" );
  m_applyFanProfilesButton->setMaximumWidth( 80 );
  m_applyFanProfilesButton->setEnabled( false );

  m_saveFanProfilesButton = new QPushButton( "Save" );
  m_saveFanProfilesButton->setMaximumWidth( 80 );
  m_saveFanProfilesButton->setEnabled( false );

  m_copyFanProfileButton = new QPushButton( "Copy" );
  m_copyFanProfileButton->setMaximumWidth( 60 );
  m_copyFanProfileButton->setEnabled( false );

  m_removeFanProfileButton = new QPushButton( "Remove" );
  m_removeFanProfileButton->setMaximumWidth( 70 );

  selectLayout->addWidget( m_fanProfileCombo, 1 );
  selectLayout->addWidget( m_applyFanProfilesButton );
  selectLayout->addWidget( m_saveFanProfilesButton );
  selectLayout->addWidget( m_copyFanProfileButton );
  selectLayout->addWidget( m_removeFanProfileButton );
  mainLayout->addLayout( selectLayout );

  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  mainLayout->addWidget( separator );

  // Sub-tabs
  QTabWidget *subTabs = new QTabWidget();
  subTabs->setStyleSheet(
    "QTabWidget::pane { border: none; }"
    "QTabBar::tab { padding: 6px 18px; }" );

  // Sub-tab 1: System (CPU / GPU)
  {
    QWidget *systemWidget = new QWidget();
    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable( true );
    QVBoxLayout *layout = new QVBoxLayout( systemWidget );
    layout->setContentsMargins( 10, 10, 10, 10 );
    layout->setSpacing( 8 );

    QVBoxLayout *cpuLayout = new QVBoxLayout();
    cpuLayout->setSpacing( 0 );
    m_cpuFanCurveEditor = new FanCurveEditorWidget();
    m_cpuFanCurveEditor->setTitle( tr( "CPU Fan Curve" ) );
    cpuLayout->addWidget( m_cpuFanCurveEditor );
    layout->addLayout( cpuLayout );

    QVBoxLayout *gpuLayout = new QVBoxLayout();
    gpuLayout->setSpacing( 0 );
    m_gpuFanCurveEditor = new FanCurveEditorWidget();
    m_gpuFanCurveEditor->setTitle( tr( "GPU Fan Curve" ) );
    gpuLayout->addWidget( m_gpuFanCurveEditor );
    layout->addLayout( gpuLayout );

    scroll->setWidget( systemWidget );
    subTabs->addTab( scroll, "System (CPU / GPU)" );
  }

  // Sub-tab 2: Water Cooler (only if water cooler supported)
  if ( m_waterCoolerSupported )
  {
    QWidget *wcWidget = new QWidget();
    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable( true );
    QVBoxLayout *layout = new QVBoxLayout( wcWidget );
    layout->setContentsMargins( 5, 5, 5, 5 );
    layout->setSpacing( 8 );

    // Water cooler hardware controls
    QHBoxLayout *wcHw = new QHBoxLayout();
    wcHw->setContentsMargins( 0, 0, 0, 0 );
    wcHw->setSpacing( 4 );

    m_waterCoolerEnableCheckBox = new QPushButton( "Enable" );
    m_waterCoolerEnableCheckBox->setCheckable( true );
    m_waterCoolerEnableCheckBox->setChecked( ucc::WATER_COOLER_INITIAL_STATE );
    m_waterCoolerEnableCheckBox->setToolTip( tr( "When enabled the daemon will scan for water cooler devices" ) );
    m_waterCoolerEnableCheckBox->setFixedHeight( 24 );
    m_waterCoolerEnableCheckBox->setStyleSheet([
      ]() {
        // Use green/red for enabled/disabled states instead of theme highlight
        const QString enabledColor = QStringLiteral("#4caf50");
        const QString disabledColor = QStringLiteral("#d32f2f");
        return QStringLiteral("QPushButton { font-size: 11px; padding: 2px 12px; border: 1px solid palette(mid); border-radius: 4px; background-color: %1; }"
                              "QPushButton:checked { background-color: %2; font-weight: bold; }")
               .arg(disabledColor, enabledColor);
      }() );
    wcHw->addWidget( m_waterCoolerEnableCheckBox );

    QLabel *pumpVoltageLabel = new QLabel( "Pump Voltage:" );
    m_pumpVoltageCombo = new QComboBox();
    m_pumpVoltageCombo->addItem( "Off", QVariant::fromValue( PumpVoltage::Off ) );
    m_pumpVoltageCombo->addItem( "7V",  QVariant::fromValue( PumpVoltage::V7  ) );
    m_pumpVoltageCombo->addItem( "8V",  QVariant::fromValue( PumpVoltage::V8  ) );
    m_pumpVoltageCombo->addItem( "11V", QVariant::fromValue( PumpVoltage::V11 ) );

    // 12V option is intentionally omitted for safety of the pump.
    // m_pumpVoltageCombo->addItem( "12V", QVariant::fromValue( PumpVoltage::V12 ) );

    m_pumpVoltageCombo->setCurrentIndex( 0 );
    m_pumpVoltageCombo->setEnabled( false );
    m_pumpVoltageCombo->setMaximumWidth( 70 );
    wcHw->addWidget( pumpVoltageLabel );
    wcHw->addWidget( m_pumpVoltageCombo );

    QLabel *fanSpeedLabel = new QLabel( "Fan Speed:" );
    m_fanSpeedSlider = new QSlider( Qt::Horizontal );
    m_fanSpeedSlider->setMinimum( 0 );
    m_fanSpeedSlider->setMaximum( 100 );
    m_fanSpeedSlider->setValue( 0 );
    m_fanSpeedSlider->setEnabled( false );

    wcHw->addWidget( fanSpeedLabel );
    wcHw->addWidget( m_fanSpeedSlider );

    m_ledOnOffCheckBox = new QCheckBox( "LED" );
    m_ledOnOffCheckBox->setChecked( true );
    m_ledOnOffCheckBox->setEnabled( true );
    m_ledOnOffCheckBox->setLayoutDirection( Qt::RightToLeft );
    wcHw->addWidget( m_ledOnOffCheckBox );

    m_colorPickerButton = new QPushButton( "Choose Color" );
    m_colorPickerButton->setEnabled( false );
    wcHw->addWidget( m_colorPickerButton );

    QLabel *ledModeLabel = new QLabel( "Mode:" );
    m_ledModeCombo = new QComboBox();
    m_ledModeCombo->addItem( "Static",        QVariant::fromValue( RGBState::Static ) );
    m_ledModeCombo->addItem( "Breathe",       QVariant::fromValue( RGBState::Breathe ) );
    m_ledModeCombo->addItem( "Colorful",      QVariant::fromValue( RGBState::Colorful ) );
    m_ledModeCombo->addItem( "Breathe Color", QVariant::fromValue( RGBState::BreatheColor ) );
    m_ledModeCombo->addItem( "Temperature",   QVariant::fromValue( RGBState::Temperature ) );
    m_ledModeCombo->setCurrentIndex( 0 );
    m_ledModeCombo->setEnabled( true );
    wcHw->addWidget( ledModeLabel );
    wcHw->addWidget( m_ledModeCombo );

    // Set initial color button state
    updateColorButtonState();

    QWidget *waterCoolerWidget = new QWidget();
    waterCoolerWidget->setLayout( wcHw );
    layout->addWidget( waterCoolerWidget );

    // Water cooler fan curve editor
    QVBoxLayout *wcFanLayout = new QVBoxLayout();
    wcFanLayout->setSpacing( 0 );
    m_waterCoolerFanCurveEditor = new FanCurveEditorWidget();
    m_waterCoolerFanCurveEditor->setTitle( tr( "Water Cooler Fan Curve" ) );
    wcFanLayout->addWidget( m_waterCoolerFanCurveEditor );
    layout->addLayout( wcFanLayout );

    // Pump voltage curve editor
    QVBoxLayout *pumpLayout = new QVBoxLayout();
    pumpLayout->setSpacing( 0 );
    m_pumpCurveEditor = new PumpCurveEditorWidget();
    m_pumpCurveEditor->setTitle( tr( "Pump Voltage Curve" ) );
    pumpLayout->addWidget( m_pumpCurveEditor );
    layout->addLayout( pumpLayout );

    scroll->setWidget( wcWidget );
    subTabs->addTab( scroll, "Water Cooler" );
  }

  mainLayout->addWidget( subTabs );
}

// Signal wiring

void FanControlTab::connectSignals()
{
  // Fan profile combo - use index-based signal to avoid rename keystrokes triggering profile load
  connect( m_fanProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this]( int index ) {
    if ( index >= 0 )
      emit fanProfileChanged( m_fanProfileCombo->itemData( index ).toString() );
  } );

  // Fan profile combo rename handling
  connect( m_fanProfileCombo->lineEdit(), &QLineEdit::editingFinished,
           this, &FanControlTab::onFanProfileComboRenamed );

  // Curve editors -> signals
  connect( m_cpuFanCurveEditor, &FanCurveEditorWidget::pointsChanged,
           this, &FanControlTab::cpuPointsChanged );
  connect( m_gpuFanCurveEditor, &FanCurveEditorWidget::pointsChanged,
           this, &FanControlTab::gpuPointsChanged );

  if ( m_waterCoolerSupported )
  {
    connect( m_waterCoolerFanCurveEditor, &FanCurveEditorWidget::pointsChanged,
             this, &FanControlTab::wcFanPointsChanged );
    connect( m_pumpCurveEditor, &PumpCurveEditorWidget::pointsChanged,
             this, &FanControlTab::pumpPointsChanged );
  }

  // Action buttons -> signals
  connect( m_applyFanProfilesButton, &QPushButton::clicked,
           this, &FanControlTab::applyRequested );
  connect( m_saveFanProfilesButton, &QPushButton::clicked,
           this, &FanControlTab::saveRequested );
  connect( m_copyFanProfileButton, &QPushButton::clicked,
           this, &FanControlTab::copyRequested );
  connect( m_removeFanProfileButton, &QPushButton::clicked,
           this, &FanControlTab::removeRequested );

  // Water cooler hardware controls
  if ( m_waterCoolerSupported )
  {
    connect( m_waterCoolerEnableCheckBox, &QPushButton::toggled,
             this, &FanControlTab::onWaterCoolerEnableToggled );
    connect( m_pumpVoltageCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &FanControlTab::onPumpVoltageChanged );
    connect( m_fanSpeedSlider, &QSlider::valueChanged,
             this, &FanControlTab::onFanSpeedChanged );
    connect( m_ledOnOffCheckBox, &QCheckBox::toggled,
             this, &FanControlTab::onLEDOnOffChanged );
    connect( m_colorPickerButton, &QPushButton::clicked,
             this, &FanControlTab::onColorPickerClicked );
    connect( m_ledModeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &FanControlTab::onLEDModeChanged );
  }

  // Initialize water cooler polling based on initial enabled state
  updateWaterCoolerPolling();
}

// Public helpers

void FanControlTab::reloadFanProfiles()
{
  QString prevId = m_fanProfileCombo ? m_fanProfileCombo->currentData().toString() : QString();
  if ( m_fanProfileCombo ) m_fanProfileCombo->clear();
  m_builtinFanProfiles.clear();

  for ( const auto &v : m_profileManager->builtinFanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_fanProfileCombo->addItem( name, id );
    m_builtinFanProfiles.append( id );
  }

  for ( const auto &v : m_profileManager->customFanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    if ( !m_builtinFanProfiles.contains( id ) )
      m_fanProfileCombo->addItem( name, id );
  }

  // Restore selection by ID

  if ( !prevId.isEmpty() )
  {
    for ( int i = 0; i < m_fanProfileCombo->count(); ++i )
    {
      if ( m_fanProfileCombo->itemData( i ).toString() == prevId )
      { m_fanProfileCombo->setCurrentIndex( i ); return; }
    }
  }
  if ( m_fanProfileCombo->count() > 0 )
    m_fanProfileCombo->setCurrentIndex( 0 );
}

void FanControlTab::updateButtonStates( bool uccdConnected )
{
  const QString id = m_fanProfileCombo ? m_fanProfileCombo->currentData().toString() : QString();
  const bool isCustom = ( !id.isEmpty() && !m_builtinFanProfiles.contains( id ) );

  if ( m_applyFanProfilesButton )   m_applyFanProfilesButton->setEnabled( uccdConnected );
  if ( m_saveFanProfilesButton )    m_saveFanProfilesButton->setEnabled( isCustom );
  if ( m_copyFanProfileButton )     m_copyFanProfileButton->setEnabled( !id.isEmpty() );
  if ( m_revertFanProfilesButton )  m_revertFanProfilesButton->setEnabled( isCustom && uccdConnected );

  // Only allow renaming custom fan profiles
  if ( m_fanProfileCombo && m_fanProfileCombo->lineEdit() )
    m_fanProfileCombo->lineEdit()->setReadOnly( !isCustom );
}

void FanControlTab::setEditorsEditable( bool editable )
{
  if ( m_cpuFanCurveEditor )             m_cpuFanCurveEditor->setEditable( editable );
  if ( m_gpuFanCurveEditor )             m_gpuFanCurveEditor->setEditable( editable );
  if ( m_waterCoolerFanCurveEditor )     m_waterCoolerFanCurveEditor->setEditable( editable );
  if ( m_pumpCurveEditor )               m_pumpCurveEditor->setEditable( editable );
}

void FanControlTab::onFanProfileComboRenamed()
{
  if ( !m_fanProfileCombo || !m_fanProfileCombo->lineEdit() ) return;

  int idx = m_fanProfileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString fanProfileId = m_fanProfileCombo->itemData( idx ).toString();
  QString oldName = m_fanProfileCombo->itemText( idx );
  QString newName = m_fanProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  // Cannot rename built-in profiles
  if ( m_builtinFanProfiles.contains( fanProfileId ) ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameFanProfile( fanProfileId, newName ) ) {
    m_fanProfileCombo->setItemText( idx, newName );
    emit fanProfileRenamed( oldName, newName );

    // Find the parent MainWindow to update the status bar
    if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    {
      if ( auto *sb = mw->statusBar() )
        sb->showMessage( QString("Fan profile renamed from '%1' to '%2'").arg( oldName, newName ) );
    }
  } else {
    m_fanProfileCombo->setEditText( oldName );
  }
}

void FanControlTab::setWaterCoolerEnabled( bool enabled )
{
  if ( m_waterCoolerEnableCheckBox )
  {
    m_waterCoolerEnableCheckBox->blockSignals( true );
    m_waterCoolerEnableCheckBox->setChecked( enabled );
    m_waterCoolerEnableCheckBox->blockSignals( false );
  }

  // Update polling state when programmatically setting enabled state
  updateWaterCoolerPolling();

  // NOTE: Do NOT call EnableWaterCooler on D-Bus here.
  // This method is called during profile loading to update the UI checkbox.
  // Calling EnableWaterCooler would restart BLE scanning (destroying any
  // active connection) or disconnect the water cooler, causing the
  // connected -> disconnected -> reconnecting oscillation on GUI startup.
  // The D-Bus call only happens via onWaterCoolerEnableToggled() when the
  // user explicitly toggles the checkbox.
}

void FanControlTab::sendWaterCoolerEnable( bool enabled )
{
  if ( m_waterCoolerDbus )
    m_waterCoolerDbus->call( QStringLiteral( "EnableWaterCooler" ), enabled );
}

bool FanControlTab::isWaterCoolerEnabled() const
{
  return m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : true;
}

// Water cooler hardware slots

void FanControlTab::onWaterCoolerEnableToggled( bool enabled )
{
  if ( m_waterCoolerDbus )
    m_waterCoolerDbus->call( QStringLiteral( "EnableWaterCooler" ), enabled );

  // Update polling state based on new enable state
  updateWaterCoolerPolling();

  // Update manual control state when water cooler enable state changes
  updateManualControlState();

  emit waterCoolerEnableChanged( enabled );
}

void FanControlTab::onConnected()
{
  // Don't connect if water cooler is disabled
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  if ( !wcEnabled ) {
    onDisconnected(); // Force disconnect state
    return;
  }

  if ( m_isWcConnected ) return;
  m_isWcConnected = true;

  // Update manual control state now that water cooler is connected
  updateManualControlState();

  // LED control mode and LED checkbox are always enabled
  // Color button is only enabled if mode is Static
  updateColorButtonState();
  m_isWcConnected = true;
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    mw->statusBar()->showMessage( tr( "Connection to water cooler successful" ) );
}

void FanControlTab::onDisconnected()
{
  if ( !m_isWcConnected ) return;
  m_isWcConnected = false;

  // Update manual control state now that water cooler is disconnected
  updateManualControlState();

  // LED control mode and LED checkbox remain always enabled
  if ( m_colorPickerButton ) m_colorPickerButton->setEnabled( false );
  m_isWcConnected = false;
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    mw->statusBar()->clearMessage();
}

void FanControlTab::onPumpVoltageChanged( int index )
{
  if ( !m_waterCoolerDbus ) return;
  if ( index == static_cast< int >( PumpVoltage::Off ) )
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
  else
  {
    PumpVoltage voltage = static_cast< PumpVoltage >( m_pumpVoltageCombo->itemData( index ).toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerPumpVoltage" ), static_cast< int >( voltage ) );
  }
}

void FanControlTab::onFanSpeedChanged( int speed )
{
  if ( !m_waterCoolerDbus ) return;
  m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerFanSpeed" ), speed );
}

void FanControlTab::onLEDOnOffChanged( bool enabled )
{
  if ( !m_waterCoolerDbus ) return;
  updateColorButtonState();
  if ( enabled )
  {
    RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                             m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
  else
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerLED" ) );
}

void FanControlTab::onLEDModeChanged( int /*index*/ )
{
  updateColorButtonState();
  if ( !m_waterCoolerDbus ) return;
  if ( m_ledOnOffCheckBox->isChecked() )
  {
    RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                             m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
}

void FanControlTab::onColorPickerClicked()
{
  QColor currentColor( m_currentRed, m_currentGreen, m_currentBlue );
  QColor color = QColorDialog::getColor( currentColor, this, "Choose LED Color" );
  if ( !color.isValid() ) return;
  m_currentRed = color.red();
  m_currentGreen = color.green();
  m_currentBlue = color.blue();
  RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
  if ( m_ledOnOffCheckBox->isChecked() && m_waterCoolerDbus )
  {
    if ( QDBusReply< bool > conn = m_waterCoolerDbus->call( QStringLiteral( "GetWaterCoolerConnected" ) );
         conn.isValid() && conn.value() )
      m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                               m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
  m_colorPickerButton->setStyleSheet(
    QString( "background-color: rgb(%1, %2, %3);" ).arg( m_currentRed ).arg( m_currentGreen ).arg( m_currentBlue ) );
}

void FanControlTab::setWaterCoolerAutoControl( bool autoControl )
{
  m_autoControl = autoControl;

  // Update manual control state considering all factors
  updateManualControlState();
}

void FanControlTab::updateManualControlState()
{
  // Manual controls are enabled when:
  // 1. Water cooler is enabled (checkbox checked)
  // 2. Water cooler is connected (hardware connection)
  // 3. Auto control is disabled (manual control allowed)
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  bool enableManualControls = wcEnabled && m_isWcConnected && !m_autoControl;

  if ( m_pumpVoltageCombo )
    m_pumpVoltageCombo->setEnabled( enableManualControls );
  if ( m_fanSpeedSlider )
    m_fanSpeedSlider->setEnabled( enableManualControls );
}

void FanControlTab::updateWaterCoolerPolling()
{
  if ( !m_waterCoolerPollTimer ) return;

  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;

  if ( wcEnabled ) {
    if ( !m_waterCoolerPollTimer->isActive() )
      m_waterCoolerPollTimer->start( 2000 );
  } else {
    m_waterCoolerPollTimer->stop();
    onDisconnected();
  }
}

void FanControlTab::updateColorButtonState()
{
  if ( !m_colorPickerButton || !m_ledModeCombo ) return;

  // Enable color button only when mode is Static and LED is enabled
  RGBState currentMode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
  bool isStaticMode = ( currentMode == RGBState::Static );
  bool isLEDEnabled = m_ledOnOffCheckBox ? m_ledOnOffCheckBox->isChecked() : false;

  m_colorPickerButton->setEnabled( isStaticMode && isLEDEnabled );
}

} // namespace ucc
