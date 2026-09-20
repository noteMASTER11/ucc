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
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QDBusInterface>
#include <QTimer>

#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "LCTWaterCoolerController.hpp"
#include "CommonTypes.hpp"
#include "../../libucc-dbus/UccdClient.hpp"
#include "ProfileManager.hpp"

namespace ucc
{

/**
 * @brief Fan control tab widget.
 *
 * Contains the fan profile selection bar, two sub-tabs
 * ("System CPU/GPU" and "Water Cooler"), all fan curve editors,
 * the pump voltage curve editor and the hardware water-cooler
 * controls (enable, pump manual, LED, colour).
 */
class FanControlTab : public QWidget
{
  Q_OBJECT

public:
  explicit FanControlTab( UccdClient *client,
                          ProfileManager *profileManager,
                          bool waterCoolerSupported,
                          QWidget *parent = nullptr );
  ~FanControlTab() override = default;

  // Accessors used by MainWindow
  QComboBox *fanProfileCombo() const { return m_fanProfileCombo; }
  const QStringList &builtinFanProfiles() const { return m_builtinFanProfiles; }

  FanCurveEditorWidget *cpuEditor()  const { return m_cpuFanCurveEditor; }
  FanCurveEditorWidget *gpuEditor()  const { return m_gpuFanCurveEditor; }
  FanCurveEditorWidget *wcFanEditor() const { return m_waterCoolerFanCurveEditor; }
  PumpCurveEditorWidget *pumpEditor() const { return m_pumpCurveEditor; }

  QPushButton *applyButton()  const { return m_applyFanProfilesButton; }
  QPushButton *saveButton()   const { return m_saveFanProfilesButton; }
  QPushButton *copyButton()   const { return m_copyFanProfileButton; }
  QPushButton *removeButton() const { return m_removeFanProfileButton; }
  QPushButton *revertButton() const { return m_revertFanProfilesButton; }

  /** Update the water-cooler enable checkbox without re-triggering signals. */
  void setWaterCoolerEnabled( bool enabled );
  void sendWaterCoolerEnable( bool enabled );
  bool isWaterCoolerEnabled() const;

  /** Update water cooler manual control state based on auto control setting. */
  void setWaterCoolerAutoControl( bool autoControl );

  /** Start/stop water cooler polling based on enable state. */
  void updateWaterCoolerPolling();

  QString currentFanProfile() const { return m_currentFanProfile; }
  void setCurrentFanProfile( const QString &name ) { m_currentFanProfile = name; }

  /** Reload combo items from daemon + custom store. */
  void reloadFanProfiles();

  /** Update button enable states. */
  void updateButtonStates( bool uccdConnected );

  /** Set editor editability. */
  void setEditorsEditable( bool editable );

signals:
  void applyRequested();
  void saveRequested();
  void revertRequested();
  void copyRequested();
  void removeRequested();
  void fanProfileChanged( const QString &fanProfileId );
  void fanProfileRenamed( const QString &oldName, const QString &newName );
  void cpuPointsChanged( const QVector<FanCurveEditorWidget::Point> &points );
  void gpuPointsChanged( const QVector<FanCurveEditorWidget::Point> &points );
  void wcFanPointsChanged( const QVector<FanCurveEditorWidget::Point> &points );
  void pumpPointsChanged( const QVector<PumpCurveEditorWidget::Point> &points );
  void waterCoolerEnableChanged( bool enabled );

private slots:
  // Water cooler hardware slots
  void onWaterCoolerEnableToggled( bool enabled );
  void onConnected();
  void onDisconnected();
  void onPumpVoltageChanged( int index );
  void onFanSpeedChanged( int speed );
  void onLEDOnOffChanged( bool enabled );
  void onLEDModeChanged( int index );
  void onColorPickerClicked();
  void onFanProfileComboRenamed();

private:
  void setupUI();
  void connectSignals();
  void updateColorButtonState();
  void updateManualControlState();
  void pollWaterCoolerStatus();

  UccdClient *m_uccdClient;
  ProfileManager *m_profileManager;

  // Fan profile selection bar
  QComboBox *m_fanProfileCombo = nullptr;
  QPushButton *m_applyFanProfilesButton = nullptr;
  QPushButton *m_saveFanProfilesButton = nullptr;
  QPushButton *m_revertFanProfilesButton = nullptr;
  QPushButton *m_copyFanProfileButton = nullptr;
  QPushButton *m_removeFanProfileButton = nullptr;
  QStringList m_builtinFanProfiles;
  QString m_currentFanProfile;

  // Fan curve editors
  FanCurveEditorWidget *m_cpuFanCurveEditor = nullptr;
  FanCurveEditorWidget *m_gpuFanCurveEditor = nullptr;
  FanCurveEditorWidget *m_waterCoolerFanCurveEditor = nullptr;
  PumpCurveEditorWidget *m_pumpCurveEditor = nullptr;

  // Water cooler hardware controls (moved from HardwareTab)
  QDBusInterface *m_waterCoolerDbus = nullptr;
  QTimer *m_waterCoolerPollTimer = nullptr;
  bool m_isWcConnected = false;
  QPushButton *m_waterCoolerEnableCheckBox = nullptr;
  QComboBox *m_pumpVoltageCombo = nullptr;
  QCheckBox *m_ledOnOffCheckBox = nullptr;
  QPushButton *m_colorPickerButton = nullptr;
  QComboBox *m_ledModeCombo = nullptr;
  QSlider *m_fanSpeedSlider = nullptr;
  int m_currentRed = 255;
  int m_currentGreen = 0;
  int m_currentBlue = 0;

  bool m_autoControl = true;
  bool m_waterCoolerReadPending = false;
  bool m_waterCoolerSupported = false;
};

} // namespace ucc
