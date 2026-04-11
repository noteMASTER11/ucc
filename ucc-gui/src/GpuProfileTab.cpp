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

#include "GpuProfileTab.hpp"

#include <array>
#include <set>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QMainWindow>
#include <QStatusBar>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QDebug>
#include <QMessageBox>
#include <algorithm>
#include <cmath>
#include <optional>

namespace ucc
{

GpuProfileTab::GpuProfileTab( UccdClient *client,
                                ProfileManager *profileManager,
                                QWidget *parent )
  : QWidget( parent )
  , m_uccdClient( client )
  , m_profileManager( profileManager )
{
  // Check if NVIDIA OC is available
  if ( auto avail = m_uccdClient->getNvidiaOCAvailable() )
    m_ocAvailable = *avail;

  setupUI();
  connectSignals();
  reloadGpuProfiles();
  updateButtonStates( m_uccdClient && m_uccdClient->isConnected() );

  // Initial hardware state read
  if ( m_ocAvailable )
  {
    refreshOCState();

    m_liveMetricsTimer = new QTimer( this );
    m_liveMetricsTimer->setInterval( 1000 );
    connect( m_liveMetricsTimer, &QTimer::timeout, this, &GpuProfileTab::refreshLiveMetrics );
    m_liveMetricsTimer->start();
  }
}

// ── UI construction ─────────────────────────────────────────────────

void GpuProfileTab::setupUI()
{
  QVBoxLayout *mainLayout = new QVBoxLayout( this );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // ── Top bar: GPU profile selection ──
  QHBoxLayout *selectLayout = new QHBoxLayout();
  m_gpuProfileCombo = new QComboBox();
  m_gpuProfileCombo->setEditable( true );
  m_gpuProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  m_applyButton = new QPushButton( "Apply" );
  m_applyButton->setMaximumWidth( 80 );
  m_applyButton->setEnabled( false );
  m_applyButton->setToolTip( "Applies current GPU OC settings temporarily. Use Save to persist." );

  m_saveButton = new QPushButton( "Save" );
  m_saveButton->setMaximumWidth( 80 );
  m_saveButton->setEnabled( false );

  m_copyButton = new QPushButton( "Copy" );
  m_copyButton->setMaximumWidth( 60 );
  m_copyButton->setEnabled( false );

  m_removeButton = new QPushButton( "Remove" );
  m_removeButton->setMaximumWidth( 70 );

  m_refreshButton = new QPushButton( "Refresh" );
  m_refreshButton->setMaximumWidth( 100 );
  m_refreshButton->setVisible( m_ocAvailable );

  m_resetButton = new QPushButton( "Reset" );
  m_resetButton->setMaximumWidth( 80 );
  m_resetButton->setVisible( m_ocAvailable );

  m_aggressiveP0Toggle = new QCheckBox( "P0 dGPU Force" );
  m_aggressiveP0Toggle->setVisible( true );
  m_aggressiveP0Toggle->setEnabled( false );
  m_aggressiveP0Toggle->setToolTip(
    "Forces the Mechrevo/TUXEDO NVIDIA power-control path to max cTGP, Dynamic Boost, "
    "overboost, and maximum ODM TDP when this GPU OC profile is applied." );

  selectLayout->addWidget( m_gpuProfileCombo, 1 );
  selectLayout->addWidget( m_aggressiveP0Toggle );
  selectLayout->addWidget( m_applyButton );
  selectLayout->addWidget( m_saveButton );
  selectLayout->addWidget( m_copyButton );
  selectLayout->addWidget( m_removeButton );
  selectLayout->addWidget( m_refreshButton );
  selectLayout->addWidget( m_resetButton );
  mainLayout->addLayout( selectLayout );

  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  mainLayout->addWidget( separator );

  // ── Scroll area ──
  QScrollArea *scrollArea = new QScrollArea();
  scrollArea->setWidgetResizable( true );
  QWidget *scrollWidget = new QWidget();
  QVBoxLayout *contentLayout = new QVBoxLayout( scrollWidget );
  contentLayout->setContentsMargins( 15, 10, 15, 10 );
  contentLayout->setSpacing( 12 );

  // Not-available label (shown when NVML isn't loaded)
  m_notAvailableLabel = new QLabel(
    "<b>NVIDIA GPU OC is not available.</b><br>"
    "Ensure that an NVIDIA GPU is present and the NVIDIA driver (with NVML) is installed." );
  m_notAvailableLabel->setWordWrap( true );
  m_notAvailableLabel->setVisible( !m_ocAvailable );
  contentLayout->addWidget( m_notAvailableLabel );

  // === GPU INFO SECTION (unboxed) ===
  QWidget *infoSection = new QWidget();
  infoSection->setVisible( m_ocAvailable );
  QVBoxLayout *infoSectionLayout = new QVBoxLayout( infoSection );
  infoSectionLayout->setContentsMargins( 0, 0, 0, 0 );
  infoSectionLayout->setSpacing( 6 );

  QGridLayout *infoLayout = new QGridLayout();
  infoLayout->setHorizontalSpacing( 18 );
  infoLayout->setVerticalSpacing( 4 );
  QLabel *tempLabel = new QLabel( "Temperature" );
  QLabel *powerLabel = new QLabel( "Power draw" );
  QLabel *pstateLabel = new QLabel( "Current P-State" );

  m_gpuNameLabel = new QLabel( "—" );
  m_tempLabel = new QLabel( "—" );
  m_powerDrawLabel = new QLabel( "—" );
  m_currentPstateLabel = new QLabel( "—" );
  QFont valueFont = m_gpuNameLabel->font();
  valueFont.setBold( true );
  m_gpuNameLabel->setFont( valueFont );
  m_tempLabel->setFont( valueFont );
  m_powerDrawLabel->setFont( valueFont );
  m_currentPstateLabel->setFont( valueFont );

  infoLayout->addWidget( m_gpuNameLabel, 0, 0 );
  infoLayout->addWidget( tempLabel, 0, 2 );
  infoLayout->addWidget( m_tempLabel, 0, 3 );
  infoLayout->addWidget( powerLabel, 0, 4 );
  infoLayout->addWidget( m_powerDrawLabel, 0, 5 );
  infoLayout->addWidget( pstateLabel, 0, 6 );
  infoLayout->addWidget( m_currentPstateLabel, 0, 7 );
  infoLayout->setColumnStretch( 1, 1 );
  infoSectionLayout->addLayout( infoLayout );
  contentLayout->addWidget( infoSection );

  // === CLOCK OFFSETS (grouped by P-state) ===
  m_pstatesLayout = new QVBoxLayout();
  m_pstatesLayout->setSpacing( 8 );

  // === cTGP (same semantics as Profiles page) ===
  QGroupBox *powerGroup = new QGroupBox( "Configurable graphics power (TGP)" );
  powerGroup->setVisible( m_ocAvailable );
  QHBoxLayout *powerLayout = new QHBoxLayout( powerGroup );
  powerLayout->addWidget( new QLabel( "TGP:" ) );
  m_powerLimitSlider = new QSlider( Qt::Horizontal );
  m_powerLimitSlider->setMinimum( 40 );
  m_powerLimitSlider->setMaximum( 175 );
  m_powerLimitValue = new QLabel( "0 W" );
  m_powerLimitValue->setMinimumWidth( 60 );
  powerLayout->addWidget( m_powerLimitSlider, 1 );
  powerLayout->addWidget( m_powerLimitValue );
  contentLayout->addWidget( powerGroup );

  // === GPU LOCKED CLOCKS ===
  m_gpuLockedGroup = new QGroupBox( "GPU Core Locked Clocks" );
  m_gpuLockedGroup->setVisible( m_ocAvailable );
  m_gpuLockedGroup->setCheckable( true );
  m_gpuLockedGroup->setChecked( false );
  QVBoxLayout *gpuLockedLayout = new QVBoxLayout( m_gpuLockedGroup );

  QHBoxLayout *gpuLockedRow = new QHBoxLayout();
  gpuLockedRow->addWidget( new QLabel( "Min:" ) );
  m_gpuLockedMinSlider = new QSlider( Qt::Horizontal );
  m_gpuLockedMinSpin = new QSpinBox();
  m_gpuLockedMinSpin->setSuffix( " MHz" );
  gpuLockedRow->addWidget( m_gpuLockedMinSlider, 1 );
  gpuLockedRow->addWidget( m_gpuLockedMinSpin );
  gpuLockedRow->addWidget( new QLabel( "Max:" ) );
  m_gpuLockedMaxSlider = new QSlider( Qt::Horizontal );
  m_gpuLockedMaxSpin = new QSpinBox();
  m_gpuLockedMaxSpin->setSuffix( " MHz" );
  gpuLockedRow->addWidget( m_gpuLockedMaxSlider, 1 );
  gpuLockedRow->addWidget( m_gpuLockedMaxSpin );
  gpuLockedLayout->addLayout( gpuLockedRow );

  contentLayout->addWidget( m_gpuLockedGroup );

  // === VRAM LOCKED CLOCKS ===
  m_vramLockedGroup = new QGroupBox( "VRAM Locked Clocks" );
  m_vramLockedGroup->setVisible( m_ocAvailable );
  m_vramLockedGroup->setCheckable( true );
  m_vramLockedGroup->setChecked( false );
  QVBoxLayout *vramLockedLayout = new QVBoxLayout( m_vramLockedGroup );

  QHBoxLayout *vramLockedRow = new QHBoxLayout();
  vramLockedRow->addWidget( new QLabel( "Min:" ) );
  m_vramLockedMinSlider = new QSlider( Qt::Horizontal );
  m_vramLockedMinSpin = new QSpinBox();
  m_vramLockedMinSpin->setSuffix( " MHz" );
  vramLockedRow->addWidget( m_vramLockedMinSlider, 1 );
  vramLockedRow->addWidget( m_vramLockedMinSpin );
  vramLockedRow->addWidget( new QLabel( "Max:" ) );
  m_vramLockedMaxSlider = new QSlider( Qt::Horizontal );
  m_vramLockedMaxSpin = new QSpinBox();
  m_vramLockedMaxSpin->setSuffix( " MHz" );
  vramLockedRow->addWidget( m_vramLockedMaxSlider, 1 );
  vramLockedRow->addWidget( m_vramLockedMaxSpin );
  vramLockedLayout->addLayout( vramLockedRow );

  contentLayout->addWidget( m_vramLockedGroup );

  // === CLOCK OFFSETS (grouped by P-state) ===
  // Individual P-state groups are added directly; no outer wrapper needed.
  contentLayout->addLayout( m_pstatesLayout );

  contentLayout->addStretch();
  scrollArea->setWidget( scrollWidget );
  mainLayout->addWidget( scrollArea );
}

// ── Signal wiring ───────────────────────────────────────────────────

void GpuProfileTab::connectSignals()
{
  // GPU profile combo - index-based signal
  connect( m_gpuProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this]( int index ) {
    if ( index >= 0 )
      emit gpuProfileChanged( m_gpuProfileCombo->itemData( index ).toString() );
  } );

  // Rename handling
  if ( m_gpuProfileCombo->lineEdit() )
  {
    connect( m_gpuProfileCombo->lineEdit(), &QLineEdit::editingFinished,
             this, &GpuProfileTab::onGpuProfileComboRenamed );
  }

  // Action buttons
  connect( m_applyButton, &QPushButton::clicked, this, [this]() {
    if ( !ensureOverclockWarningAcknowledged() )
      return;
    emit applyRequested();
  } );
  connect( m_saveButton, &QPushButton::clicked, this, [this]() {
    if ( !ensureOverclockWarningAcknowledged() )
      return;
    emit saveRequested();
  } );
  connect( m_copyButton, &QPushButton::clicked, this, &GpuProfileTab::copyRequested );
  connect( m_removeButton, &QPushButton::clicked, this, &GpuProfileTab::removeRequested );
  connect( m_refreshButton, &QPushButton::clicked, this, &GpuProfileTab::onRefreshClicked );
  connect( m_resetButton, &QPushButton::clicked, this, &GpuProfileTab::onResetClicked );

  connect( m_gpuLockedGroup, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );
  connect( m_vramLockedGroup, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );

  // GPU locked clocks: bidirectional slider <-> spinbox sync
  connect( m_gpuLockedMinSlider, &QSlider::valueChanged, m_gpuLockedMinSpin, &QSpinBox::setValue );
  connect( m_gpuLockedMinSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMinSlider, &QSlider::setValue );
  connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, m_gpuLockedMaxSpin, &QSpinBox::setValue );
  connect( m_gpuLockedMaxSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMaxSlider, &QSlider::setValue );
  connect( m_gpuLockedMinSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v > m_gpuLockedMaxSlider->value() ) m_gpuLockedMaxSlider->setValue( v );
    emit changed();
  } );
  connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v < m_gpuLockedMinSlider->value() ) m_gpuLockedMinSlider->setValue( v );
    emit changed();
  } );

  // VRAM locked clocks: bidirectional slider <-> spinbox sync
  connect( m_vramLockedMinSlider, &QSlider::valueChanged, m_vramLockedMinSpin, &QSpinBox::setValue );
  connect( m_vramLockedMinSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_vramLockedMinSlider, &QSlider::setValue );
  connect( m_vramLockedMaxSlider, &QSlider::valueChanged, m_vramLockedMaxSpin, &QSpinBox::setValue );
  connect( m_vramLockedMaxSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_vramLockedMaxSlider, &QSlider::setValue );
  connect( m_vramLockedMinSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v > m_vramLockedMaxSlider->value() ) m_vramLockedMaxSlider->setValue( v );
    emit changed();
  } );
  connect( m_vramLockedMaxSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v < m_vramLockedMinSlider->value() ) m_vramLockedMinSlider->setValue( v );
    emit changed();
  } );

  // Power limit slider <-> label
  if ( m_powerLimitSlider )
  {
    connect( m_powerLimitSlider, &QSlider::valueChanged, this, [this]( int v ) {
      m_powerLimitValue->setText( QString::number( v ) + " W" );
      emit changed();
    } );
  }

  if ( m_aggressiveP0Toggle )
    connect( m_aggressiveP0Toggle, &QCheckBox::toggled, this, [this]( bool ) { emit changed(); } );
}

// ── Public helpers ──────────────────────────────────────────────────

void GpuProfileTab::reloadGpuProfiles()
{
  QString prevId = m_gpuProfileCombo ? m_gpuProfileCombo->currentData().toString() : QString();
  if ( m_gpuProfileCombo )
    m_gpuProfileCombo->clear();

  for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    if ( !id.isEmpty() )
      m_gpuProfileCombo->addItem( name, id );
  }

  for ( const auto &v : m_profileManager->customGpuProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_gpuProfileCombo->addItem( name, id );
  }

  if ( !prevId.isEmpty() )
  {
    for ( int i = 0; i < m_gpuProfileCombo->count(); ++i )
    {
      if ( m_gpuProfileCombo->itemData( i ).toString() == prevId )
      {
        m_gpuProfileCombo->setCurrentIndex( i );
        return;
      }
    }
  }
  if ( m_gpuProfileCombo->count() > 0 )
    m_gpuProfileCombo->setCurrentIndex( 0 );
}

void GpuProfileTab::updateButtonStates( bool uccdConnected )
{
  const QString id = m_gpuProfileCombo ? m_gpuProfileCombo->currentData().toString() : QString();
  const bool hasSelection = !id.isEmpty();
  bool isBuiltin = false;
  for ( const auto &v : m_profileManager->builtinGpuProfilesData() )
  {
    if ( v.isObject() && v.toObject().value( "id" ).toString() == id )
    {
      isBuiltin = true;
      break;
    }
  }

  if ( m_applyButton )   m_applyButton->setEnabled( uccdConnected && m_ocAvailable );
  if ( m_saveButton )    m_saveButton->setEnabled( hasSelection && !isBuiltin );
  if ( m_copyButton )    m_copyButton->setEnabled( hasSelection || m_ocAvailable );
  if ( m_removeButton )  m_removeButton->setEnabled( hasSelection && !isBuiltin );
  if ( m_resetButton )   m_resetButton->setEnabled( uccdConnected && m_ocAvailable );

  // Built-in GPU profiles are immutable on disk, but their values can still be
  // adjusted and applied temporarily to the hardware.
  const bool controlsEnabled = hasSelection && uccdConnected && m_ocAvailable;
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setEnabled( controlsEnabled && m_offsetsSupported );
  }
  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setEnabled( controlsEnabled && m_lockedSupported );
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setEnabled( controlsEnabled && m_lockedSupported );
  if ( m_powerLimitSlider )
    m_powerLimitSlider->setEnabled( controlsEnabled && m_powerMaxW > m_powerMinW );
  if ( m_aggressiveP0Toggle )
    m_aggressiveP0Toggle->setEnabled( uccdConnected && m_ocAvailable && m_uccdClient
                                      && m_uccdClient->getNVIDIAPowerCTRLAvailable().value_or( false ) );

  // Allow renaming custom profiles
  if ( m_gpuProfileCombo && m_gpuProfileCombo->lineEdit() )
    m_gpuProfileCombo->lineEdit()->setReadOnly( !hasSelection || isBuiltin );
}

void GpuProfileTab::refreshOCState()
{
  if ( !m_ocAvailable || !m_uccdClient )
    return;

  auto stateOpt = m_uccdClient->getNvidiaOCState( 0 );
  if ( !stateOpt )
    return;

  qDebug() << "[GPU-CTGP] refreshOCState raw state:" << QString::fromStdString( *stateOpt );

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *stateOpt ) );
  if ( !doc.isObject() )
    return;

  QJsonObject state = doc.object();

  // Update info labels
  m_gpuNameLabel->setText( state["gpuName"].toString( "Unknown" ) );
  refreshLiveMetrics();

  // cTGP range (Profiles-page compatible semantics)
  m_powerMinW = state["powerMinW"].toDouble();
  m_powerMaxW = state["powerMaxW"].toDouble();
  m_powerDefaultW = state["powerDefaultW"].toDouble();

  int defaultPower = static_cast< int >( std::round( m_powerDefaultW ) );
  if ( auto v = m_uccdClient->getNVIDIAPowerCTRLDefaultPowerLimit() )
    defaultPower = *v;

  int maxPower = static_cast< int >( std::round( m_powerMaxW ) );
  if ( auto v = m_uccdClient->getNVIDIAPowerCTRLMaxPowerLimit() )
    maxPower = *v;

  int currentOffset = 0;
  bool haveOffset = false;
  if ( auto v = m_uccdClient->getNVIDIAPowerOffset() )
  {
    currentOffset = *v;
    haveOffset = true;
  }

  int currentPowerFromState = static_cast< int >( std::round( state["powerLimitW"].toDouble( 0.0 ) ) );
  m_nvmlPowerLimitSupported = currentPowerFromState > 0;

  qDebug() << "[GPU-CTGP] refresh inputs"
           << "powerMinW=" << m_powerMinW
           << "powerDefaultW(state)=" << m_powerDefaultW
           << "powerMaxW(state)=" << m_powerMaxW
           << "defaultPower(iface)=" << defaultPower
           << "maxPower(iface)=" << maxPower
           << "haveOffset=" << haveOffset
           << "offset(iface)=" << currentOffset
           << "powerLimitW(state)=" << currentPowerFromState;

  if ( defaultPower > 0 && maxPower >= defaultPower )
  {
    int currentPower = defaultPower;

    if ( haveOffset )
      currentPower = defaultPower + currentOffset;
    else if ( currentPowerFromState > 0 )
      currentPower = currentPowerFromState;

    currentPower = std::clamp( currentPower, defaultPower, maxPower );

    qDebug() << "[GPU-CTGP] refresh resolved"
             << "sliderMin=" << defaultPower
             << "sliderMax=" << maxPower
             << "sliderValue=" << currentPower;

    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setMinimum( defaultPower );
    m_powerLimitSlider->setMaximum( maxPower );
    m_powerLimitSlider->setValue( currentPower );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( currentPower ) + " W" );

    m_powerDefaultW = defaultPower;
    m_powerMaxW = maxPower;
  }

  // GPU clock range for locked clocks
  if ( state.contains( "gpuClockRange" ) )
  {
    QJsonObject r = state["gpuClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    m_gpuLockedMinSlider->setMinimum( lo );
    m_gpuLockedMinSlider->setMaximum( hi );
    m_gpuLockedMaxSlider->setMinimum( lo );
    m_gpuLockedMaxSlider->setMaximum( hi );
    m_gpuLockedMinSpin->setMinimum( lo );
    m_gpuLockedMinSpin->setMaximum( hi );
    m_gpuLockedMaxSpin->setMinimum( lo );
    m_gpuLockedMaxSpin->setMaximum( hi );

    // Set defaults
    m_gpuLockedMinSlider->setValue( lo );
    m_gpuLockedMaxSlider->setValue( hi );
  }

  if ( state.contains( "vramClockRange" ) )
  {
    QJsonObject r = state["vramClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    m_vramLockedMinSlider->setMinimum( lo );
    m_vramLockedMinSlider->setMaximum( hi );
    m_vramLockedMaxSlider->setMinimum( lo );
    m_vramLockedMaxSlider->setMaximum( hi );
    m_vramLockedMinSpin->setMinimum( lo );
    m_vramLockedMinSpin->setMaximum( hi );
    m_vramLockedMaxSpin->setMinimum( lo );
    m_vramLockedMaxSpin->setMaximum( hi );

    m_vramLockedMinSlider->setValue( lo );
    m_vramLockedMaxSlider->setValue( hi );
  }

  // Load existing locked clocks if applied
  if ( state.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = state["gpuLockedClocks"].toObject();
    m_gpuLockedMinSlider->setValue( lc["min"].toInt() );
    m_gpuLockedMaxSlider->setValue( lc["max"].toInt() );
  }

  if ( state.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = state["vramLockedClocks"].toObject();
    m_vramLockedMinSlider->setValue( lc["min"].toInt() );
    m_vramLockedMaxSlider->setValue( lc["max"].toInt() );
  }

  // Populate P-state offset rows – preserve checked state across refresh
  std::set< unsigned int > checkedPStates;
  for ( const auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox && grp.groupBox->isChecked() )
      checkedPStates.insert( grp.pstate );
  }

  clearPStateWidgets();
  if ( state.contains( "pstates" ) )
    populatePStates( state["pstates"].toArray() );

  // Restore checked state for P-states that were enabled before refresh
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setChecked( checkedPStates.count( grp.pstate ) > 0 );
  }

  // Feature support
  bool offsetsSupported = state["offsetsSupported"].toBool( false );
  bool lockedSupported = state["lockedClocksSupported"].toBool( false );
  m_offsetsSupported = offsetsSupported;
  m_lockedSupported = lockedSupported;

  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setEnabled( lockedSupported );
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setEnabled( lockedSupported );
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setEnabled( offsetsSupported );
  }

  updateButtonStates( m_uccdClient && m_uccdClient->isConnected() );
}

void GpuProfileTab::refreshLiveMetrics()
{
  if ( !m_ocAvailable || !m_uccdClient )
    return;

  std::optional< int > tempC = m_uccdClient->getGpuTemperature();
  std::optional< double > powerW = m_uccdClient->getGpuPower();
  std::optional< int > currentPstate = m_uccdClient->getDGpuCurrentPstate();

  if ( !tempC || !powerW )
  {
    auto stateOpt = m_uccdClient->getNvidiaOCState( 0 );
    if ( stateOpt )
    {
      const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *stateOpt ) );
      if ( doc.isObject() )
      {
        const QJsonObject state = doc.object();
        if ( !tempC && state.contains( "tempC" ) )
          tempC = state["tempC"].toInt();
        if ( !powerW && state.contains( "powerDrawW" ) )
          powerW = state["powerDrawW"].toDouble();
        if ( !currentPstate && state.contains( "currentPstate" ) )
          currentPstate = state["currentPstate"].toInt( -1 );
      }
    }
  }

  if ( m_tempLabel )
    m_tempLabel->setText( tempC ? ( QString::number( *tempC ) + " °C" ) : QStringLiteral( "—" ) );

  if ( m_powerDrawLabel )
    m_powerDrawLabel->setText( powerW ? ( QString::number( *powerW, 'f', 1 ) + " W" ) : QStringLiteral( "—" ) );

  if ( m_currentPstateLabel )
  {
    if ( currentPstate && *currentPstate >= 0 )
      m_currentPstateLabel->setText( QStringLiteral( "P" ) + QString::number( *currentPstate ) );
    else
      m_currentPstateLabel->setText( QStringLiteral( "—" ) );
  }
}

void GpuProfileTab::populatePStates( const QJsonArray &pstates )
{
  // Typical-use descriptions for NVIDIA performance states
  static const std::array< const char *, 16 > pstateDesc = {
    "Maximum 3D performance (gaming, compute)",   // P0
    "High 3D performance",                        // P1
    "Balanced 3D performance",                    // P2
    "Mixed 3D / media playback",                  // P3
    "HD video playback",                          // P4
    "Medium performance",                         // P5
    "Low performance",                            // P6
    "Low power",                                  // P7
    "Basic desktop / idle",                       // P8
    "Very low power",                             // P9
    "Lowest GPU clocks",                          // P10
    "Standby",                                    // P11
    "Minimal power",                              // P12
    "Reserved",                                   // P13
    "Reserved",                                   // P14
    "Maximum power saving",                       // P15
  };

  for ( const auto &v : pstates )
  {
    QJsonObject ps = v.toObject();
    unsigned int pstate = static_cast< unsigned int >( ps["pstate"].toInt() );

    QString title = QString( "P-State %1" ).arg( pstate );
    if ( pstate < pstateDesc.size() )
      title += QString( " — %1" ).arg( pstateDesc[pstate] );

    QGroupBox *group = new QGroupBox( title );
    group->setCheckable( true );
    group->setChecked( false );
    QHBoxLayout *groupLayout = new QHBoxLayout( group );
    groupLayout->setSpacing( 8 );
    groupLayout->setContentsMargins( 10, 6, 10, 6 );

    PStateGroup psg{};
    psg.pstate = pstate;
    psg.groupBox = group;

    // ── GPU Core column ──
    if ( ps.contains( "gpu" ) )
    {
      QJsonObject gpu = ps["gpu"].toObject();
      int minMHz = gpu["minMHz"].toInt();
      int maxMHz = gpu["maxMHz"].toInt();
      int minOff = gpu["minOffset"].toInt( -500 );
      int maxOff = gpu["maxOffset"].toInt( 500 );
      int curOff = gpu["currentOffset"].toInt( 0 );

      QVBoxLayout *col = new QVBoxLayout();
      col->setSpacing( 2 );
      QLabel *label = new QLabel( QString( "GPU Core (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

      QHBoxLayout *ctrlRow = new QHBoxLayout();
      QSlider *slider = new QSlider( Qt::Horizontal );
      slider->setMinimum( minOff ); slider->setMaximum( maxOff ); slider->setValue( curOff );
      slider->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      QSpinBox *spin = new QSpinBox();
      spin->setMinimum( minOff ); spin->setMaximum( maxOff ); spin->setValue( curOff );
      spin->setSuffix( " MHz" );
      spin->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      connect( slider, &QSlider::valueChanged, spin, &QSpinBox::setValue );
      connect( spin, QOverload< int >::of( &QSpinBox::valueChanged ), slider, &QSlider::setValue );
      connect( slider, &QSlider::valueChanged, this, [this]() { emit changed(); } );

      ctrlRow->addWidget( slider, 1 );
      ctrlRow->addWidget( spin );
      col->addWidget( label );
      col->addLayout( ctrlRow );
      groupLayout->addLayout( col, 1 );

      psg.gpuRow = { pstate, true, slider, spin };
    }

    // ── VRAM column ──
    if ( ps.contains( "vram" ) )
    {
      QJsonObject vram = ps["vram"].toObject();
      int minMHz = vram["minMHz"].toInt();
      int maxMHz = vram["maxMHz"].toInt();
      int minOff = vram["minOffset"].toInt( -500 );
      int maxOff = vram["maxOffset"].toInt( 500 );
      int curOff = vram["currentOffset"].toInt( 0 );

      QVBoxLayout *col = new QVBoxLayout();
      col->setSpacing( 2 );
      QLabel *label = new QLabel( QString( "VRAM (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

      QHBoxLayout *ctrlRow = new QHBoxLayout();
      QSlider *slider = new QSlider( Qt::Horizontal );
      slider->setMinimum( minOff ); slider->setMaximum( maxOff ); slider->setValue( curOff );
      slider->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      QSpinBox *spin = new QSpinBox();
      spin->setMinimum( minOff ); spin->setMaximum( maxOff ); spin->setValue( curOff );
      spin->setSuffix( " MHz" );
      spin->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      connect( slider, &QSlider::valueChanged, spin, &QSpinBox::setValue );
      connect( spin, QOverload< int >::of( &QSpinBox::valueChanged ), slider, &QSlider::setValue );
      connect( slider, &QSlider::valueChanged, this, [this]() { emit changed(); } );

      ctrlRow->addWidget( slider, 1 );
      ctrlRow->addWidget( spin );
      col->addWidget( label );
      col->addLayout( ctrlRow );
      groupLayout->addLayout( col, 1 );

      psg.vramRow = { pstate, false, slider, spin };
    }

    m_pstatesLayout->addWidget( group );
    connect( group, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );
    m_pstateGroups.push_back( psg );
  }
}

void GpuProfileTab::clearPStateWidgets()
{
  // Remove all P-state group boxes
  for ( auto &grp : m_pstateGroups )
    delete grp.groupBox;   // deletes children (sliders, spinboxes, labels) automatically
  m_pstateGroups.clear();
}

QString GpuProfileTab::buildProfileJSON() const
{
  QJsonObject root;

  // Clock offsets grouped by P-state
  {
    QJsonArray offsets;
    for ( const auto &grp : m_pstateGroups )
    {
      if ( grp.groupBox && !grp.groupBox->isChecked() )
        continue;

      QJsonObject o;
      o["pstate"] = static_cast< int >( grp.pstate );
      if ( grp.gpuRow.slider )
      {
        o["gpuOffsetMHz"] = grp.gpuRow.slider->value();
      }
      if ( grp.vramRow.slider )
      {
        o["vramOffsetMHz"] = grp.vramRow.slider->value();
      }
      offsets.append( o );
    }
    if ( !offsets.isEmpty() )
      root["offsets"] = offsets;
  }

  // GPU locked clocks (only include enabled when feature is supported)
  if ( m_gpuLockedGroup && m_gpuLockedGroup->isChecked() )
  {
    QJsonObject gpuLocked;
    gpuLocked["enabled"] = true;
    gpuLocked["min"] = m_gpuLockedMinSlider->value();
    gpuLocked["max"] = m_gpuLockedMaxSlider->value();
    root["gpuLockedClocks"] = gpuLocked;
  }

  // VRAM locked clocks (only include enabled when feature is supported)
  if ( m_vramLockedGroup && m_vramLockedGroup->isChecked() )
  {
    QJsonObject vramLocked;
    vramLocked["enabled"] = true;
    vramLocked["min"] = m_vramLockedMinSlider->value();
    vramLocked["max"] = m_vramLockedMaxSlider->value();
    root["vramLockedClocks"] = vramLocked;
  }

  // GPU power-limit payload: NVML powerLimitW plus the Tuxedo cTGP offset when available.
  if ( m_powerLimitSlider )
  {
    const int targetPowerW = m_powerLimitSlider->value();
    const int ctgpOffset = m_powerLimitSlider->value() - static_cast< int >( std::round( m_powerDefaultW ) );
    QJsonObject nvidiaPowerObj;
    nvidiaPowerObj["cTGPOffset"] = ctgpOffset;
    if ( m_nvmlPowerLimitSupported )
      root["powerLimitW"] = targetPowerW;
    root["nvidiaPowerCTRLProfile"] = nvidiaPowerObj;

    qDebug() << "[GPU-CTGP] buildProfileJSON"
             << "sliderValueW=" << targetPowerW
             << "baselineDefaultW=" << m_powerDefaultW
             << "ctgpOffset=" << ctgpOffset;
  }

  if ( m_aggressiveP0Toggle && m_aggressiveP0Toggle->isChecked() )
    root["aggressiveP0State"] = true;

  QJsonDocument doc( root );
  qDebug() << "[GPU-CTGP] buildProfileJSON payload:" << QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );
  return QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );
}

void GpuProfileTab::loadProfile( const QString &json )
{
  if ( json.isEmpty() || json == "{}" )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() )
    return;

  QJsonObject obj = doc.object();

  if ( m_aggressiveP0Toggle )
  {
    m_aggressiveP0Toggle->blockSignals( true );
    m_aggressiveP0Toggle->setChecked( obj.value( "aggressiveP0State" ).toBool( false ) );
    m_aggressiveP0Toggle->blockSignals( false );
  }

  // Load clock offsets
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setChecked( false );
  }

  if ( obj.contains( "offsets" ) )
  {
    QJsonArray offsets = obj["offsets"].toArray();
    for ( const auto &v : offsets )
    {
      QJsonObject o = v.toObject();
      int ps = o["pstate"].toInt();
      for ( auto &grp : m_pstateGroups )
      {
        if ( static_cast< int >( grp.pstate ) == ps )
        {
          if ( grp.groupBox )
            grp.groupBox->setChecked( true );

          if ( o.contains( "gpuOffsetMHz" ) && grp.gpuRow.slider )
          {
            int off = o["gpuOffsetMHz"].toInt();
            grp.gpuRow.slider->blockSignals( true );
            grp.gpuRow.slider->setValue( off );
            grp.gpuRow.slider->blockSignals( false );
            grp.gpuRow.spinBox->blockSignals( true );
            grp.gpuRow.spinBox->setValue( off );
            grp.gpuRow.spinBox->blockSignals( false );
          }
          if ( o.contains( "vramOffsetMHz" ) && grp.vramRow.slider )
          {
            int off = o["vramOffsetMHz"].toInt();
            grp.vramRow.slider->blockSignals( true );
            grp.vramRow.slider->setValue( off );
            grp.vramRow.slider->blockSignals( false );
            grp.vramRow.spinBox->blockSignals( true );
            grp.vramRow.spinBox->setValue( off );
            grp.vramRow.spinBox->blockSignals( false );
          }
          break;
        }
      }
    }
  }

  // Load GPU locked clocks
  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setChecked( obj.contains( "gpuLockedClocks" ) );

  if ( obj.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = obj["gpuLockedClocks"].toObject();

    m_gpuLockedMinSlider->blockSignals( true );
    m_gpuLockedMinSlider->setValue( lc["min"].toInt() );
    m_gpuLockedMinSlider->blockSignals( false );
    m_gpuLockedMaxSlider->blockSignals( true );
    m_gpuLockedMaxSlider->setValue( lc["max"].toInt() );
    m_gpuLockedMaxSlider->blockSignals( false );
  }

  // Load VRAM locked clocks
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setChecked( obj.contains( "vramLockedClocks" ) );

  if ( obj.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = obj["vramLockedClocks"].toObject();

    m_vramLockedMinSlider->blockSignals( true );
    m_vramLockedMinSlider->setValue( lc["min"].toInt() );
    m_vramLockedMinSlider->blockSignals( false );
    m_vramLockedMaxSlider->blockSignals( true );
    m_vramLockedMaxSlider->setValue( lc["max"].toInt() );
    m_vramLockedMaxSlider->blockSignals( false );
  }

  if ( m_powerLimitSlider )
  {
    int valueW = m_powerLimitSlider->value();
    int offset = valueW - static_cast< int >( std::round( m_powerDefaultW ) );

    if ( obj.contains( "powerLimitW" ) )
    {
      valueW = static_cast< int >( std::round( obj["powerLimitW"].toDouble( valueW ) ) );
      offset = valueW - static_cast< int >( std::round( m_powerDefaultW ) );
    }
    else if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() )
    {
      QJsonObject gpuObj = obj["nvidiaPowerCTRLProfile"].toObject();
      offset = gpuObj["cTGPOffset"].toInt( 0 );
      valueW = static_cast< int >( std::round( m_powerDefaultW ) ) + offset;
    }
    else
    {
      return;
    }

    qDebug() << "[GPU-CTGP] loadProfile"
             << "baselineDefaultW=" << m_powerDefaultW
             << "loadedOffset=" << offset
             << "targetW=" << valueW
             << "sliderMin=" << m_powerLimitSlider->minimum()
             << "sliderMax=" << m_powerLimitSlider->maximum();

    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setValue( std::clamp( valueW, m_powerLimitSlider->minimum(), m_powerLimitSlider->maximum() ) );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( m_powerLimitSlider->value() ) + " W" );

    qDebug() << "[GPU-CTGP] loadProfile applied sliderW=" << m_powerLimitSlider->value();
  }
}

// ── Private slots ───────────────────────────────────────────────────

void GpuProfileTab::onGpuProfileComboRenamed()
{
  if ( !m_gpuProfileCombo || !m_gpuProfileCombo->lineEdit() )
    return;

  int idx = m_gpuProfileCombo->currentIndex();
  if ( idx < 0 )
    return;

  QString gpuProfileId = m_gpuProfileCombo->itemData( idx ).toString();
  QString oldName = m_gpuProfileCombo->itemText( idx );
  QString newName = m_gpuProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName )
  {
    m_gpuProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameGpuProfile( gpuProfileId, newName ) )
  {
    m_gpuProfileCombo->setItemText( idx, newName );
    emit gpuProfileRenamed( oldName, newName );

    if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    {
      if ( auto *sb = mw->statusBar() )
        sb->showMessage( QString( "GPU profile renamed from '%1' to '%2'" ).arg( oldName, newName ) );
    }
  }
  else
  {
    m_gpuProfileCombo->setEditText( oldName );
  }
}

void GpuProfileTab::onRefreshClicked()
{
  refreshOCState();
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
  {
    if ( auto *sb = mw->statusBar() )
      sb->showMessage( "GPU OC state refreshed" );
  }
}

void GpuProfileTab::onResetClicked()
{
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Reset GPU OC",
    "Are you sure you want to reset all GPU overclocking settings to their defaults?\n\n"
    "This will clear all clock offsets, locked clocks, and restore the default power limit.",
    QMessageBox::Yes | QMessageBox::No );

  if ( reply == QMessageBox::Yes )
  {
    if ( m_uccdClient->getCTGPAdjustmentSupported().value_or( false ) )
      (void)m_uccdClient->setNVIDIAPowerOffset( 0 );

    bool ok = m_uccdClient->resetNvidiaGpuOCAll( 0 );

    // Fallback for platforms where monolithic reset reports failure
    // due to partially unsupported operations.
    if ( !ok )
    {
      bool anySucceeded = false;

      if ( m_uccdClient->resetNvidiaAllClockOffsets( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaGpuLockedClocks( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaVramLockedClocks( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaGpuPowerLimit( 0 ) )
        anySucceeded = true;

      ok = anySucceeded;
    }

    if ( ok )
    {
      if ( m_aggressiveP0Toggle )
        m_aggressiveP0Toggle->setChecked( false );

      refreshOCState();
      if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
      {
        if ( auto *sb = mw->statusBar() )
          sb->showMessage( "GPU OC settings reset to defaults" );
      }
    }
    else
    {
      QMessageBox::warning( this, "Error", "Failed to reset GPU OC settings." );
    }
  }
}

bool GpuProfileTab::ensureOverclockWarningAcknowledged()
{
  if ( !m_ocAvailable )
    return false;

  if ( isOverclockWarningAcknowledged() )
    return true;

  return showOverclockWarningDialog();
}

bool GpuProfileTab::isOverclockWarningAcknowledged() const
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  return settings.value( "gpu/ocWarningAcknowledged", false ).toBool();
}

void GpuProfileTab::setOverclockWarningAcknowledged( bool acknowledged )
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  settings.setValue( "gpu/ocWarningAcknowledged", acknowledged );
  settings.sync();
}

bool GpuProfileTab::showOverclockWarningDialog()
{
  QDialog dialog( this );
  dialog.setWindowTitle( "GPU Overclocking Warning" );
  dialog.setModal( true );

  QVBoxLayout *layout = new QVBoxLayout( &dialog );
  QLabel *msg = new QLabel(
    "Overclocking your GPU may cause instability, crashes, or hardware damage. "
    "Changes take effect immediately when applied." );
  msg->setWordWrap( true );
  layout->addWidget( msg );

  QCheckBox *ack = new QCheckBox( "I understand" );
  layout->addWidget( ack );

  QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );
  QPushButton *okButton = buttons->button( QDialogButtonBox::Ok );
  if ( okButton )
    okButton->setEnabled( false );

  connect( ack, &QCheckBox::toggled, this, [okButton]( bool checked ) {
    if ( okButton )
      okButton->setEnabled( checked );
  } );
  connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
  connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
  layout->addWidget( buttons );

  if ( dialog.exec() == QDialog::Accepted && ack->isChecked() )
  {
    setOverclockWarningAcknowledged( true );
    return true;
  }

  return false;
}

} // namespace ucc
