#ifndef DESKTOP_RECORD_QUERY_DIALOG_H
#define DESKTOP_RECORD_QUERY_DIALOG_H

#include <QDialog>
#include <QString>
#include <QPointer>

class QLabel;
class QTimer;
class QObject;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QTreeWidget;
class QDateTimeEdit;
class QNetworkReply;

namespace media_server
{

class record_thread;
class platform_client;

inline constexpr char record_directory_key[] = "record_directory";

QString record_directory();

class record_query_dialog final : public QDialog
{
    Q_OBJECT

   public:
    record_query_dialog(platform_client& client, QString device_id, QString channel_id, QWidget* parent = nullptr);
    ~record_query_dialog() override;

   signals:
    void playback_requested(qint64 start_time, qint64 end_time);

   protected:
    void done(int result) override;

   private:
    void query();
    void cancel_query();
    void set_busy(bool busy);
    bool selected_range(qint64& start_time, qint64& end_time, QString& stem);
    void start_download();
    void cancel_download();

   private:
    platform_client& client_;
    QString device_id_;
    QString channel_id_;
    QObject* context_{};
    QPointer<QNetworkReply> reply_;
    QDateTimeEdit* start_;
    QDateTimeEdit* end_;
    QComboBox* type_;
    QPushButton* query_;
    QPushButton* cancel_;
    QSpinBox* utc_offset_;
    QPushButton* play_;
    QTreeWidget* records_;
    QLabel* result_;
    QLineEdit* directory_;
    QComboBox* download_speed_;
    QPushButton* download_;
    QPushButton* download_cancel_;
    QLabel* download_result_;
    QTimer* download_progress_;
    QObject* download_session_{};
    QPointer<record_thread> downloader_;
};

}    // namespace media_server

#endif
