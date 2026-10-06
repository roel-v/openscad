#include "gui/WindowManager.h"

#include <QSet>

#include "gui/MainWindow.h"

void WindowManager::add(MainWindow *mainwin)
{
  this->windows.insert(mainwin);
  emit windowsChanged();
}

void WindowManager::remove(MainWindow *mainwin)
{
  this->windows.remove(mainwin);
  emit windowsChanged();
}

const QSet<MainWindow *>& WindowManager::getWindows() const
{
  return this->windows;
}
