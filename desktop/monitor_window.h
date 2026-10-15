#ifndef DESKTOP_MONITOR_WINDOW_H
#define DESKTOP_MONITOR_WINDOW_H

#include <array>

#include <QMainWindow>
#include <QVariantMap>

#include "desktop/platform_client.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QGridLayout;
class QPushButton;
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
    ~monitor_window() override;

   protected:
    void closeEvent(QCloseEvent* event) override;

   private:
    void reload_devices();
    void play_item(QTreeWidgetItem* item);
    void play_data(const QVariantMap& data, playback_view* view);
    void set_layout(int count);
    void select_view(playback_view* view);
    void stop_all();
    void take_snapshot();
    void query_catalog();

   private:
    platform_client client_;
    QObject* listing_{};
    std::array<playback_view*, 16> views_{};
    playback_view* selected_{};
    quint32 audio_id_{};
    int audio_period_bytes_{};
    QGridLayout* grid_;
    QComboBox* view_layout_;
    QPushButton* sound_;
    QPushButton* catalog_;
    QTreeWidget* devices_;
    QLineEdit* address_;
    QLabel* connection_;
};

}    // namespace media_server

#endif
