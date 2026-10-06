// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionTypes.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
class QProcess;
namespace Insertion {
class PreviewProcess:public QObject {
  Q_OBJECT
public:
  using Completion=std::function<void(PreviewResponse)>;
  using Current=std::function<bool()>;
  using Capture=std::function<SourceInput(const std::string&)>;
  PreviewProcess(PreviewRequest,QObject *lifetime,Current,Capture,Completion);
  ~PreviewProcess() override;
  void start();
  void cancel();
  qint64 elapsedMilliseconds()const{return elapsed.elapsed();}
  bool running()const{return !finished;}
  const PreviewRequest& request()const{return capturedRequest;}
private:
  void launch();
  void readOutput();
  void exited(int,bool);
  void fail(const std::string&,const std::string&);
  void complete(PreviewResponse);
  void retire();
  PreviewRequest capturedRequest;
  QPointer<QObject> lifetime;
  QPointer<QProcess> process;
  Current current;
  Capture capture;
  Completion completion;
  QTimer startup;
  QElapsedTimer elapsed;
  QByteArray stdoutBytes,stderrBytes;
  std::optional<PreviewResponse> response;
  size_t discoveries=0;
  bool hello=false,finished=false,started=false;
};
}
