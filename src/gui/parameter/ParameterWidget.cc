#include <algorithm>
/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2014 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include "gui/parameter/ParameterWidget.h"
#include "gui/evaluation/EvaluationService.h"

#include <QAction>
#include <QJsonDocument>
#include <QJsonArray>
#include <QInputDialog>
#include <QLayoutItem>
#include <QMenu>
#include <QMessageBox>
#include <QString>
#include <QToolButton>
#include <QWidget>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/Context.h"
#include "core/BuiltinContext.h"
#include "core/EvaluationSession.h"
#include "core/ScopeContext.h"

#include "core/customizer/ParameterObject.h"
#include "platform/PlatformUtils.h"
#include "gui/Preferences.h"
#include "gui/parameter/GroupWidget.h"
#include "gui/parameter/ParameterCheckBox.h"
#include "gui/parameter/ParameterComboBox.h"
#include "gui/parameter/ParameterSlider.h"
#include "gui/parameter/ParameterSpinBox.h"
#include "gui/parameter/ParameterText.h"
#include "gui/parameter/ParameterVector.h"
#include "gui/parameter/ParameterVirtualWidget.h"

QJsonObject ParameterWidget::annotationValues() const
{
  QJsonObject values;
  for (const auto& parameter : parameters) {
    const auto json = "[" + parameter->jsonValue().dump() + "]";
    values[QString::fromStdString(parameter->name())] = QJsonDocument::fromJson(QByteArray::fromStdString(json)).array().at(0);
  }
  return values;
}

ParameterWidget::ParameterWidget(QWidget *parent) : QWidget(parent)
{
  setupUi(this);
  scrollAreaWidgetContents->layout()->setAlignment(Qt::AlignTop);

  autoPreviewTimer.setInterval(1000);
  autoPreviewTimer.setSingleShot(true);

  connect(&autoPreviewTimer, &QTimer::timeout, this, &ParameterWidget::emitParametersChanged);
  // connect(comboBoxPreset, &QComboBox::editTextChanged, this, &ParameterWidget::onSetNameChanged);

  auto *customizer_menu = new QMenu(this);
  parameterMenuButton->setMenu(customizer_menu);

  QAction *collapseAction = customizer_menu->addAction(_("Collapse All"));
  connect(collapseAction, &QAction::triggered, this, &ParameterWidget::onCollapseAll);

  QAction *expandAction = customizer_menu->addAction(_("Expand All"));
  connect(expandAction, &QAction::triggered, this, &ParameterWidget::onExpandAll);

  QString fontfamily = GlobalPreferences::inst()->getValue("advanced/customizerFontFamily").toString();
  uint fontsize = GlobalPreferences::inst()->getValue("advanced/customizerFontSize").toUInt();
  setFontFamilySize(fontfamily, fontsize);

  connect(GlobalPreferences::inst(), &Preferences::customizerFontChanged, this,
          &ParameterWidget::setFontFamilySize);
}

ParameterWidget::~ParameterWidget()
{
  // QWidget children can still dereference their ParameterObject in destruction.
  auto *layout = scrollAreaWidgetContents->layout();
  while (layout && layout->count()) {
    auto *child = layout->takeAt(0);
    delete child->widget(); delete child;
  }
  auto retained = std::make_shared<std::pair<std::shared_ptr<const void>, ParameterObjects>>(
    std::move(evaluationBacking), std::move(parameters));
  BackgroundEvaluation::Service::instance().retire([retained] {});
}

// Load presets before the first asynchronous metadata result is installed.
void ParameterWidget::readFile(const QString& scadFile)
{
  assert(sets.empty());
  assert(parameters.empty());
  assert(widgets.empty());
  QString jsonFile = getJsonFile(scadFile);
  if (!std::filesystem::exists(jsonFile.toStdString()) || sets.readFile(jsonFile.toStdString()))
    invalidJsonFile.clear();
  else invalidJsonFile = jsonFile;
  for (const auto& set : sets) comboBoxPreset->addItem(QString::fromStdString(set.name()));
}

// Write the json file if the parameter sets are not empty.
// This prevents creating unnecessary json files.
void ParameterWidget::saveFile(const QString& scadFile)
{
  if (sets.empty()) {
    return;
  }

  QString jsonFile = getJsonFile(scadFile);
  if (jsonFile == this->invalidJsonFile) {
    QMessageBox msgBox;
    msgBox.setWindowTitle(_("Saving presets"));
    msgBox.setText(QString(_("%1 was found, but was unreadable. Do you want to overwrite %1?"))
                     .arg(this->invalidJsonFile));
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    if (msgBox.exec() == QMessageBox::Cancel) {
      return;
    }
  }

  cleanSets();
  sets.writeFile(jsonFile.toStdString());
}

void ParameterWidget::saveBackupFile(const QString& scadFile)
{
  if (sets.empty()) {
    return;
  }

  sets.writeFile(getJsonFile(scadFile).toStdString());
}



// updates every parameter
void ParameterWidget::updateParameterStates()
{
  ++parameterRevision;
  emit metadataRequested();
}

ParameterSet ParameterWidget::requestedValues() const
{
  const auto index = comboBoxPreset->currentIndex();
  if (index > 0 && size_t(index) <= sets.size()) return sets[index - 1];
  return {};
}

void ParameterWidget::installEvaluation(ParameterObjects values, std::string evaluatedSource,
                                        std::shared_ptr<const void> backing)
{
  // Attribute refreshes must preserve live controls (focus, slider drags and
  // group expansion). The accepted revision already guarantees matching values.
  bool same = source == evaluatedSource && parameters.size() == values.size();
  auto old = parameters.begin();
  for (const auto& value : values) {
    if (!same) break;
    same = (*old)->sameDefinition(*value);
    ++old;
  }
  if (same) {
    old = parameters.begin();
    for (const auto& value : values) {
      const bool changed = (*old)->jsonValue() != value->jsonValue();
      if (changed) (*old)->importValue(value->exportValue(), true);
      (*old)->setHidden(value->isHidden());
      (*old)->setLocked(value->isLocked());
      for (auto widget : widgets[old->get()]) {
        if (changed) widget->setValue();
        widget->setHidden(value->isHidden());
        widget->setEnabled(!value->isLocked());
      }
      ++old;
    }
    updateGroupVisibility();
    return;
  }
  // Retain old parameter ownership until rebuildWidgets deletes their controls.
  auto oldBacking = std::move(evaluationBacking);
  auto oldParameters = std::move(parameters);
  parameters = std::move(values);
  evaluationBacking = std::move(backing);
  source = std::move(evaluatedSource);
  rebuildWidgets();
  updateSetEditability();
  setEnabled(true);
  auto retained = std::make_shared<std::pair<std::shared_ptr<const void>, ParameterObjects>>(
    std::move(oldBacking), std::move(oldParameters));
  BackgroundEvaluation::Service::instance().retire([retained] {});
}

void ParameterWidget::updateGroupVisibility()
{
  for (auto *group : this->findChildren<GroupWidget *>()) group->updateVisibility();
}



bool ParameterWidget::childHasFocus()
{
  if (this->hasFocus()) {
    return true;
  }
  auto children = this->findChildren<QWidget *>();
  for (auto child : children) {
    if (child->hasFocus()) {
      return true;
    }
  }
  return false;
}

void ParameterWidget::setModified(bool modified)
{
  if (this->modified != modified) {
    this->modified = modified;
    emit modificationChanged();
  }
}

void ParameterWidget::emitParametersChanged()
{
  for (const auto& kvp : widgets) {
    for (ParameterVirtualWidget *widget : kvp.second) {
      widget->valueApplied();
    }
  }
  emit parametersChanged();
}

void ParameterWidget::autoPreview(bool immediate)
{
  autoPreviewTimer.stop();
  if (checkBoxAutoPreview->isChecked()) {
    if (immediate) {
      emitParametersChanged();
    } else {
      autoPreviewTimer.start();
    }
  }
}

void ParameterWidget::on_checkBoxAutoPreview_toggled(bool)
{
  autoPreview(true);
}

void ParameterWidget::on_comboBoxDetails_currentIndexChanged(int)
{
  rebuildWidgets();
}

void ParameterWidget::on_comboBoxPreset_activated(int index)
{
  loadSet(index);
  autoPreview(true);
}

void ParameterWidget::onSetNameChanged()
{
  assert(static_cast<size_t>(comboBoxPreset->count()) == sets.size() + 1);
  comboBoxPreset->setItemText(comboBoxPreset->currentIndex(), comboBoxPreset->lineEdit()->text());
  sets[comboBoxPreset->currentIndex() - 1].setName(comboBoxPreset->currentText().toStdString());
  setModified();
}

void ParameterWidget::on_addButton_clicked()
{
  bool ok = true;
  QString result =
    QInputDialog::getText(this, _("Create new set of parameter"), _("Enter name of the parameter set"),
                          QLineEdit::Normal, "", &ok);

  if (ok) {
    createSet(result.trimmed());
  }
  setModified();
}

void ParameterWidget::on_deleteButton_clicked()
{
  int index = comboBoxPreset->currentIndex();
  assert(index > 0);
  int newIndex;
  if (index + 1 == comboBoxPreset->count()) {
    newIndex = index - 1;
  } else {
    newIndex = index + 1;
  }
  comboBoxPreset->setCurrentIndex(newIndex);
  loadSet(newIndex);

  comboBoxPreset->removeItem(index);
  sets.erase(sets.begin() + (index - 1));
  setModified();
  autoPreview(true);
}

void ParameterWidget::onCollapseAll()
{
  for (GroupWidget *groupWidget : this->findChildren<GroupWidget *>()) {
    groupWidget->setExpanded(false);
  }
}

void ParameterWidget::onExpandAll()
{
  for (GroupWidget *groupWidget : this->findChildren<GroupWidget *>()) {
    groupWidget->setExpanded(true);
  }
}

void ParameterWidget::parameterModified(bool immediate)
{
  auto *widget = (ParameterVirtualWidget *)sender();
  if (!widget) return;
  ParameterObject *parameter = widget->getParameter();

  ++parameterRevision;
  // When attempting to modify the design default, create a new set to edit.
  if (comboBoxPreset->currentIndex() == 0) {
    std::set<std::string> setNames;
    for (const auto& set : this->sets) {
      setNames.insert(set.name());
    }

    QString name;
    for (int i = 1;; i++) {
      name = _("New set ") + QString::number(i);
      if (setNames.count(name.toStdString()) == 0) {
        break;
      }
    }
    createSet(name);
  }

  size_t setIndex = comboBoxPreset->currentIndex() - 1;
  assert(setIndex < sets.size());
  sets[setIndex][parameter->name()] = parameter->exportValue();

  assert(widgets.count(parameter) == 1);
  for (ParameterVirtualWidget *otherWidget : widgets[parameter]) {
    if (otherWidget != widget) {
      otherWidget->setValue();
    }
  }

  setModified();
  emit metadataRequested();
  autoPreview(immediate);
}

void ParameterWidget::loadSet(size_t index)
{
  assert(index <= sets.size());
  if (index == 0) {
    parameters.reset();
  } else {
    parameters.importValues(sets[index - 1]);
  }

  updateParameterStates();
  updateSetEditability();

  for (const auto& pair : widgets) {
    for (ParameterVirtualWidget *widget : pair.second) {
      widget->setValue();
    }
  }
}

void ParameterWidget::createSet(const QString& name)
{
  sets.push_back(parameters.exportValues(name.toStdString()));
  comboBoxPreset->addItem(name);
  comboBoxPreset->setCurrentIndex(comboBoxPreset->count() - 1);
  updateSetEditability();
}

void ParameterWidget::updateSetEditability()
{
  if (comboBoxPreset->currentIndex() == 0) {
    comboBoxPreset->setEditable(false);
    deleteButton->setEnabled(false);
  } else {
    if (!comboBoxPreset->isEditable()) {
      comboBoxPreset->setEditable(true);
      connect(comboBoxPreset->lineEdit(), &QLineEdit::textEdited, this,
              &ParameterWidget::onSetNameChanged);
    }
    deleteButton->setEnabled(true);
  }
}

void ParameterWidget::rebuildWidgets()
{
  std::map<QString, bool> expandedGroups;
  for (GroupWidget *groupWidget : this->findChildren<GroupWidget *>()) {
    expandedGroups.emplace(groupWidget->title(), groupWidget->isExpanded());
  }

  widgets.clear();
  QLayout *layout = this->scrollAreaWidgetContents->layout();
  while (layout->count() > 0) {
    QLayoutItem *child = layout->takeAt(0);
    delete child->widget();
    delete child;
  }

  auto descriptionStyle = static_cast<DescriptionStyle>(comboBoxDetails->currentIndex());
  std::vector<ParameterGroup> parameterGroups = getParameterGroups();
  for (const auto& group : parameterGroups) {
    auto *groupWidget = new GroupWidget(group.name);
    for (ParameterObject *parameter : group.parameters) {
      ParameterVirtualWidget *parameterWidget = createParameterWidget(parameter, descriptionStyle);
      connect(parameterWidget, &ParameterVirtualWidget::changed, this,
              &ParameterWidget::parameterModified);
      if (!widgets.count(parameter)) {
        widgets[parameter] = {};
      }
      widgets[parameter].push_back(parameterWidget);
      groupWidget->addWidget(parameterWidget);
      parameterWidget->setHidden(parameter->isHidden());
      parameterWidget->setEnabled(!parameter->isLocked());
    }
    auto it = expandedGroups.find(group.name);
    groupWidget->setExpanded(it == expandedGroups.end() || it->second);
    layout->addWidget(groupWidget);
  }

  updateGroupVisibility();
}

std::vector<ParameterWidget::ParameterGroup> ParameterWidget::getParameterGroups()
{
  std::vector<ParameterWidget::ParameterGroup> output;
  std::map<std::string, size_t> groupIndices;
  std::vector<ParameterObject *> globalParameters;

  for (const std::unique_ptr<ParameterObject>& parameter : parameters) {
    std::string group = parameter->group();
    if (group == "Global") {
      globalParameters.push_back(parameter.get());
    } else if (group == "Hidden") {
      continue;
    } else {
      if (!groupIndices.count(group)) {
        groupIndices[group] = output.size();
        output.push_back({QString::fromStdString(group)});
      }
      output[groupIndices[group]].parameters.push_back(parameter.get());
    }
  }

  if (output.size() == 0 && globalParameters.size() > 0) {
    ParameterGroup global;
    global.name = "Global";
    global.parameters = std::move(globalParameters);
    output.push_back(std::move(global));
  } else {
    for (auto& group : output) {
      group.parameters.insert(group.parameters.end(), globalParameters.begin(), globalParameters.end());
    }
  }

  return output;
}

ParameterVirtualWidget *ParameterWidget::createParameterWidget(ParameterObject *parameter,
                                                               DescriptionStyle descriptionStyle)
{
  ParameterVirtualWidget *widget = nullptr;
  if (parameter->type() == ParameterObject::ParameterType::Bool) {
    widget = new ParameterCheckBox(this, static_cast<BoolParameter *>(parameter), descriptionStyle);
  } else if (parameter->type() == ParameterObject::ParameterType::String) {
    widget = new ParameterText(this, static_cast<StringParameter *>(parameter), descriptionStyle);
  } else if (parameter->type() == ParameterObject::ParameterType::Number) {
    auto *numberParameter = static_cast<NumberParameter *>(parameter);
    if (numberParameter->minimum && numberParameter->maximum && numberParameter->isSliderEnabled()) {
      widget = new ParameterSlider(this, numberParameter, descriptionStyle);
    } else {
      widget = new ParameterSpinBox(this, numberParameter, descriptionStyle);
    }
  } else if (parameter->type() == ParameterObject::ParameterType::Vector) {
    widget = new ParameterVector(this, static_cast<VectorParameter *>(parameter), descriptionStyle);
  } else if (parameter->type() == ParameterObject::ParameterType::Enum) {
    widget = new ParameterComboBox(this, static_cast<EnumParameter *>(parameter), descriptionStyle);
  } else {
    assert(false);
    throw std::runtime_error("Unsupported parameter widget type");
  }
  if (widget) {
    widget->setEnabled(!parameter->isLocked());
    widget->setHidden(parameter->isHidden());
  }
  return widget;
}

QString ParameterWidget::getJsonFile(const QString& scadFile)
{
  std::filesystem::path p = scadFile.toStdString();
  return QString::fromStdString(p.replace_extension(".json").string());
}

// Remove set values that do not correspond to a parameter,
// or that cannot be parsed as such.
void ParameterWidget::cleanSets()
{
  std::map<std::string, ParameterObject *> namedParameters;
  for (const auto& parameter : parameters) {
    namedParameters[parameter->name()] = parameter.get();
  }

  for (ParameterSet& set : sets) {
    for (auto it = set.begin(); it != set.end();) {
      if (!namedParameters.count(it->first)) {
        it = set.erase(it);
      } else {
        if (namedParameters[it->first]->importValue(it->second, false)) {
          ++it;
        } else {
          it = set.erase(it);
        }
      }
    }
  }
}

void ParameterWidget::setFontFamilySize(const QString& fontFamily, uint fontSize)
{
  scrollArea->setStyleSheet(
    QString("font-family: \"%1\"; font-size: %2pt;").arg(fontFamily).arg(fontSize));
}
ParameterSet ParameterWidget::evaluationValues() const
{
  ParameterSet values;
  for (const auto& parameter : parameters) values.emplace(parameter->name(), parameter->exportValue());
  return values;
}
