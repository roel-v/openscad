#include "gui/semantic/ModelTreeModel.h"
namespace Semantic {
ModelTreeModel::ModelTreeModel(QObject *parent) : QAbstractItemModel(parent) {}
ModelTreeModel::Row *ModelTreeModel::row(const QModelIndex& index) const { return index.isValid()?static_cast<Row *>(index.internalPointer()):const_cast<Row *>(&root); }
ModelTreeModel::Row *ModelTreeModel::add(Row& parent,TreeRole kind,std::string id,QString label,QString detail) {
  auto value=std::make_unique<Row>(Row{kind,std::move(id),std::move(label),std::move(detail),&parent,{}});
  auto *pointer=value.get(); parent.children.push_back(std::move(value)); return pointer;
}
void ModelTreeModel::setScene(std::shared_ptr<const Scene> scene,QString resultId,QString diagnostic) {
  beginResetModel(); root.children.clear(); result=std::move(resultId); ready=false;
  if (!diagnostic.isEmpty() || !scene) add(root,TreeRole::Diagnostic,"",diagnostic.isEmpty()?tr("Evaluate the document to inspect its model"):diagnostic);
  else if (!scene->valid()) {
    for (const auto& entry : scene->diagnostics) add(root,TreeRole::Diagnostic,"",QString::fromStdString(entry.message));
    if (scene->diagnostics.empty()) add(root,TreeRole::Diagnostic,"",tr("Semantic identifiers are invalid"));
  } else if (!scene->marked) add(root,TreeRole::Model,"",tr("Model"),tr("Ordinary geometry"));
  else {
    auto *model=add(root,TreeRole::Model,"",tr("Model")); std::map<std::string,Row *> rows;
    // Paths sort parents before descendants, independently of source order.
    for (const auto& [path,instance] : scene->instances) {
      auto parent=rows.find(instance.parentPath); auto *owner=parent==rows.end()?model:parent->second;
      const auto& definition=scene->definitions.at(instance.definitionId);
      rows[path]=add(*owner,TreeRole::Instance,path,QString::fromStdString(instance.label),
        definition.kind==DefinitionKind::Assembly?tr("Assembly"):tr("Part"));
      if (!instance.displayed) rows[path]->detail += tr(" · excluded by root modifier");
    }
    if (!scene->residual.empty()) add(*model,TreeRole::Other,"",tr("Other geometry"),tr("No persistent semantic target"));
    auto *definitions=add(root,TreeRole::Definitions,"",tr("Definitions"));
    for (const auto& [id,definition] : scene->definitions) add(*definitions,TreeRole::Definition,id,
      QString::fromStdString(definition.label),definition.kind==DefinitionKind::Assembly?tr("Assembly defaults"):tr("Part defaults"));
  }
  endResetModel();
}
void ModelTreeModel::setVisibility(Visibility value,bool prepared) {
  visibility=std::move(value); ready=prepared;
  std::function<void(const QModelIndex&)> notify=[&](const QModelIndex& parent) {
    if(rowCount(parent)) emit dataChanged(index(0,0,parent),index(rowCount(parent)-1,1,parent),{Qt::CheckStateRole,Qt::ToolTipRole});
    for(int i=0;i<rowCount(parent);++i)notify(index(i,0,parent));
  }; notify({});
}
QModelIndex ModelTreeModel::index(int number,int column,const QModelIndex& parent) const {
  if(number<0 || column<0 || column>=2 || (parent.isValid() && parent.column()!=0))return {};
  auto *owner=row(parent); if(static_cast<size_t>(number)>=owner->children.size())return {};
  return createIndex(number,column,owner->children[number].get());
}
QModelIndex ModelTreeModel::parent(const QModelIndex& value) const {
  if(!value.isValid())return {}; auto *owner=row(value)->parent; if(!owner || owner==&root)return {};
  auto *ancestor=owner->parent; for(size_t i=0;i<ancestor->children.size();++i)if(ancestor->children[i].get()==owner)return createIndex(static_cast<int>(i),0,owner);
  return {};
}
int ModelTreeModel::rowCount(const QModelIndex& parent) const { return parent.isValid() && parent.column()!=0 ? 0 : static_cast<int>(row(parent)->children.size()); }
QVariant ModelTreeModel::data(const QModelIndex& value,int role) const {
  if(!value.isValid())return {}; const auto *entry=row(value);
  if(role==KindRole)return static_cast<int>(entry->kind);
  if(role==IdRole)return QString::fromStdString(entry->id);
  if(role==ResultRole)return result;
  if(role==Qt::DisplayRole)return value.column()==0?entry->label:entry->detail;
  if(role==Qt::CheckStateRole && value.column()==0 && (entry->kind==TreeRole::Instance || entry->kind==TreeRole::Other))
    return (entry->kind==TreeRole::Other?visibility.otherVisible && !visibility.solo:visibility.visible(entry->id))?Qt::Checked:Qt::Unchecked;
  if(role==Qt::ToolTipRole) return !ready && (entry->kind==TreeRole::Instance || entry->kind==TreeRole::Other) ? tr("Prepare semantic inspection before changing visibility") : QString::fromStdString(entry->id);
  return {};
}
QVariant ModelTreeModel::headerData(int section,Qt::Orientation orientation,int role) const { return orientation==Qt::Horizontal && role==Qt::DisplayRole ? QVariant(section==0?tr("Component"):tr("Type")) : QVariant(); }
Qt::ItemFlags ModelTreeModel::flags(const QModelIndex& value) const {
  if(!value.isValid())return Qt::NoItemFlags;
  auto flags=Qt::ItemIsEnabled|Qt::ItemIsSelectable;
  if(ready && value.column()==0 && (row(value)->kind==TreeRole::Instance || row(value)->kind==TreeRole::Other))flags|=Qt::ItemIsUserCheckable;
  return flags;
}
bool ModelTreeModel::setData(const QModelIndex& value,const QVariant& state,int role) {
  if(role!=Qt::CheckStateRole || !(flags(value)&Qt::ItemIsUserCheckable))return false;
  emit eyeChanged(QString::fromStdString(row(value)->id),state.toInt()==Qt::Checked); return true;
}
QModelIndex ModelTreeModel::find(TreeRole kind,const std::string& id) const {
  std::function<QModelIndex(const QModelIndex&)> search=[&](const QModelIndex& parent)->QModelIndex {
    for(int i=0;i<rowCount(parent);++i){auto value=index(i,0,parent);if(row(value)->kind==kind && row(value)->id==id)return value;auto child=search(value);if(child.isValid())return child;}return {};
  };return search({});
}
}
