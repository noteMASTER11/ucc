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

#pragma once

#include <QWidget>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDBusInterface>
#include <QTimer>

namespace ucc
{
  class SystemMonitor;
  class ProfileManager;


  /**
   * @brief Dashboard tab widget for system monitoring
   */
  class DashboardTab : public QWidget
  {
    Q_OBJECT

  public:
    explicit DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager, bool waterCoolerSupported,
                          const QString &laptopModel = {},
                          const QString &cpuModel = {},
                          const QString &dGpuModel = {},
                          const QString &iGpuModel = {},
                          QWidget *parent = nullptr );
    ~DashboardTab() override = default;

    /** Update the water-cooler enable checkbox without re-triggering signals. */
    void setWaterCoolerEnabled( bool enabled );

    /** Trigger an immediate water-cooler status query and emit waterCoolerStatusChanged. */
    void refreshWaterCoolerStatus();

  signals:
    void waterCoolerEnableChanged( bool enabled );
    /** Emitted whenever the water-cooler status changes (rich-text, e.g. for the status bar). */
    void waterCoolerStatusChanged( const QString &richText );

  private slots:
    void onCpuTempChanged();
    void onCpuFrequencyChanged();
    void onCpuPowerChanged();
    void onGpuTempChanged();
    void onGpuFrequencyChanged();
    void onGpuPowerChanged();
    void onIGpuFrequencyChanged();
    void onIGpuPowerChanged();
    void onIGpuTempChanged();
    void onFanSpeedChanged();
    void onGpuFanSpeedChanged();
    void onDGpuComputeUtilChanged();
    void onDGpuMemoryUtilChanged();
    void onDGpuPstateChanged();
    void onDGpuClockOffsetsChanged();
    void onWaterCoolerConnected();
    void onWaterCoolerDisconnected();
    void onWaterCoolerDiscoveryStarted();
    void onWaterCoolerDiscoveryFinished();
    void onWaterCoolerConnectionError(const QString &error);
    void onWaterCoolerFanSpeedChanged();
    void onWaterCoolerPumpLevelChanged();

  private:
    void setupUI();
    void connectSignals();
    void updateWaterCoolerStatus();
    void runHardwareCheck();
    void setHardwareCheckStatus( QLabel *label, const QString &name, bool ok );
    void switchGpuView( bool showIGpu );
    void updateGpuSwitchVisibility();

    SystemMonitor *m_systemMonitor;
    ProfileManager *m_profileManager;
    // DBus interface for water cooler status
    QDBusInterface *m_waterCoolerDbus = nullptr;
    QTimer *m_waterCoolerPollTimer = nullptr;

    // Dashboard widgets
    QLabel *m_activeProfileLabel = nullptr;
    QLabel *m_waterCoolerStatusLabel = nullptr;
    QLabel *m_hwCheckCpuLabel = nullptr;
    QLabel *m_hwCheckGpuLabel = nullptr;
    QLabel *m_hwCheckKeyboardBacklightLabel = nullptr;
    QLabel *m_hwCheckKeyboardColorLabel = nullptr;
    QLabel *m_hwCheckCpuTdpLabel = nullptr;
    QLabel *m_hwCheckGpuTdpLabel = nullptr;
    QLabel *m_cpuTempLabel = nullptr;
    QLabel *m_cpuFrequencyLabel = nullptr;
    QLabel *m_gpuTempLabel = nullptr;
    QLabel *m_gpuFrequencyLabel = nullptr;
    QLabel *m_fanSpeedLabel = nullptr;
    QLabel *m_gpuFanSpeedLabel = nullptr;
    QLabel *m_cpuPowerLabel = nullptr;
    QLabel *m_gpuPowerLabel = nullptr;
    QLabel *m_iGpuTempLabel = nullptr;
    QLabel *m_iGpuFanSpeedLabel = nullptr;
    QLabel *m_iGpuFrequencyLabel = nullptr;
    QLabel *m_iGpuPowerLabel = nullptr;
    // Extended dGPU metrics (second row, NVIDIA-only)
    QLabel *m_gpuComputeUtilLabel = nullptr;
    QLabel *m_gpuMemoryUtilLabel  = nullptr;
    QLabel *m_gpuPstateLabel      = nullptr;
    QLabel *m_gpuClockOffsetLabel = nullptr;
    QWidget *m_dGpuExtraRow  = nullptr;
    QFrame  *m_dGpuExtraHSep = nullptr;
    QLabel *m_waterCoolerFanSpeedLabel = nullptr;
    QLabel *m_waterCoolerPumpLabel = nullptr;
    QGridLayout *m_waterCoolerGrid = nullptr;
    QLabel *m_waterCoolerHeader = nullptr;
    QPushButton *m_waterCoolerEnableCheckBox = nullptr;

    bool m_waterCoolerSupported = false;

    // System hardware info strings
    QString m_laptopModel;
    QString m_cpuModel;
    QString m_dGpuModel;
    QString m_iGpuModel;

    // GPU section header label (updated when toggling dGPU / iGPU)
    QLabel *m_gpuHeaderLabel = nullptr;

    // GPU view toggle (dGPU / iGPU)
    QPushButton *m_gpuToggleButton = nullptr;
    QWidget *m_dGpuGaugeContainer = nullptr;
    QWidget *m_iGpuGaugeContainer = nullptr;
    bool m_showingIGpu = false;
    bool m_hasDGpuData = false;
    bool m_hasIGpuData = false;

    // Theme colors
    QString m_ringColorHex = "#d32f2f";  // Red for disconnected/disabled state
  };
}
