#include <QtTest>
#include <QApplication>
#include <QDBusVirtualObject>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include "FanControlTab.hpp"
#include "DashboardTab.hpp"
#include "MonitorTab.hpp"
#include "SystemMonitor.hpp"

class AuditDaemon : public QDBusVirtualObject {
  QJsonArray writes;
  QJsonObject reads;
  bool failReads=false;
public:
  QString introspect(const QString &) const override {
    return "<interface name=\"com.uniwill.uccd\"><method name=\"GetWaterCoolerConnected\"><arg type=\"b\" direction=\"out\"/></method></interface>";
  }
  bool handleMessage(const QDBusMessage &msg,const QDBusConnection &bus) override {
    const auto method=msg.member(); QVariant result;
    if (method=="TestStats") result=QString::fromUtf8(QJsonDocument(QJsonObject{{"writes",writes},{"reads",reads}}).toJson());
    else if (method=="TestReset") { writes={};reads={};failReads=false;result=true; }
    else if (method=="TestFailReads") { failReads=msg.arguments().value(0).toBool();result=true; }
    else if (method.startsWith("Set") || method.startsWith("TurnOff") || method=="EnableWaterCooler") {
      writes.append(method);result=true;
    } else if (method.startsWith("GetWaterCooler") || method=="GetMonitorDataSince") {
      reads[method]=reads.value(method).toInt()+1;
      if (method=="GetWaterCoolerConnected") result=true;
      else if (method=="GetWaterCoolerAvailable") result=false;
      else if (method=="GetWaterCoolerPumpLevel") result=3;
      else if (method=="GetWaterCoolerFanSpeed") result=100;
      else if (method=="GetMonitorDataSince") result=QByteArray();
      const bool fail=failReads;
      QTimer::singleShot(300,this,[msg,bus,result,fail] {
        bus.send(fail ? msg.createErrorReply(QDBusError::Failed,"Test read failure") : msg.createReply(result));
      });
      return true;
    } else if (method.endsWith("JSON") || (method.endsWith("Available") || method.endsWith("Thresholds")) || method=="GetFanProfileNames") {
      result=QString(method=="GetSettingsJSON" || method=="GetActiveProfileJSON" ? "{}" : "[]");
    } else if (method.startsWith("GetCurrent") || method=="GetPowerState") result=QString();
    else result=0;
    bus.send(msg.createReply(result));return true;
  }
};

class GuiAudit : public QObject {
  Q_OBJECT
  QProcess daemon;
  std::unique_ptr<ucc::UccdClient> client;
  std::unique_ptr<ucc::ProfileManager> profiles;
  QVariant control(const QString &method,QVariantList args={}) {
    auto msg=QDBusMessage::createMethodCall("com.uniwill.uccd","/com/uniwill/uccd","com.uniwill.uccd",method);
    msg.setArguments(args);return QDBusConnection::systemBus().call(msg).arguments().value(0);
  }
  QJsonObject stats() { return QJsonDocument::fromJson(control("TestStats").toString().toUtf8()).object(); }
  QComboBox *pump(ucc::FanControlTab &tab) {
    for(auto *combo:tab.findChildren<QComboBox*>()) if(combo->findText("8V")>=0) return combo;
    return nullptr;
  }
private slots:
  void initTestCase() {
    daemon.start(QCoreApplication::applicationFilePath(),{"--serve"});
    QVERIFY(daemon.waitForStarted());QVERIFY(daemon.waitForReadyRead());
    QCOMPARE(daemon.readAllStandardOutput().trimmed(),QByteArray("READY"));
    client=std::make_unique<ucc::UccdClient>();
    profiles=std::make_unique<ucc::ProfileManager>();
  }
  void init() { control("TestReset"); }
  void displayingManualControlsMustNotStopCooling() {
    ucc::FanControlTab tab(client.get(),profiles.get(),true);
    tab.setWaterCoolerEnabled(true);tab.setWaterCoolerAutoControl(false);
    QVERIFY(QMetaObject::invokeMethod(&tab,"onConnected"));
    QVERIFY2(stats().value("writes").toArray().isEmpty(),"Loading connected manual controls must not send Off or 0% commands");
    tab.setWaterCoolerEnabled(false);
    QVERIFY2(stats().value("writes").toArray().isEmpty(),"Reflecting disabled state must not send hardware commands");
  }
  void waterPollingIsAsyncAndPreservesValuesOnFailure() {
    ucc::FanControlTab tab(client.get(),profiles.get(),true);
    tab.setWaterCoolerEnabled(true);tab.setWaterCoolerAutoControl(false);tab.show();
    qint64 maxGap=0;QElapsedTimer elapsed;elapsed.start();qint64 previous=0;
    QTimer heartbeat;connect(&heartbeat,&QTimer::timeout,this,[&]{auto now=elapsed.elapsed();maxGap=std::max(maxGap,now-previous);previous=now;});heartbeat.start(10);
    QTest::qWait(2700);heartbeat.stop();
    QVERIFY2(maxGap<100,"Water status polling must not block the GUI event loop");
    auto *combo=pump(tab);QVERIFY(combo);QCOMPARE(combo->currentText(),QString("8V"));
    QVERIFY(stats().value("writes").toArray().isEmpty());
    control("TestFailReads",{true});QTest::qWait(2200);
    QVERIFY2(combo->isEnabled(),"A timeout must not be treated as a confirmed disconnection");
    QCOMPARE(combo->currentText(),QString("8V"));
    QVERIFY(stats().value("writes").toArray().isEmpty());
  }
  void dashboardBoundsOverlappingReads() {
    ucc::SystemMonitor monitor;
    ucc::DashboardTab tab(&monitor,profiles.get(),true);tab.setWaterCoolerEnabled(true);tab.show();
    QElapsedTimer elapsed;elapsed.start();tab.refreshWaterCoolerStatus();
    QVERIFY2(elapsed.elapsed()<100,"Dashboard status refresh must be asynchronous");
    for(int i=0;i<20;++i) tab.refreshWaterCoolerStatus();
    QTest::qWait(400);
    QCOMPARE(stats().value("reads").toObject().value("GetWaterCoolerConnected").toInt(),1);
  }
  void chartBoundsOverlappingReadsAndHonorsPause() {
    ucc::MonitorTab tab(client.get());tab.show();
    QElapsedTimer elapsed;elapsed.start();tab.setMonitoringActive(true);
    QVERIFY2(elapsed.elapsed()<100,"Graph fetch must be asynchronous");
    for(int i=0;i<20;++i) QVERIFY(QMetaObject::invokeMethod(&tab,"fetchData"));
    QTest::qWait(400);
    QCOMPARE(stats().value("reads").toObject().value("GetMonitorDataSince").toInt(),1);
    tab.setMonitoringActive(false);
    QVERIFY(QMetaObject::invokeMethod(&tab,"fetchData"));QTest::qWait(50);
    QCOMPARE(stats().value("reads").toObject().value("GetMonitorDataSince").toInt(),1);
  }
  void cleanupTestCase() { profiles.reset();client.reset();daemon.terminate();daemon.waitForFinished(); }
};
int main(int argc,char **argv) {
  QApplication app(argc,argv);
  if(app.arguments().contains("--serve")) {
    AuditDaemon service;auto bus=QDBusConnection::systemBus();
    if(!bus.registerVirtualObject("/com/uniwill/uccd",&service)||!bus.registerService("com.uniwill.uccd"))return 2;
    QTextStream(stdout)<<"READY\n"<<Qt::flush;return app.exec();
  }
  GuiAudit test;return QTest::qExec(&test,argc,argv);
}
#include "test_gui_audit.moc"
