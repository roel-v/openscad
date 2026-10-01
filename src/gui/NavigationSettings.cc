#include "gui/NavigationSettings.h"
#include "gui/QSettingsCached.h"
#include <QMetaProperty>
#include <QCheckBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QGroupBox>
#include <QSignalBlocker>
#include <algorithm>

namespace {
QList<NavigationSettings *> instances;
const QList<QByteArray> persistentKeys{"visible","animate","duration","showInformation","triangles","fps","frameTime"};
}

NavigationSettings::NavigationSettings(QObject *parent) : QObject(parent)
{
  for (const auto& name : persistentKeys) {
    auto value=persistent(name,property(name));
    if (name=="duration") value=std::clamp(value.toInt(),0,5000);
    setProperty(name,value);
  }
  baselines=values();
  instances.append(this);
}
NavigationSettings::~NavigationSettings() { instances.removeAll(this); }

QVariant NavigationSettings::persistent(const QByteArray& name,const QVariant& fallback)
{
  return QSettingsCached().value("navigation/"+QString::fromLatin1(name),fallback);
}

void NavigationSettings::setPersistent(const QByteArray& name,const QVariant& value)
{
  if (!persistentKeys.contains(name)) return;
  QSettingsCached().setValue("navigation/"+QString::fromLatin1(name),value);
  for (auto instance : instances) {
    instance->baselines[QString::fromLatin1(name)]=value;
    instance->overrides.remove(QString::fromLatin1(name));
    instance->setProperty(name,value);
  }
}

void NavigationSettings::setTemporary(const QByteArray& name,const QVariant& value)
{
  if (metaObject()->indexOfProperty(name)<metaObject()->propertyOffset()) return;
  overrides[QString::fromLatin1(name)]=value;
  setProperty(name,value);
}

void NavigationSettings::resetOverrides()
{
  overrides.clear();
  for (auto it=baselines.cbegin();it!=baselines.cend();++it) setProperty(it.key().toLatin1(),it.value());
}

QVariantMap NavigationSettings::values() const
{
  QVariantMap result;
  for (int i=metaObject()->propertyOffset();i<metaObject()->propertyCount();++i) {
    const auto p=metaObject()->property(i);
    result[QString::fromLatin1(p.name())]=p.read(this);
  }
  return result;
}

QString NavigationSettings::unit(const QByteArray& name)
{
  if (name.endsWith("Delay") || name.startsWith("fade") || name=="duration" || name=="statsIdle" || name=="statsRefresh") return "ms";
  if (name.endsWith("Step")) return "degrees";
  if (name.endsWith("Right") || name.endsWith("Top") || name.endsWith("Size") || name.endsWith("Radius") || name=="dragThreshold") return "logical px";
  if (name=="statsWindow") return "frames";
  return "";
}

QString NavigationSettings::description() const
{
  QString result="Navigation settings v1\n";
  const auto effective=values();
  for (auto it=effective.cbegin();it!=effective.cend();++it) {
    result+=QString("%1 = %2 %3; baseline = %4; override = %5\n")
      .arg(it.key(),it.value().toString(),unit(it.key().toLatin1()),baselines.value(it.key()).toString(),
           overrides.contains(it.key()) ? "yes" : "no");
  }
  return result;
}

QWidget *NavigationSettings::preferenceControls(QWidget *parent)
{
  auto group=new QGroupBox(tr("Viewport navigation"),parent);
  auto form=new QFormLayout(group);
  auto visibleBox=new QCheckBox(tr("Show navigation cube"),group);
  auto animateBox=new QCheckBox(tr("Animate camera changes"),group);
  auto durationBox=new QSpinBox(group);
  durationBox->setRange(0,5000);
  durationBox->setSuffix(tr(" ms"));
  visibleBox->setChecked(persistent("visible",true).toBool());
  animateBox->setChecked(persistent("animate",true).toBool());
  durationBox->setValue(persistent("duration",200).toInt());
  form->addRow(visibleBox);
  form->addRow(animateBox);
  form->addRow(tr("Transition duration"),durationBox);
  connect(visibleBox,&QCheckBox::toggled,group,[](bool b){setPersistent("visible",b);});
  connect(animateBox,&QCheckBox::toggled,group,[](bool b){setPersistent("animate",b);});
  connect(durationBox,qOverload<int>(&QSpinBox::valueChanged),group,[](int v){setPersistent("duration",v);});
  return group;
}
