#pragma once
#include "gui/design/DesignEdits.h"
#include "gui/source/EditorUndoRegistry.h"
#include <QObject>
#include <QPointer>
#include <map>
#include <optional>

class EditorInterface;
class MainWindow;
class TabManager;
namespace Design {
struct Draft {
  std::string token, key, kind;
  std::shared_ptr<const Record> before, preview;
  std::vector<FieldChange> changes;
  Selection::Stamp stamp;
  bool consumer = false;
};
struct LifecycleCallbacks {
  std::function<std::shared_ptr<const Record>(const std::string&, const Selection::Stamp&)> resolveConsumerRecord;
  std::function<PreparedEdit(const Draft&, const Selection::Stamp&)> prepareInsert;
  std::function<PreparedEdit(const std::string&, const std::vector<FieldChange>&, const Selection::Stamp&)> prepareEdit;
  std::function<std::shared_ptr<const Record>(const Draft&, const Selection::Stamp&)> locatePromotedRecord;
  std::function<std::shared_ptr<const Record>(const Record&, const std::vector<FieldChange>&, std::string&)> previewConsumerRecord;
};
struct LifecycleResult { bool changed = false; std::string error; explicit operator bool() const { return error.empty(); } };
struct SaveOutcome { QPointer<EditorInterface> editor; bool saved = false; QString detail; };
class DesignDocument : public QObject {
  Q_OBJECT
public:
  class Token {
  public:
    Token() = default;
    ~Token() { reset(); }
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    Token(Token&&) noexcept;
    Token& operator=(Token&&) noexcept;
    explicit operator bool() const { return document; }
    void reset();
  private:
    friend class DesignDocument;
    Token(DesignDocument *document, std::string kind) : document(document), kind(std::move(kind)) {}
    QPointer<DesignDocument> document;
    std::string kind;
  };
  DesignDocument(EditorInterface *owner, MainWindow *window);
  static DesignDocument *forEditor(EditorInterface&, MainWindow&);
  Token registerLifecycleAdapter(std::string kind, QObject *lifetime, LifecycleCallbacks);
  std::shared_ptr<const Record> consumerRecord(const std::string& kind, const std::string& opaqueKey, const Selection::Stamp&) const;
  std::optional<std::string> createTransient(const std::string& kind, Value definition, std::string *error = nullptr);
  std::optional<std::string> beginEdit(const std::string& id, const Selection::Stamp&);
  std::optional<std::string> beginConsumerEdit(const std::string& kind, const std::string& opaqueKey, const Selection::Stamp&);
  std::optional<std::string> createConsumerTransient(const std::string& kind, const std::string& opaqueKey,
      Value definition, const Selection::Stamp&, std::string *error = nullptr);
  LifecycleResult updateDraft(const std::string& token, const std::vector<FieldChange>&);
  void cancelDraft(const std::string& token);
  LifecycleResult commitDraft(const std::string& token);
  LifecycleResult promote(const std::vector<std::string>& ids, EditorInterface& destination, const Selection::Stamp&);
  LifecycleResult promoteConsumer(const std::string& token);
  LifecycleResult promotePersonal(const std::string& id, EditorInterface& destination, const Selection::Stamp&);
  LifecycleResult remove(const std::string& id, const Selection::Stamp&);
  Records records(const Selection::Stamp&) const;
  const Records& transientRecords() const { return transient; }
  const std::map<std::string, Draft>& drafts() const { return editing; }
  static std::vector<SaveOutcome> saveSequentially(TabManager&, const std::vector<EditorInterface *>&,
                                                 const std::function<bool(EditorInterface&)>& save = {});
signals:
  void changed();
private:
  struct Adapter { QPointer<QObject> owner; LifecycleCallbacks callbacks; QMetaObject::Connection lifetime; };
  QPointer<EditorInterface> owner;
  QPointer<MainWindow> window;
  DesignEdits edits;
  Records transient;
  std::map<std::string, Draft> editing;
  std::map<std::string, Adapter> adapters;
  std::map<std::string, Draft> pendingConsumers;
  std::set<std::string> restoredConsumers;
  void unregisterAdapter(const std::string& kind);
  std::shared_ptr<const Record> updated(const Record&, const std::vector<FieldChange>&, std::string&) const;
  bool valid(const Selection::Stamp&) const;
  void synchronize();
  void clearTemporaryDrafts();
  LifecycleResult changeTransient(const std::string& id, std::shared_ptr<const Record> replacement);
};
}
