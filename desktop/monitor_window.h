#ifndef DESKTOP_MONITOR_WINDOW_H
#define DESKTOP_MONITOR_WINDOW_H

#include <array>

#include <QPointer>
#include <QMainWindow>
#include <QVariantMap>

#include "desktop/platform_client.h"

class QLabel;
class QSpinBox;
class QLineEdit;
class QComboBox;
class QGridLayout;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;

namespace media_server
{

class playback_view;
class record_query_dialog;

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
    void update_record_pause();
    void stop_all();
    void take_snapshot();
    void query_catalog();
    void open_record_query();
    void reset_device_query();
    void set_control_busy(bool busy);
    void query_device(const QString& command);
    void query_presets();
    void control_ptz(const QString& command);
    int selected_preset_id() const;
    void control_preset(const QString& command);

   private:
    platform_client client_;
    QObject* listing_{};
    QObject* control_context_{};
    QPointer<record_query_dialog> record_dialog_;
    std::array<playback_view*, 16> views_{};
    playback_view* selected_{};
    quint32 audio_id_{};
    int audio_period_bytes_{};
    QGridLayout* grid_;
    QComboBox* view_layout_;
    QPushButton* sound_;
    QPushButton* record_pause_;
    QPushButton* catalog_;
    QPushButton* record_query_;
    QPushButton* device_info_;
    QPushButton* device_status_;
    QLabel* device_target_;
    QPlainTextEdit* device_result_;
    QPushButton* preset_query_;
    QTreeWidget* preset_list_;
    QLabel* preset_result_;
    QSpinBox* preset_number_;
    QPushButton* preset_set_;
    QPushButton* preset_goto_;
    QPushButton* preset_remove_;
    std::array<QPushButton*, 10> ptz_actions_{};
    QPushButton* ptz_stop_;
    QSpinBox* ptz_speed_;
    QSpinBox* ptz_zoom_speed_;
    QLabel* ptz_result_;
    QTreeWidget* devices_;
    QLineEdit* address_;
    QLabel* connection_;
};

}    // namespace media_server

#endif
