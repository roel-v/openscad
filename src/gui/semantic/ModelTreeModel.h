#pragma once
#include "core/semantic/SemanticVisibility.h"
#include <QAbstractItemModel>
namespace Semantic {
enum class TreeRole { Model, Instance, Definitions, Definition, Other, Diagnostic };
class ModelTreeModel : public QAbstractItemModel {
  Q_OBJECT
public:
  enum Roles { KindRole=Qt::UserRole+1, IdRole, ResultRole };
  explicit ModelTreeModel(QObject *parent=nullptr);
  void setScene(std::shared_ptr<const Scene>, QString resultId, QString diagnostic={});
  void setVisibility(Visibility, bool ready);
  QModelIndex index(int row,int column,const QModelIndex& parent={}) const override;
  QModelIndex parent(const QModelIndex&) const override;
  int rowCount(const QModelIndex& parent={}) const override;
  int columnCount(const QModelIndex& = {}) const override { return 2; }
  QVariant data(const QModelIndex&,int role) const override;
  QVariant headerData(int section,Qt::Orientation,int role) const override;
  Qt::ItemFlags flags(const QModelIndex&) const override;
  bool setData(const QModelIndex&,const QVariant&,int role) override;
  QModelIndex find(TreeRole,const std::string& id) const;
signals:
  void eyeChanged(QString id,bool visible);
private:
  struct Row { TreeRole kind; std::string id; QString label,detail; Row *parent=nullptr; std::vector<std::unique_ptr<Row>> children; };
  Row root{TreeRole::Model,"",{}, {}};
  QString result;
  Visibility visibility;
  bool ready=false;
  Row *row(const QModelIndex&) const;
  Row *add(Row&,TreeRole,std::string,QString,QString={});
};
}
