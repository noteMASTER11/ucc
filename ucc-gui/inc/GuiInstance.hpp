#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

namespace ucc {
class GuiInstance : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "com.uniwill.UccGui")
public:
  enum class Result { Primary, ActivatedExisting, Error };
  explicit GuiInstance(QObject *parent = nullptr) : QObject(parent) {}
  ~GuiInstance() override;
  Result startOrActivate();
  void setWindow(QWidget *window);
public slots:
  void Activate();
private:
  QPointer<QWidget> m_window;
  bool m_activationPending = false;
  bool m_primary = false;
};
}
