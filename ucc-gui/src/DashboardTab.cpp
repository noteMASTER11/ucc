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

#include "DashboardTab.hpp"
#include "SystemMonitor.hpp"
#include "ProfileManager.hpp"
#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QPalette>
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "CommonTypes.hpp"

namespace
{

QString formatFanSpeed( const QString &fanSpeed )
{
  QString display = "---";

  if ( fanSpeed.endsWith( " %" ) || fanSpeed.endsWith( "%" ) )
  {
    QString num = fanSpeed;
    if ( fanSpeed.endsWith( " %" ) )
      num = fanSpeed.left( fanSpeed.size() - 2 ).trimmed();
    else
      num = fanSpeed.left( fanSpeed.size() - 1 ).trimmed();

    bool ok = false;
    int pct = num.toInt( &ok );
    if ( ok && pct >= 0 )
      display = QString::number( pct );
  }
  else if ( fanSpeed.endsWith( " RPM" ) )
  {
    QString rpmStr = fanSpeed.left( fanSpeed.size() - 4 ).trimmed();
    bool ok = false;
    int rpm = rpmStr.toInt( &ok );

    if ( ok && rpm > 0 )
    {
      int pct = rpm / 60;
      display = QString::number( pct );
    }
  }

  return display;
}

}


namespace ucc
{

DashboardTab::DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager, bool waterCoolerSupported,
                            const QString &laptopModel, const QString &cpuModel,
                            const QString &dGpuModel, const QString &iGpuModel,
                            QWidget *parent )
  : QWidget( parent )
  , m_systemMonitor( systemMonitor )
  , m_profileManager( profileManager )
  , m_waterCoolerSupported( waterCoolerSupported )
  , m_laptopModel( laptopModel )
  , m_cpuModel( cpuModel )
  , m_dGpuModel( dGpuModel )
  , m_iGpuModel( iGpuModel )
{
  setupUI();
  connectSignals();

  // m_activeProfileLabel is created but hidden; it's only for internal use

  // Initialize water cooler status polling only if supported
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerDbus = new QDBusInterface(QStringLiteral("com.uniwill.uccd"), QStringLiteral("/com/uniwill/uccd"), QStringLiteral("com.uniwill.uccd"), QDBusConnection::systemBus(), this);
    m_waterCoolerPollTimer = new QTimer(this);
    connect(m_waterCoolerPollTimer, &QTimer::timeout, this, &DashboardTab::updateWaterCoolerStatus);
    m_waterCoolerPollTimer->start(1000);
    updateWaterCoolerStatus();
  }

  QTimer::singleShot( 0, this, [this]() { runHardwareCheck(); } );
}

void DashboardTab::setupUI()
{
  // Do not force a static background or text color here; allow the application's
  // palette/theme to control colors so the UI remains readable in all themes.
  QVBoxLayout *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 20, 20, 20, 20 );
  layout->setSpacing( 20 );

  // Resolve palette colors once and reuse as explicit hex values so the
  // applied styles have consistent contrast in both light and dark themes.
  QPalette pal = this->palette();
  const QString textHex = pal.color(QPalette::WindowText).name();
  const QString midHex = pal.color(QPalette::Mid).name();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QString linkHex = pal.color(QPalette::Link).name();
  const QColor windowBg = pal.color(QPalette::Window);
  // Choose a high-contrast inner text color based on window background
  const QString innerTextHex = (windowBg.value() < 128) ? QString("#ffffff") : QString("#000000");
  m_ringColorHex = QString("#d32f2f");  // Red for disconnected state and other alerts

  // Title — use laptop model from daemon if available
  // Use a grid so the title is centered over the full row width while
  // the checkbox floats to the right edge, both occupying the same cell.
  QGridLayout *titleLayout = new QGridLayout();
  const QString titleText = m_laptopModel.isEmpty() ? QStringLiteral( "System Monitor" ) : m_laptopModel;
  QLabel *titleLabel = new QLabel( titleText );
  titleLabel->setStyleSheet( QString("font-size: 22px; font-weight: bold;") );

  // Water Cooler Enable toggle button (synced with FanControlTab)
  m_waterCoolerEnableCheckBox = new QPushButton( "Water Cooler" );
  m_waterCoolerEnableCheckBox->setCheckable( true );
  m_waterCoolerEnableCheckBox->setChecked( ucc::WATER_COOLER_INITIAL_STATE );
  m_waterCoolerEnableCheckBox->setToolTip( tr( "When enabled the daemon will scan for water cooler devices" ) );
  m_waterCoolerEnableCheckBox->setFixedHeight( 24 );

  {
    QPalette pal_ = this->palette();
    const QString midHex_ = pal_.color(QPalette::Mid).name();
    const QString enabledColorHex = QStringLiteral("#4caf50"); // green
    const QString disabledColorHex = QStringLiteral("#d32f2f"); // red
    m_waterCoolerEnableCheckBox->setStyleSheet(
      QString("QPushButton { font-size: 11px; padding: 2px 16px; border: 1px solid %1; border-radius: 4px; background-color: %2; }"
              "QPushButton:checked { background-color: %3; font-weight: bold; padding: 2px 12px; }")
        .arg(midHex_, disabledColorHex, enabledColorHex) );
  }

  // Hide water cooler checkbox if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerEnableCheckBox->setVisible( false );
  }

  // Both widgets share the same cell — title centered, checkbox right-aligned
  titleLayout->addWidget( titleLabel,                0, 0, Qt::AlignCenter );
  titleLayout->addWidget( m_waterCoolerEnableCheckBox, 0, 0, Qt::AlignRight | Qt::AlignVCenter );
  layout->addLayout( titleLayout );

  QLabel *hardwareCheckHeader = new QLabel( "Hardware Check" );
  hardwareCheckHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  hardwareCheckHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( hardwareCheckHeader );

  QFrame *hardwareCheckPanel = new QFrame();
  hardwareCheckPanel->setObjectName( "hardwareCheckPanel" );
  hardwareCheckPanel->setStyleSheet(
    QString("QFrame#hardwareCheckPanel { border: 1px solid %1; border-radius: 6px; background: transparent; }")
      .arg(midHex) );
  QGridLayout *hardwareCheckLayout = new QGridLayout( hardwareCheckPanel );
  hardwareCheckLayout->setContentsMargins( 14, 10, 14, 10 );
  hardwareCheckLayout->setHorizontalSpacing( 20 );
  hardwareCheckLayout->setVerticalSpacing( 8 );

  auto makeHardwareCheckLabel = [&]( const QString &text ) -> QLabel * {
    QLabel *label = new QLabel( text + ": ..." );
    label->setStyleSheet( QString("font-size: 12px; font-weight: bold; color: %1;").arg(textHex) );
    return label;
  };

  m_hwCheckCpuLabel = makeHardwareCheckLabel( "CPU" );
  m_hwCheckGpuLabel = makeHardwareCheckLabel( "GPU" );
  m_hwCheckKeyboardBacklightLabel = makeHardwareCheckLabel( "Keyboard Backlight" );
  m_hwCheckKeyboardColorLabel = makeHardwareCheckLabel( "Keyboard Color" );
  m_hwCheckCpuTdpLabel = makeHardwareCheckLabel( "CPU TDP" );
  m_hwCheckGpuTdpLabel = makeHardwareCheckLabel( "GPU TDP" );

  hardwareCheckLayout->addWidget( m_hwCheckCpuLabel, 0, 0 );
  hardwareCheckLayout->addWidget( m_hwCheckGpuLabel, 0, 1 );
  hardwareCheckLayout->addWidget( m_hwCheckKeyboardBacklightLabel, 0, 2 );
  hardwareCheckLayout->addWidget( m_hwCheckKeyboardColorLabel, 1, 0 );
  hardwareCheckLayout->addWidget( m_hwCheckCpuTdpLabel, 1, 1 );
  hardwareCheckLayout->addWidget( m_hwCheckGpuTdpLabel, 1, 2 );
  layout->addWidget( hardwareCheckPanel );

  // Active Profile label (created but not shown; only used in status bar)
  m_activeProfileLabel = new QLabel( "Loading..." );
  m_activeProfileLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(textHex) );
  m_activeProfileLabel->setVisible( false );

  // Water Cooler Status label (created but not shown; only used in status bar)
  m_waterCoolerStatusLabel = new QLabel( "Disconnected" );
  m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(m_ringColorHex) );
  m_waterCoolerStatusLabel->setVisible( false );

  // makeCard: compact value+unit+caption cell — no border (the panel provides it)
  auto makeCard = [&]( const QString &caption, const QString &unit, QLabel *&valueLabel ) -> QWidget * {
    QWidget *card = new QWidget();
    QVBoxLayout *vl = new QVBoxLayout( card );
    vl->setContentsMargins( 16, 16, 16, 14 );
    vl->setSpacing( 6 );
    vl->setAlignment( Qt::AlignCenter );

    // Value + unit side-by-side, unit smaller and pushed to baseline
    QWidget *valueRow = new QWidget();
    QHBoxLayout *hl = new QHBoxLayout( valueRow );
    hl->setContentsMargins( 0, 0, 0, 0 );
    hl->setSpacing( 4 );
    hl->setAlignment( Qt::AlignCenter );

    valueLabel = new QLabel( "--" );
    valueLabel->setStyleSheet( QString("font-size: 26px; font-weight: bold; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    valueLabel->setAlignment( Qt::AlignCenter );

    QLabel *unitLabel = new QLabel( unit );
    unitLabel->setStyleSheet( QString("font-size: 11px; color: %1; background: transparent; border: none; padding-top: 9px;").arg(innerTextHex) );
    unitLabel->setAlignment( Qt::AlignVCenter );

    hl->addWidget( valueLabel );
    hl->addWidget( unitLabel );

    QLabel *captionLabel = new QLabel( caption );
    captionLabel->setStyleSheet( QString("font-size: 11px; color: %1; background: transparent; border: none;").arg(textHex) );
    captionLabel->setAlignment( Qt::AlignCenter );

    vl->addWidget( valueRow );
    vl->addWidget( captionLabel );
    return card;
  };

  // makeCardRow: horizontal row of cards with dashed vertical dividers, no outer border
  auto makeCardRow = [&]( std::initializer_list<QWidget *> cards ) -> QWidget * {
    QWidget *row = new QWidget();
    QHBoxLayout *hl = new QHBoxLayout( row );
    hl->setContentsMargins( 0, 0, 0, 0 );
    hl->setSpacing( 0 );
    bool first = true;
    for ( QWidget *card : cards )
    {
      if ( !first )
      {
        QFrame *sep = new QFrame();
        sep->setFrameShape( QFrame::VLine );
        sep->setFixedWidth( 1 );
        sep->setStyleSheet( QString("QFrame { border: none; border-left: 2px dashed %1; background: transparent; }").arg(m_ringColorHex) );
        hl->addWidget( sep );
      }
      hl->addWidget( card, 1 );
      first = false;
    }
    return row;
  };

  // makePanel: wraps N cards in one bordered box with dashed vertical dividers
  auto makePanel = [&]( std::initializer_list<QWidget *> cards ) -> QFrame * {
    QFrame *panel = new QFrame();
    panel->setStyleSheet( QString("QFrame#metricPanel { border: 2px solid %1; border-radius: 6px; background: transparent; }"
                                  "QWidget { background: transparent; }").arg(m_ringColorHex) );
    panel->setObjectName( "metricPanel" );
    QHBoxLayout *hl = new QHBoxLayout( panel );
    hl->setContentsMargins( 0, 0, 0, 0 );
    hl->setSpacing( 0 );
    bool first = true;
    for ( QWidget *card : cards )
    {
      if ( !first )
      {
        QFrame *sep = new QFrame();
        sep->setFrameShape( QFrame::VLine );
        sep->setFixedWidth( 1 );
        sep->setStyleSheet( QString("QFrame { border: none; border-left: 2px dashed %1; background: transparent; }").arg(m_ringColorHex) );
        hl->addWidget( sep );
      }
      hl->addWidget( card, 1 );
      first = false;
    }
    return panel;
  };

  // CPU section
  const QString cpuHeaderText = m_cpuModel.isEmpty() ? QStringLiteral( "Main Processor Monitor" ) : m_cpuModel;
  QLabel *cpuHeader = new QLabel( cpuHeaderText );
  cpuHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  cpuHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( cpuHeader );

  layout->addWidget( makePanel({
    makeCard( "CPU - Temp",      "°C", m_cpuTempLabel ),
    makeCard( "CPU - Fan",       "%",  m_fanSpeedLabel ),
    makeCard( "CPU - Frequency", "GHz", m_cpuFrequencyLabel ),
    makeCard( "CPU - Power",     "W",  m_cpuPowerLabel )
  }) );

  // GPU section — single section with toggle between dGPU and iGPU
  // Initial GPU header text: prefer dGPU model, fall back to iGPU model
  const QString gpuHeaderText = !m_dGpuModel.isEmpty() ? m_dGpuModel
                               : !m_iGpuModel.isEmpty() ? m_iGpuModel
                               : QStringLiteral( "Graphics Card Monitor" );
  m_gpuHeaderLabel = new QLabel( gpuHeaderText );
  m_gpuHeaderLabel->setStyleSheet( "font-size: 14px; font-weight: bold;" );

  m_gpuToggleButton = new QPushButton( "Show iGPU" );
  m_gpuToggleButton->setFixedHeight( 24 );
  m_gpuToggleButton->setStyleSheet(
    QString("QPushButton { font-size: 11px; padding: 2px 12px; border: 1px solid %1; border-radius: 4px; }"
            "QPushButton:hover { background-color: %2; }").arg(midHex, highlightHex) );
  m_gpuToggleButton->setVisible( false );  // Hidden until both GPUs detected

  // Same grid trick as title row: both share cell (0,0) — label centered, button right-aligned
  QGridLayout *gpuHeaderLayout = new QGridLayout();
  gpuHeaderLayout->setContentsMargins( 0, 0, 0, 0 );
  gpuHeaderLayout->addWidget( m_gpuHeaderLabel,  0, 0, Qt::AlignCenter );
  gpuHeaderLayout->addWidget( m_gpuToggleButton, 0, 0, Qt::AlignRight | Qt::AlignVCenter );
  layout->addLayout( gpuHeaderLayout );

  // dGPU panel (default view) — two-row box: main metrics + NVIDIA extended row (hidden until data)
  m_dGpuGaugeContainer = new QWidget();
  {
    QVBoxLayout *outerVL = new QVBoxLayout( m_dGpuGaugeContainer );
    outerVL->setContentsMargins( 0, 0, 0, 0 );
    outerVL->setSpacing( 0 );

    QFrame *panel = new QFrame();
    panel->setObjectName( "metricPanel" );
    panel->setStyleSheet( QString("QFrame#metricPanel { border: 2px solid %1; border-radius: 6px; background: transparent; }"
                                  "QWidget { background: transparent; }").arg(m_ringColorHex) );

    QVBoxLayout *panelVL = new QVBoxLayout( panel );
    panelVL->setContentsMargins( 0, 0, 0, 0 );
    panelVL->setSpacing( 0 );

    // Row 1: primary dGPU metrics
    panelVL->addWidget( makeCardRow({
      makeCard( "Temp",      "\u00b0C", m_gpuTempLabel ),
      makeCard( "Fan",       "%",   m_gpuFanSpeedLabel ),
      makeCard( "Core Frequency", "GHz", m_gpuFrequencyLabel ),
      makeCard( "Power",     "W",   m_gpuPowerLabel )
    }) );

    // Horizontal dashed separator (only visible when row 2 is shown)
    m_dGpuExtraHSep = new QFrame();
    m_dGpuExtraHSep->setFrameShape( QFrame::HLine );
    m_dGpuExtraHSep->setFixedHeight( 2 );
    m_dGpuExtraHSep->setStyleSheet( QString("QFrame { border: none; border-top: 2px dashed %1; background: transparent; margin: 0px 8px; }").arg(m_ringColorHex) );
    m_dGpuExtraHSep->setVisible( false );
    panelVL->addWidget( m_dGpuExtraHSep );

    // Row 2: NVIDIA extended metrics — hidden until data arrives
    m_dGpuExtraRow = makeCardRow({
      makeCard( "GPU Load",     "%",   m_gpuComputeUtilLabel ),
      makeCard( "VRAM Load",    "%",   m_gpuMemoryUtilLabel ),
      makeCard( "P-State",      "",    m_gpuPstateLabel ),
      makeCard( "Clock Offset", "MHz", m_gpuClockOffsetLabel )
    });
    m_gpuClockOffsetLabel->setStyleSheet( QString("font-size: 18px; font-weight: bold; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    m_dGpuExtraRow->setVisible( false );
    panelVL->addWidget( m_dGpuExtraRow );

    outerVL->addWidget( panel );
  }
  layout->addWidget( m_dGpuGaugeContainer );

  // iGPU panel (hidden by default)
  m_iGpuGaugeContainer = new QWidget();
  {
    QVBoxLayout *vl = new QVBoxLayout( m_iGpuGaugeContainer );
    vl->setContentsMargins( 0, 0, 0, 0 );
    vl->setSpacing( 0 );
    vl->addWidget( makePanel({
      makeCard( "iGPU - Temp",      "°C", m_iGpuTempLabel ),
      makeCard( "iGPU - Fan",       "%",  m_iGpuFanSpeedLabel ),
      makeCard( "iGPU - Frequency", "GHz", m_iGpuFrequencyLabel ),
      makeCard( "iGPU - Power",     "W",  m_iGpuPowerLabel )
    }) );
  }
  m_iGpuGaugeContainer->setVisible( false );
  layout->addWidget( m_iGpuGaugeContainer );

  // Water cooler section
  m_waterCoolerHeader = new QLabel( "Water Cooler Monitor" );
  m_waterCoolerHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  m_waterCoolerHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( m_waterCoolerHeader );

  m_waterCoolerGrid = new QGridLayout();
  m_waterCoolerGrid->setContentsMargins( 0, 0, 0, 0 );
  m_waterCoolerGrid->addWidget( makePanel({
    makeCard( "Water Cooler - Fan",  "%",     m_waterCoolerFanSpeedLabel ),
    makeCard( "Water Cooler - Pump", "Level", m_waterCoolerPumpLabel )
  }), 0, 0 );
  layout->addLayout( m_waterCoolerGrid );

  // Hide water cooler monitor section if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerHeader->setVisible( false );
    // Hide all widgets in the water cooler grid
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( auto *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( false );
    }
  }

  layout->addStretch();
}

void DashboardTab::setHardwareCheckStatus( QLabel *label, const QString &name, bool ok )
{
  if ( !label )
    return;

  const QString color = ok ? QStringLiteral( "#2e7d32" ) : QStringLiteral( "#d32f2f" );
  const QString mark = ok ? QStringLiteral( "✓" ) : QStringLiteral( "✕" );
  label->setText( QString( "%1: %2" ).arg( name, mark ) );
  label->setStyleSheet( QString( "font-size: 12px; font-weight: bold; color: %1;" ).arg( color ) );
}

void DashboardTab::runHardwareCheck()
{
  auto *client = m_profileManager ? m_profileManager->getClient() : nullptr;
  const bool connected = client && client->isConnected();

  bool cpuOk = false;
  bool gpuOk = false;
  bool keyboardBacklightOk = false;
  bool keyboardColorOk = false;
  bool cpuTdpOk = false;
  bool gpuTdpOk = false;

  if ( connected )
  {
    if ( auto cores = client->getCpuCoreCount() )
      cpuOk = *cores > 0;
    if ( !cpuOk )
      if ( auto freq = client->getCpuFrequency() )
        cpuOk = *freq > 0;
    if ( !cpuOk )
      if ( auto temp = client->getCpuTemperature() )
        cpuOk = *temp > 0;

    if ( auto gpuName = client->getNvidiaOCState( 0 ) )
    {
      const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *gpuName ) );
      if ( doc.isObject() )
      {
        const QJsonObject obj = doc.object();
        gpuOk = !obj.value( "gpuName" ).toString().isEmpty();
        gpuTdpOk = obj.value( "powerMaxW" ).toDouble( 0.0 ) > obj.value( "powerMinW" ).toDouble( 0.0 );
      }
    }

    if ( !gpuOk )
      gpuOk = !m_dGpuModel.isEmpty();
    if ( !gpuOk )
      if ( auto temp = client->getGpuTemperature() )
        gpuOk = *temp > 0;
    if ( !gpuOk )
      if ( auto power = client->getGpuPower() )
        gpuOk = *power >= 0.0;

    if ( auto keyboardInfo = client->getKeyboardBacklightInfo() )
    {
      const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *keyboardInfo ) );
      if ( doc.isObject() )
      {
        const QJsonObject caps = doc.object();
        keyboardBacklightOk = caps.value( "zones" ).toInt( 0 ) > 0 &&
                              caps.value( "maxBrightness" ).toInt( 0 ) > 0;
        keyboardColorOk = keyboardBacklightOk &&
                          caps.value( "maxRed" ).toInt( 0 ) > 0 &&
                          caps.value( "maxGreen" ).toInt( 0 ) > 0 &&
                          caps.value( "maxBlue" ).toInt( 0 ) > 0;
      }
    }

    if ( auto tdpLimits = client->getODMPowerLimits() )
      cpuTdpOk = !tdpLimits->empty();

    if ( !gpuTdpOk )
      gpuTdpOk = client->getNVIDIAPowerCTRLAvailable().value_or( false );
    if ( !gpuTdpOk )
    {
      const int defaultLimit = client->getNVIDIAPowerCTRLDefaultPowerLimit().value_or( 0 );
      const int maxLimit = client->getNVIDIAPowerCTRLMaxPowerLimit().value_or( 0 );
      gpuTdpOk = defaultLimit > 0 && maxLimit >= defaultLimit;
    }
  }

  setHardwareCheckStatus( m_hwCheckCpuLabel, "CPU", cpuOk );
  setHardwareCheckStatus( m_hwCheckGpuLabel, "GPU", gpuOk );
  setHardwareCheckStatus( m_hwCheckKeyboardBacklightLabel, "Keyboard Backlight", keyboardBacklightOk );
  setHardwareCheckStatus( m_hwCheckKeyboardColorLabel, "Keyboard Color", keyboardColorOk );
  setHardwareCheckStatus( m_hwCheckCpuTdpLabel, "CPU TDP", cpuTdpOk );
  setHardwareCheckStatus( m_hwCheckGpuTdpLabel, "GPU TDP", gpuTdpOk );
}

void DashboardTab::connectSignals()
{
  connect( m_systemMonitor, &SystemMonitor::cpuTempChanged,
           this, &DashboardTab::onCpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuFrequencyChanged,
           this, &DashboardTab::onCpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuPowerChanged,
           this, &DashboardTab::onCpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuTempChanged,
           this, &DashboardTab::onGpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFrequencyChanged,
           this, &DashboardTab::onGpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuPowerChanged,
           this, &DashboardTab::onGpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuFrequencyChanged,
           this, &DashboardTab::onIGpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuPowerChanged,
           this, &DashboardTab::onIGpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuTempChanged,
           this, &DashboardTab::onIGpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::fanSpeedChanged,
           this, &DashboardTab::onFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFanSpeedChanged,
           this, &DashboardTab::onGpuFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuComputeUtilChanged,
           this, &DashboardTab::onDGpuComputeUtilChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuMemoryUtilChanged,
           this, &DashboardTab::onDGpuMemoryUtilChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuPstateChanged,
           this, &DashboardTab::onDGpuPstateChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuGrClockOffsetChanged,
           this, &DashboardTab::onDGpuClockOffsetsChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuMemClockOffsetChanged,
           this, &DashboardTab::onDGpuClockOffsetsChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerFanSpeedChanged,
           this, &DashboardTab::onWaterCoolerFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerPumpLevelChanged,
           this, &DashboardTab::onWaterCoolerPumpLevelChanged );

  // Connect to profile manager for active profile changes
  connect( m_profileManager, &ProfileManager::activeProfileIndexChanged,
           this, [this]() {
             m_activeProfileLabel->setText( m_profileManager->activeProfileName() );
           } );

  Q_UNUSED(m_waterCoolerDbus)

  // Water cooler enable toggle button → emit signal for cross-tab sync and update status
  connect( m_waterCoolerEnableCheckBox, &QPushButton::toggled,
           this, [this]() {
             updateWaterCoolerStatus();
             emit waterCoolerEnableChanged( m_waterCoolerEnableCheckBox->isChecked() );
           } );

  // GPU toggle button switches between dGPU and iGPU views
  connect( m_gpuToggleButton, &QPushButton::clicked, this, [this]() {
    switchGpuView( !m_showingIGpu );
  } );
}

void DashboardTab::updateWaterCoolerStatus()
{
  if ( not m_waterCoolerDbus || not m_waterCoolerHeader )
    return;

  auto setWCStatus = [ this ]( const bool connected )
  {
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( QWidget *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( connected );
    }

    m_waterCoolerHeader->setVisible( connected );
  };

  // Check if water cooler is enabled
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;

  // Get water cooler state from daemon
  // Note: GetWaterCoolerAvailable returns true when scanning is active (not when a device is found)
  // GetWaterCoolerConnected returns true only when a device is actually connected
  QDBusReply<bool> scanning = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerAvailable"));
  QDBusReply<bool> connected = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerConnected"));

  // Compute explicit hex colors from the current palette so styles are consistent.
  QPalette pal = this->palette();
  const QString textHex = pal.color(QPalette::WindowText).name();
  const QString midHex = pal.color(QPalette::Mid).name();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QString searchingColorHex = QStringLiteral("#0066cc");  // Dark blue for searching

  // Helper: emit status bar signal (dashboard label is hidden).
  auto emitStatus = [this]( const QString &statusText, const QString &colorHex )
  {
    emit waterCoolerStatusChanged(
      QString("<span style='color: %1;'>&#9679;</span> Water Cooler: %2").arg( colorHex, statusText ) );
  };

  // Status progression: Disabled → Disconnected → Searching → Connected
  if ( !wcEnabled )
  {
    emitStatus( QStringLiteral("Disabled"), m_ringColorHex );
    setWCStatus( false );
  }
  else if ( connected.isValid() && connected.value() )
  {
    emitStatus( QStringLiteral("Connected"), highlightHex );
    setWCStatus( true );
  }
  else if ( scanning.isValid() && scanning.value() )
  {
    // GetWaterCoolerAvailable == true means the daemon is actively scanning
    emitStatus( QStringLiteral("Searching..."), searchingColorHex );
    setWCStatus( false );
  }
  else
  {
    emitStatus( QStringLiteral("Disconnected"), m_ringColorHex );
    setWCStatus( false );
  }
}

void DashboardTab::refreshWaterCoolerStatus()
{
  updateWaterCoolerStatus();
}

// Dashboard slots
void DashboardTab::onCpuTempChanged()
{
  QString temp = m_systemMonitor->cpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_cpuTempLabel->setText( temp );
  }
  else
  {
    m_cpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onCpuFrequencyChanged()
{
  QString freq = m_systemMonitor->cpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_cpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        return;
      }
      m_cpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_cpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onCpuPowerChanged()
{
  QString power = m_systemMonitor->cpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_cpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_cpuPowerLabel->setText( "--" );
}

void DashboardTab::onGpuTempChanged()
{
  QString temp = m_systemMonitor->gpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_gpuTempLabel->setText( temp );
    if ( !m_hasDGpuData ) { m_hasDGpuData = true; updateGpuSwitchVisibility(); }
  }
  else
  {
    m_gpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onGpuFrequencyChanged()
{
  QString freq = m_systemMonitor->gpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_gpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        if ( !m_hasDGpuData ) { m_hasDGpuData = true; updateGpuSwitchVisibility(); }
        return;
      }
      m_gpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_gpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onGpuPowerChanged()
{
  QString power = m_systemMonitor->gpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_gpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_gpuPowerLabel->setText( "--" );
}

void DashboardTab::onIGpuFrequencyChanged()
{
  QString freq = m_systemMonitor->iGpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_iGpuFrequencyLabel->setText( QString::number( ghz, 'f', 2 ) );
        if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
        return;
      }
      m_iGpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_iGpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onIGpuPowerChanged()
{
  QString power = m_systemMonitor->iGpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_iGpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
    return;
  }
  m_iGpuPowerLabel->setText( "--" );
}

void DashboardTab::onIGpuTempChanged()
{
  QString temp = m_systemMonitor->iGpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_iGpuTempLabel->setText( temp );
    if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
  }
  else
  {
    m_iGpuTempLabel->setText( "---" );
  }
}

// Water cooler status slots
void DashboardTab::onWaterCoolerConnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDisconnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryStarted()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryFinished()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerConnectionError( const QString &error )
{
  Q_UNUSED( error );
  updateWaterCoolerStatus();
}

// Note: status is determined by daemon; this function queries it and updates label/color
// (The DBus-backed implementation is the single source of truth.)

void DashboardTab::onFanSpeedChanged()
{
  m_fanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->cpuFanSpeed() ) );
}

void DashboardTab::onGpuFanSpeedChanged()
{
  m_gpuFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->gpuFanSpeed() ) );
}

void DashboardTab::onDGpuComputeUtilChanged()
{
  int val = m_systemMonitor->dGpuComputeUtil();
  if ( val >= 0 )
  {
    m_gpuComputeUtilLabel->setText( QString::number( val ) );
    if ( m_dGpuExtraRow && !m_dGpuExtraRow->isVisible() )
    {
      m_dGpuExtraRow->setVisible( true );
      if ( m_dGpuExtraHSep ) m_dGpuExtraHSep->setVisible( true );
    }
  }
  else
    m_gpuComputeUtilLabel->setText( "--" );
}

void DashboardTab::onDGpuMemoryUtilChanged()
{
  int val = m_systemMonitor->dGpuMemoryUtil();
  m_gpuMemoryUtilLabel->setText( val >= 0 ? QString::number( val ) : "--" );
}

void DashboardTab::onDGpuPstateChanged()
{
  int val = m_systemMonitor->dGpuPstate();
  m_gpuPstateLabel->setText( val >= 0 ? QStringLiteral( "P" ) + QString::number( val ) : "--" );
}

void DashboardTab::onDGpuClockOffsetsChanged()
{
  int gr  = m_systemMonitor->dGpuGrClockOffset();
  int mem = m_systemMonitor->dGpuMemClockOffset();
  if ( gr > -999 || mem > -999 )
  {
    auto fmt = []( int v ) -> QString {
      return ( v >= 0 ? QStringLiteral( "+" ) : QString() ) + QString::number( v );
    };
    m_gpuClockOffsetLabel->setText(
      ( gr  > -999 ? fmt( gr )  : "--" ) + " / " +
      ( mem > -999 ? fmt( mem ) : "--" ) );
  }
  else
    m_gpuClockOffsetLabel->setText( "--" );
}

void DashboardTab::onWaterCoolerFanSpeedChanged()
{
  if ( m_waterCoolerFanSpeedLabel )
    m_waterCoolerFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->waterCoolerFanSpeed() ) );
}

void DashboardTab::onWaterCoolerPumpLevelChanged()
{
  if ( m_waterCoolerPumpLabel )
  {
    QString val = m_systemMonitor->waterCoolerPumpLevel();
    if ( val.isEmpty() )
      val = "--";
    m_waterCoolerPumpLabel->setText( val );
  }
}

void DashboardTab::setWaterCoolerEnabled( bool enabled )
{
  m_waterCoolerEnableCheckBox->blockSignals( true );
  m_waterCoolerEnableCheckBox->setChecked( enabled );
  m_waterCoolerEnableCheckBox->blockSignals( false );
}

void DashboardTab::switchGpuView( bool showIGpu )
{
  m_showingIGpu = showIGpu;
  m_dGpuGaugeContainer->setVisible( !showIGpu );
  m_iGpuGaugeContainer->setVisible( showIGpu );
  m_gpuToggleButton->setText( showIGpu ? "Show dGPU" : "Show iGPU" );

  // Update header to reflect which GPU is being shown
  if ( m_gpuHeaderLabel )
  {
    const QString headerText = showIGpu
      ? ( m_iGpuModel.isEmpty() ? QStringLiteral( "Integrated GPU" ) : m_iGpuModel )
      : ( m_dGpuModel.isEmpty() ? QStringLiteral( "Discrete GPU" )   : m_dGpuModel );
    m_gpuHeaderLabel->setText( headerText );
  }
}

void DashboardTab::updateGpuSwitchVisibility()
{
  // Show the toggle button only when both dGPU and iGPU data is available
  const bool bothAvailable = m_hasDGpuData && m_hasIGpuData;
  m_gpuToggleButton->setVisible( bothAvailable );

  // If only iGPU data exists (no dGPU), automatically show iGPU view
  if ( m_hasIGpuData && !m_hasDGpuData )
    switchGpuView( true );
}

}
