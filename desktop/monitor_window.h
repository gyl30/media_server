#ifndef DESKTOP_MONITOR_WINDOW_H
#define DESKTOP_MONITOR_WINDOW_H

#include <QMainWindow>

#include "desktop/platform_client.h"

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace media_server
{

class playback_view;

class monitor_window final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit monitor_window(QWidget* parent = nullptr);

   protected:
    void closeEvent(QCloseEvent* event) override;

   private:
    void reload_devices();
    void play_item(QTreeWidgetItem* item);

   private:
    platform_client client_;
    QObject* listing_{};
    playback_view* view_;
    QTreeWidget* devices_;
    QLineEdit* address_;
    QLabel* connection_;
};

}    // namespace media_server

#endif
