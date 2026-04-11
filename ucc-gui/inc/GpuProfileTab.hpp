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
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QSettings>
#include <vector>

#include "../../libucc-dbus/UccdClient.hpp"
#include "ProfileManager.hpp"

namespace ucc
{

/**
 * @brief GPU OC Profile Tab widget.
 *
 * Contains the GPU profile selection bar (editable combo + Apply/Save/Copy/Remove),
 * P-state clock offset sliders, locked clock controls, and power limit slider.
 * Mirrors the fan/keyboard profile tab pattern.
 */
class GpuProfileTab : public QWidget
{
  Q_OBJECT

public:
  explicit GpuProfileTab( UccdClient *client,
                          ProfileManager *profileManager,
                          QWidget *parent = nullptr );
  ~GpuProfileTab() override = default;

  // ── Accessors used by MainWindow ──
  QComboBox *gpuProfileCombo() const { return m_gpuProfileCombo; }
  QPushButton *applyButton()  const { return m_applyButton; }
  QPushButton *saveButton()   const { return m_saveButton; }
  QPushButton *copyButton()   const { return m_copyButton; }
  QPushButton *removeButton() const { return m_removeButton; }

  /** Reload combo items from custom store. */
  void reloadGpuProfiles();

  /** Update button enable states. */
  void updateButtonStates( bool uccdConnected );

  /** Refresh hardware state from daemon (reads NVML OC state). */
  void refreshOCState();

  /** Build a JSON string representing the current tab UI state (for saving). */
  QString buildProfileJSON() const;

  /** Load a GPU OC profile JSON into the tab widgets. */
  void loadProfile( const QString &json );

  /** Whether NVIDIA OC is available on this system. */
  bool isOCAvailable() const { return m_ocAvailable; }

signals:
  void applyRequested();
  void saveRequested();
  void copyRequested();
  void removeRequested();
  void gpuProfileChanged( const QString &gpuProfileId );
  void gpuProfileRenamed( const QString &oldName, const QString &newName );
  void changed();   ///< emitted when any slider/spin is modified

private slots:
  void onGpuProfileComboRenamed();
  void onRefreshClicked();
  void onResetClicked();

private:
  void setupUI();
  void connectSignals();
  void populatePStates( const QJsonArray &pstates );
  void clearPStateWidgets();
  bool ensureOverclockWarningAcknowledged();
  bool isOverclockWarningAcknowledged() const;
  void setOverclockWarningAcknowledged( bool acknowledged );
  bool showOverclockWarningDialog();

  UccdClient *m_uccdClient;
  ProfileManager *m_profileManager;
  bool m_ocAvailable = false;
  bool m_offsetsSupported = false;
  bool m_lockedSupported = false;

  // Profile selection bar
  QComboBox *m_gpuProfileCombo = nullptr;
  QPushButton *m_applyButton = nullptr;
  QPushButton *m_saveButton = nullptr;
  QPushButton *m_copyButton = nullptr;
  QPushButton *m_removeButton = nullptr;

  // GPU info labels
  QLabel *m_gpuNameLabel = nullptr;
  QLabel *m_tempLabel = nullptr;
  QLabel *m_powerDrawLabel = nullptr;
  QLabel *m_currentPstateLabel = nullptr;

  // Per-P-state offset controls (GPU core + VRAM grouped together)
  struct PStateOffsetRow
  {
    unsigned int pstate;
    bool isGpu;           // true = GPU core, false = VRAM
    QSlider *slider;
    QSpinBox *spinBox;
  };
  struct PStateGroup
  {
    unsigned int pstate;
    QGroupBox *groupBox;
    PStateOffsetRow gpuRow;
    PStateOffsetRow vramRow;
  };
  std::vector< PStateGroup > m_pstateGroups;
  QVBoxLayout *m_pstatesLayout = nullptr;

  // Locked clocks
  QGroupBox *m_gpuLockedGroup = nullptr;
  QGroupBox *m_vramLockedGroup = nullptr;
  QSlider *m_gpuLockedMinSlider = nullptr;
  QSlider *m_gpuLockedMaxSlider = nullptr;
  QSpinBox *m_gpuLockedMinSpin = nullptr;
  QSpinBox *m_gpuLockedMaxSpin = nullptr;

  QSlider *m_vramLockedMinSlider = nullptr;
  QSlider *m_vramLockedMaxSlider = nullptr;
  QSpinBox *m_vramLockedMinSpin = nullptr;
  QSpinBox *m_vramLockedMaxSpin = nullptr;

  // Power limit
  QSlider *m_powerLimitSlider = nullptr;
  QLabel *m_powerLimitValue = nullptr;
  double m_powerMinW = 0.0;
  double m_powerMaxW = 0.0;
  double m_powerDefaultW = 0.0;
  bool m_nvmlPowerLimitSupported = false;

  // Action buttons
  QPushButton *m_refreshButton = nullptr;
  QPushButton *m_resetButton = nullptr;

  // Warnings
  QLabel *m_notAvailableLabel = nullptr;

  // Live metrics refresh
  QTimer *m_liveMetricsTimer = nullptr;

  void refreshLiveMetrics();
};

} // namespace ucc
