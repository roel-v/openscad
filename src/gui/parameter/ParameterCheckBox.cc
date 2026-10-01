#include "gui/parameter/ParameterCheckBox.h"

#include <QWidget>

#include "gui/parameter/ParameterVirtualWidget.h"

ParameterCheckBox::ParameterCheckBox(QWidget *parent, BoolParameter *parameter,
                                     DescriptionStyle descriptionStyle)
  : ParameterVirtualWidget(parent, parameter), parameter(parameter)
{
  setupUi(this);
  descriptionWidget->setDescription(parameter, descriptionStyle);

  connect(checkBox, &QCheckBox::clicked, this, &ParameterCheckBox::onChanged);
  ParameterCheckBox::setValue();
}

void ParameterCheckBox::onChanged()
{
  parameter->value = checkBox->isChecked();
  emit changed(true);
}

void ParameterCheckBox::setValue()
{
  checkBox->setChecked(parameter->value);
}
