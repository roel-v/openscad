// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/insertion/InsertionPreviewProcess.h"
#include "core/insertion/InsertionPreviewProtocol.h"
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace Insertion {
PreviewProcess::PreviewProcess(PreviewRequest request,QObject *owner,Current valid,Capture reader,Completion callback)
  :QObject(owner),capturedRequest(std::move(request)),lifetime(owner),current(std::move(valid)),capture(std::move(reader)),completion(std::move(callback)){
  startup.setSingleShot(true);startup.setInterval(5000);connect(&startup,&QTimer::timeout,this,[this]{fail("WorkerStartup","Preview worker did not complete its handshake within five seconds");});
}
PreviewProcess::~PreviewProcess(){cancel();}
void PreviewProcess::start(){if(started || finished)return;started=true;elapsed.start();launch();}
void PreviewProcess::retire(){
  startup.stop();if(!process)return;auto orphan=process.data();process.clear();orphan->disconnect(this);
  // Delete only after exit. QProcess destruction while running can wait on the GUI thread.
  if(orphan->state()==QProcess::NotRunning)orphan->deleteLater();else orphan->kill();
}
void PreviewProcess::cancel(){if(finished)return;finished=true;completion={};retire();}
void PreviewProcess::launch(){
  if(finished)return;if(!lifetime || !current || !current()){fail("Stale","The captured model or source has changed");return;}
  stdoutBytes.clear();stderrBytes.clear();response.reset();hello=false;
  auto worker=new QProcess;process=worker;worker->setProcessChannelMode(QProcess::SeparateChannels);worker->setWorkingDirectory(QString::fromUtf8(capturedRequest.directory));
#ifdef Q_OS_WIN
  worker->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments){arguments->flags|=CREATE_NO_WINDOW;arguments->startupInfo->dwFlags|=STARTF_USESHOWWINDOW;arguments->startupInfo->wShowWindow=SW_HIDE;});
#endif
  connect(worker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),worker,&QObject::deleteLater);
  connect(worker,&QProcess::readyReadStandardOutput,this,&PreviewProcess::readOutput);
  connect(worker,&QProcess::readyReadStandardError,this,[this]{if(process){stderrBytes+=process->readAllStandardError();if(stderrBytes.size()>1024*1024)fail("WorkerOutput","Worker error output exceeded its limit");}});
  connect(worker,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status){exited(code,status==QProcess::NormalExit);});
  connect(worker,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart && process)fail("WorkerStartup",process->errorString().toStdString());});
  startup.start();worker->start(QCoreApplication::applicationFilePath(),QStringList{"--insertion-preview-worker"},QIODevice::ReadWrite);
}
void PreviewProcess::readOutput(){
  if(finished || !process)return;
  try {stdoutBytes+=process->readAllStandardOutput();if(uint64_t(stdoutBytes.size())>maxFrameBytes+4)throw LimitError("Worker output frame exceeds limit");auto message=takeFrame(stdoutBytes);if(!message)return;
    if(!hello){validateHandshake(*message,capturedRequest.build);hello=true;startup.stop();if(!lifetime || !current()){fail("Stale","Captured inputs changed while the worker started");return;}auto bytes=frame(encodeRequest(capturedRequest));if(process->write(bytes)!=bytes.size())throw ProtocolError("Worker request pipe failed");process->closeWriteChannel();}
    else {if(response)throw ProtocolError("Worker sent more than one response");response=decodeResponse(*message);validateProof(capturedRequest,*response);}
  }catch(const LimitError& error){fail("LimitExceeded",error.what());}catch(const std::exception& error){fail("WorkerProtocol",error.what());}
}
void PreviewProcess::exited(int code,bool normal){
  if(finished)return;readOutput();if(finished)return;startup.stop();
  if(!normal || code!=0 || !hello || !response || !stdoutBytes.isEmpty()){fail("WorkerCrash","Preview worker exited without a complete valid result. "+stderrBytes.toStdString());return;}
  if(!lifetime || !current()){fail("Stale","Captured inputs changed during preview evaluation");return;}
  if(response->status==PreviewStatus::NeedInput){
    try {const auto key=response->missingKey;if(key.empty() || capturedRequest.sources.count(key))throw ProtocolError("Worker requested an already captured or invalid source");if(++discoveries>maxDiscoveryRounds)throw LimitError("Source discovery exceeded 32 rounds");if(!capture)throw ProtocolError("No captured source reader is available");auto input=capture(key);if(input.fileKey!=key)throw ProtocolError("Captured dependency resolved to a different source");capturedRequest.sources.emplace(key,std::move(input));
      // Validate the expanded closure with the same decoder used by the worker.
      capturedRequest=decodeRequest(encodeRequest(capturedRequest));if(!current())throw ProtocolError("Captured inputs changed during dependency discovery");retire();launch();return;
    }catch(const LimitError& error){fail("LimitExceeded",error.what());return;}catch(const std::exception& error){fail("NeedInput",error.what());return;}
  }
  complete(std::move(*response));
}
void PreviewProcess::fail(const std::string& code,const std::string& message){PreviewResponse result;result.serial=capturedRequest.serial;result.build=capturedRequest.build;result.candidateHash=capturedRequest.candidateHash;result.configurationHash=capturedRequest.configurationHash;result.status=code=="LimitExceeded"?PreviewStatus::LimitExceeded:PreviewStatus::Invalid;result.diagnostics.push_back({code,message});complete(std::move(result));}
void PreviewProcess::complete(PreviewResponse result){if(finished)return;finished=true;auto callback=std::move(completion);retire();if(callback && lifetime)callback(std::move(result));}
}
