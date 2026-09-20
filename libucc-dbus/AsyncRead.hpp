#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <optional>

namespace ucc {
// Watchers belong to the consumer, so destroying a tab discards its late replies.
inline void readUccdAsync(QObject *consumer, const QString &method, const QVariantList &args,
                          std::function<void(std::optional<QVariant>)> ready)
{
  auto message = QDBusMessage::createMethodCall("com.uniwill.uccd", "/com/uniwill/uccd",
                                                "com.uniwill.uccd", method);
  message.setArguments(args);
  message.setAutoStartService(false);
  auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 1500), consumer);
  QObject::connect(watcher, &QDBusPendingCallWatcher::finished, consumer,
    [ready = std::move(ready)](QDBusPendingCallWatcher *done) {
      const auto reply = done->reply();
      done->deleteLater();
      if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) ready(std::nullopt);
      else ready(reply.arguments().front());
    });
}

inline void readUccdBatch(QObject *consumer, const QStringList &methods,
                          std::function<void(QVariantMap)> ready)
{
  if (methods.isEmpty()) { ready({}); return; }
  struct Batch { QVariantMap values; qsizetype remaining; };
  auto batch = std::make_shared<Batch>();
  batch->remaining = methods.size();
  for (const auto &method : methods)
    readUccdAsync(consumer, method, {}, [batch,method,ready](std::optional<QVariant> value) {
      if (value) batch->values[method] = *value;
      if (--batch->remaining == 0) ready(batch->values);
    });
}
}
