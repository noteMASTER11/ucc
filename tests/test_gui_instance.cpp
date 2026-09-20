#include <QtTest>
#include <QApplication>
#include <QProcess>
#include <QTimer>
#include "GuiInstance.hpp"

class InstanceTest : public QObject {
  Q_OBJECT
  QProcess primary;
  void launch(QProcess &process) {
    process.start(QCoreApplication::applicationFilePath(), {"--instance"});
  }
private slots:
  void initTestCase() {
    launch(primary);
    QVERIFY(primary.waitForStarted());
    QVERIFY(primary.waitForReadyRead());
    QVERIFY(primary.readAllStandardOutput().contains("READY"));
  }
  void repeatedLaunchRestoresTheExistingWindow() {
    QProcess second;
    launch(second);
    QVERIFY(second.waitForStarted());
    const bool exited = second.waitForFinished(4000);
    if (!exited) { second.terminate(); second.waitForFinished(); }
    QVERIFY2(exited, "A repeated launch must exit, not open another GUI instance");
    QCOMPARE(second.exitCode(), 0);
    QVERIFY(second.readAllStandardOutput().contains("FORWARDED"));
    QVERIFY(primary.waitForReadyRead());
    QVERIFY2(primary.readAllStandardOutput().contains("RESTORED"),
             "The existing minimized window must become visible");
  }
  void concurrentLaunchesDoNotCreateMoreWindows() {
    QProcess a, b, c;
    for (auto *p : {&a,&b,&c}) launch(*p);
    for (auto *p : {&a,&b,&c}) {
      const bool exited = p->waitForFinished(4000);
      if (!exited) { p->terminate(); p->waitForFinished(); }
      QVERIFY2(exited, "Concurrent launch must use the existing owner");
      QCOMPARE(p->exitCode(), 0);
      QVERIFY(p->readAllStandardOutput().contains("FORWARDED"));
    }
  }
  void ownerExitAllowsANewWindow() {
    primary.terminate(); QVERIFY(primary.waitForFinished());
    launch(primary);
    QVERIFY(primary.waitForStarted());
    QVERIFY(primary.waitForReadyRead());
    QVERIFY(primary.readAllStandardOutput().contains("READY"));
  }
  void cleanupTestCase() { primary.terminate(); primary.waitForFinished(); }
};

int main(int argc,char **argv) {
  QApplication app(argc,argv);
  if (app.arguments().contains("--instance")) {
    ucc::GuiInstance instance;
    const auto result=instance.startOrActivate();
    if (result==ucc::GuiInstance::Result::ActivatedExisting) {
      QTextStream(stdout)<<"FORWARDED\n"<<Qt::flush; return 0;
    }
    if (result!=ucc::GuiInstance::Result::Primary) return 2;
    QWidget window;
    instance.setWindow(&window);
    window.showMinimized();
    QTextStream(stdout)<<"READY\n"<<Qt::flush;
    QTimer observed;
    QObject::connect(&observed,&QTimer::timeout,&window,[&] {
      if (window.isVisible() && !window.isMinimized()) {
        QTextStream(stdout)<<"RESTORED\n"<<Qt::flush;
        observed.stop();
      }
    });
    observed.start(10);
    return app.exec();
  }
  InstanceTest test;
  return QTest::qExec(&test,argc,argv);
}
#include "test_gui_instance.moc"
