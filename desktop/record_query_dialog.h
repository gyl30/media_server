#ifndef DESKTOP_RECORD_QUERY_DIALOG_H
#define DESKTOP_RECORD_QUERY_DIALOG_H

#include <QDialog>
#include <QString>
#include <QPointer>

class QLabel;
class QObject;
class QComboBox;
class QPushButton;
class QTreeWidget;
class QDateTimeEdit;
class QNetworkReply;

namespace media_server
{

class platform_client;

class record_query_dialog final : public QDialog
{
    Q_OBJECT

   public:
    record_query_dialog(platform_client& client, QString device_id, QString channel_id, QWidget* parent = nullptr);
    ~record_query_dialog() override;

   protected:
    void done(int result) override;

   private:
    void query();
    void cancel_query();
    void set_busy(bool busy);

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
    QTreeWidget* records_;
    QLabel* result_;
};

}    // namespace media_server

#endif
