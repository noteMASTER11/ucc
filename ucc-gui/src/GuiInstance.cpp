#include "GuiInstance.hpp"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusError>
#include <QDebug>
#include <QWindow>

namespace ucc {
namespace {
constexpr auto service = "com.uniwill.UccGui";
constexpr auto path = "/com/uniwill/UccGui";
}

GuiInstance::~GuiInstance() {
  if (m_primary) {
    auto bus = QDBusConnection::sessionBus();
    bus.unregisterService(service);
    bus.unregisterObject(path);
  }
}

GuiInstance::Result GuiInstance::startOrActivate() {
  if (m_primary) return Result::Primary;
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected() ||
      !bus.registerObject(path, this, QDBusConnection::ExportAllSlots)) {
    qWarning() << "Cannot register the GUI session endpoint:" << bus.lastError().message();
    return Result::Error;
  }
  // D-Bus ownership is atomic, including simultaneous launches. Claim it before
  // constructing MainWindow or any hardware client.
  if (bus.registerService(service)) {
    m_primary = true;
    return Result::Primary;
  }
  bus.unregisterObject(path);
  const auto request = QDBusMessage::createMethodCall(service, path, service, "Activate");
  const auto reply = bus.call(request, QDBus::Block, 3000);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    qWarning() << "Could not activate the existing GUI:" << reply.errorMessage();
    return Result::Error;
  }
  return Result::ActivatedExisting;
}

void GuiInstance::setWindow(QWidget *window) {
  m_window = window;
  if (m_activationPending) Activate();
}

void GuiInstance::Activate() {
  m_activationPending = !m_window;
  if (!m_window) return;
  m_window->setWindowState(m_window->windowState() & ~Qt::WindowMinimized);
  m_window->show();
  m_window->raise();
  m_window->activateWindow();
  if (auto *handle = m_window->windowHandle()) handle->requestActivate();
}
}
