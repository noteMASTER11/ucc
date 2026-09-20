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

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QList>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <functional>

#include "CommonTypes.hpp"

// Forward declarations
class UccDBusData;

/**
 * @brief Water cooler connection states
 */
enum class WaterCoolerState
{
  Disconnected,
  Discovering,
  Reconnecting, // Fast-path: direct connect to known device (skips discovery)
  Connecting,
  Connected,
  Error
};

/**
 * @brief LCT Water Cooler Worker
 *
 * Single-class BLE water cooler manager that runs entirely on the main thread.
 * Uses a QTimer for periodic tick instead of a dedicated QThread, eliminating
 * all cross-thread dispatch overhead for the internal state machine.
 *
 * Handles BLE device discovery, connection and control of fan, pump and LED.
 *
 * Thread safety: public control methods (setFanSpeed, setPumpVoltage, etc.)
 * are safe to call from any thread - they dispatch to the main thread when
 * needed via BlockingQueuedConnection.
 */
class LCTWaterCoolerWorker : public QObject
{
  Q_OBJECT

public:
  /**
   * @brief Status change callback type
   */
  using StatusCallback = std::function< void( WaterCoolerState ) >;

  /**
   * @brief Constructor
   *
   * @param dbusData Reference to shared DBus data container (includes mutex)
   * @param statusCallback Callback function called when status changes
   * @param parent Optional parent QObject
   */
  explicit LCTWaterCoolerWorker( UccDBusData& dbusData, StatusCallback statusCallback = nullptr,
                                 QObject* parent = nullptr );

  ~LCTWaterCoolerWorker() override;

  // Prevent copy and move
  LCTWaterCoolerWorker( const LCTWaterCoolerWorker& ) = delete;
  LCTWaterCoolerWorker( LCTWaterCoolerWorker&& ) = delete;
  LCTWaterCoolerWorker& operator=( const LCTWaterCoolerWorker& ) = delete;
  LCTWaterCoolerWorker& operator=( LCTWaterCoolerWorker&& ) = delete;

  /**
   * @brief Start the periodic tick timer and begin automatic BLE discovery
   */
  void start();

  /**
   * @brief Stop the periodic tick timer
   */
  void stop();

  /**
   * @brief Check if connected to device (thread-safe)
   * @return true if connected
   */
  bool isConnected() const;

  /**
   * @brief Set fan speed (thread-safe - can be called from any thread)
   * @param dutyCyclePercent Fan speed percentage (0-100)
   * @return true if command sent successfully
   */
  bool setFanSpeed( int dutyCyclePercent );

  /**
   * @brief Set pump voltage (thread-safe - can be called from any thread)
   * @param voltage Pump voltage setting
   * @return true if command sent successfully
   */
  bool setPumpVoltage( int voltage );

  /**
   * @brief Get last set pump voltage (thread-safe)
   * @return Last pump voltage setting
   */
  int32_t getLastPumpVoltage() const;

  /**
   * @brief Set LED color (thread-safe - can be called from any thread)
   * @param red Red component (0-255)
   * @param green Green component (0-255)
   * @param blue Blue component (0-255)
   * @param mode RGB mode
   * @return true if command sent successfully
   */
  bool setLEDColor( int red, int green, int blue, int mode );

  /**
   * @brief Turn off LED (thread-safe)
   * @return true if command sent successfully
   */
  bool turnOffLED();

  /**
   * @brief Turn off fan (thread-safe)
   * @return true if command sent successfully
   */
  bool turnOffFan();

  /**
   * @brief Turn off pump (thread-safe)
   * @return true if command sent successfully
   */
  bool turnOffPump();

  // External control helpers (called by service to request actions)
  void startScanning();
  void stopScanning();
  void disconnectFromDevice();

  inline int32_t getLastFanSpeed() const
  {
    return m_lastFanSpeed.load();
  }

private slots:
  void onTick();

  // BLE signal handlers
  void onDeviceDiscovered( const QBluetoothDeviceInfo& device );
  void onDiscoveryFinished();
  void onBleConnected();
  void onBleDisconnected();
  void onServiceDiscoveryFinished();
  void onServiceStateChanged( QLowEnergyService::ServiceState state );
  void onCharacteristicRead( const QLowEnergyCharacteristic& characteristic, const QByteArray& value );
  void onCharacteristicWritten( const QLowEnergyCharacteristic& characteristic, const QByteArray& value );
  void onBleError( QLowEnergyController::Error error );

  // Thread-safe invokable implementations (for cross-thread dispatch)
  Q_INVOKABLE bool setFanSpeedImpl( int dutyCyclePercent );
  Q_INVOKABLE bool setPumpVoltageImpl( int voltage );
  Q_INVOKABLE bool writeCommandImpl( const QByteArray& data, bool withResponse );

private:
  // State machine handlers
  void handleDisconnected();
  void handleDiscovering();
  void handleReconnecting();
  void handleConnecting();
  void handleConnected();
  void handleError();
  void recordConnectionFailure();
  void onConnectionReady();

  // Discovery helpers
  bool ensureDiscoveryAgent();
  bool startDiscoveryInternal();
  void stopDiscoveryInternal();

  // Connection helpers
  bool connectToDevice( const QString& deviceUuid );
  bool connectToKnownDevice();
  void disconnectFromDeviceInternal();

  // BLE helpers
  bool setupBleController( const QBluetoothDeviceInfo& deviceInfo );
  void cleanupBleController();
  void throttledBleWrite( const QByteArray& data );
  bool writeCommand( const QByteArray& data );
  bool writeReceive( const QByteArray& data );
  bool turnOffDevice( uint8_t cmd );
  bool resetBluetoothAdapter();
  bool purgeBlueZDeviceCache( const QString& macAddress );

  // Suspend / resume
  void setupSuspendResumeHandling();
  Q_SLOT void onPrepareForSleep( bool suspending );

  // BLE keepalive
  void sendKeepaliveProbe();

  // State machine helpers
  void requestStartDiscovery();
  void requestConnectToDevice( const QString& uuid );
  void setAvailableFlag( bool available );
  int64_t secondsSinceLastDiscovery() const;
  ucc::LCTDeviceModel deviceModelFromName( const QString& name ) const;

  // BLE constants
  static const QString NORDIC_UART_SERVICE_UUID;
  static const QString NORDIC_UART_CHAR_TX;
  static const QString NORDIC_UART_CHAR_RX;
  static const QBluetoothUuid NORDIC_UART_SERVICE_UUID_OBJ;
  static const QBluetoothUuid NORDIC_UART_CHAR_TX_OBJ;
  static const QBluetoothUuid NORDIC_UART_CHAR_RX_OBJ;

  static constexpr uint8_t CMD_RESET = 0x19;
  static constexpr uint8_t CMD_FAN = 0x1b;
  static constexpr uint8_t CMD_PUMP = 0x1c;
  static constexpr uint8_t CMD_RGB = 0x1e;

  // Device info (private, replaces ucc::DeviceInfo)
  struct DeviceInfo
  {
    QString uuid;
    QString name;
    int rssi = 0;
    QBluetoothDeviceInfo deviceInfo;
  };

  // State machine
  UccDBusData& m_dbusData;
  std::atomic<WaterCoolerState> m_state = WaterCoolerState::Disconnected;
  WaterCoolerState m_previousState = WaterCoolerState::Disconnected;
  std::chrono::steady_clock::time_point m_lastDiscoveryStart;
  std::chrono::steady_clock::time_point m_errorEntryTime;
  int m_consecutiveFailures = 0;
  int m_fastReconnectFailures = 0;
  StatusCallback m_statusCallback;
  QTimer* m_tickTimer = nullptr;

  // BLE infrastructure
  QBluetoothDeviceDiscoveryAgent* m_deviceDiscoveryAgent = nullptr;
  QLowEnergyController* m_bleController = nullptr;
  QLowEnergyService* m_uartService = nullptr;
  QLowEnergyCharacteristic m_txCharacteristic;
  QLowEnergyCharacteristic m_rxCharacteristic;

  QBluetoothDeviceInfo m_connectedDeviceInfo;
  QBluetoothDeviceInfo m_lastKnownDeviceInfo;
  bool m_hasKnownDevice = false;

  // Store trusted MAC to prevent impersonation
  QString m_trustedDeviceMacAddress;
  QString m_savedDeviceAddress;
  QString m_savedDeviceName;
  bool m_hasMigrationConfig = false;

  QList< DeviceInfo > m_discoveredDevices;
  ucc::LCTDeviceModel m_connectedModel = ucc::LCTDeviceModel::LCT21001;
  std::atomic< bool > m_isConnected{ false };
  bool m_isDiscovering = false;

  // Track last sent values (atomic for cross-thread reads from DBus/FanControl)
  std::atomic< int > m_lastFanSpeed{ -1 };
  std::atomic< int > m_lastPumpVoltage{ -1 };
  std::atomic< int > m_lastLedR{ -1 };
  std::atomic< int > m_lastLedG{ -1 };
  std::atomic< int > m_lastLedB{ -1 };
  std::atomic< int > m_lastLedMode{ -1 };

  bool m_waitingForResponse = false;
  QByteArray m_pendingData;

  // BLE write throttle - minimum gap between successive UART writes
  static constexpr int BLE_WRITE_GAP_MS = 80;
  std::chrono::steady_clock::time_point m_lastBleWrite{};

  // Keepalive tracking
  std::chrono::steady_clock::time_point m_lastKeepalive{};
  int m_missedKeepalives = 0;

  // Suspend/resume state
  bool m_suspending = false;
};
