#include <utility>

#include <QUrl>
#include <QDate>
#include <QTime>
#include <QDir>
#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTreeWidget>
#include <QJsonObject>
#include <QStringList>
#include <QDateTimeEdit>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QCoreApplication>

#include "desktop/record_thread.h"
#include "desktop/history_request.h"
#include "desktop/platform_client.h"
#include "desktop/record_query_dialog.h"

namespace media_server
{

QString record_directory()
{
    return QSettings().value(QString::fromLatin1(record_directory_key), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
}

record_query_dialog::record_query_dialog(platform_client& client, QString device_id, QString channel_id, QWidget* parent)
    : QDialog(parent),
      client_(client),
      device_id_(std::move(device_id)),
      channel_id_(std::move(channel_id)),
      start_(new QDateTimeEdit(this)),
      end_(new QDateTimeEdit(this)),
      type_(new QComboBox(this)),
      query_(new QPushButton(QStringLiteral("查询"), this)),
      cancel_(new QPushButton(QStringLiteral("取消查询"), this)),
      utc_offset_(new QSpinBox(this)),
      play_(new QPushButton(QStringLiteral("在选中画面回放"), this)),
      records_(new QTreeWidget(this)),
      result_(new QLabel(this)),
      directory_(new QLineEdit(this)),
      download_speed_(new QComboBox(this)),
      download_(new QPushButton(QStringLiteral("下载选中录像"), this)),
      download_cancel_(new QPushButton(QStringLiteral("取消下载"), this)),
      download_result_(new QLabel(this)),
      download_progress_(new QTimer(this))
{
    setObjectName(QStringLiteral("record_query_dialog"));
    setWindowTitle(QStringLiteral("录像查询 · %1").arg(channel_id_));
    setWindowModality(Qt::NonModal);
    resize(1000, 480);
    auto* layout = new QVBoxLayout(this);
    auto* target =
        new QLabel(QStringLiteral("父设备：%1\n通道：%2\n查询时间按设备当地墙上时间填写；回放前请确认下方设备 UTC 偏移。目录不代表录像可回放。")
                       .arg(device_id_, channel_id_),
                   this);
    target->setObjectName(QStringLiteral("record_target"));
    target->setTextFormat(Qt::PlainText);
    target->setWordWrap(true);
    layout->addWidget(target);
    auto* filters = new QHBoxLayout;
    const auto now = QDateTime::currentDateTime();
    start_->setObjectName(QStringLiteral("record_start"));
    end_->setObjectName(QStringLiteral("record_end"));
    for (auto* input : {start_, end_})
    {
        input->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        input->setCalendarPopup(true);
    }
    start_->setDateTime(now.addSecs(-3600));
    end_->setDateTime(now);
    type_->setObjectName(QStringLiteral("record_type"));
    type_->addItem(QStringLiteral("全部"), QStringLiteral("all"));
    type_->addItem(QStringLiteral("定时"), QStringLiteral("time"));
    type_->addItem(QStringLiteral("报警"), QStringLiteral("alarm"));
    type_->addItem(QStringLiteral("手动"), QStringLiteral("manual"));
    query_->setObjectName(QStringLiteral("record_query"));
    cancel_->setObjectName(QStringLiteral("record_cancel"));
    filters->addWidget(new QLabel(QStringLiteral("开始"), this));
    filters->addWidget(start_);
    filters->addWidget(new QLabel(QStringLiteral("结束"), this));
    filters->addWidget(end_);
    filters->addWidget(type_);
    filters->addWidget(query_);
    filters->addWidget(cancel_);
    layout->addLayout(filters);
    records_->setObjectName(QStringLiteral("record_list"));
    records_->setHeaderLabels({QStringLiteral("通道"),
                               QStringLiteral("名称"),
                               QStringLiteral("开始时间"),
                               QStringLiteral("结束时间"),
                               QStringLiteral("类型"),
                               QStringLiteral("文件路径"),
                               QStringLiteral("地址"),
                               QStringLiteral("保密"),
                               QStringLiteral("触发者"),
                               QStringLiteral("大小（字节）")});
    records_->setRootIsDecorated(false);
    records_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(records_, 1);
    auto* playback = new QHBoxLayout;
    utc_offset_->setObjectName(QStringLiteral("record_utc_offset"));
    utc_offset_->setRange(-14 * 60, 14 * 60);
    utc_offset_->setSuffix(QStringLiteral(" 分钟"));
    utc_offset_->setValue(QDateTime::currentDateTime().offsetFromUtc() / 60);
    play_->setObjectName(QStringLiteral("record_play"));
    playback->addWidget(new QLabel(QStringLiteral("设备 UTC 偏移（须人工确认；不自动识别夏令时）"), this));
    playback->addWidget(utc_offset_);
    playback->addStretch();
    playback->addWidget(play_);
    layout->addLayout(playback);
    auto* download = new QHBoxLayout;
    directory_->setObjectName(QStringLiteral("record_directory"));
    directory_->setText(record_directory());
    download_speed_->setObjectName(QStringLiteral("record_download_speed"));
    for (const int speed : {1, 2, 4})
    {
        download_speed_->addItem(QStringLiteral("%1 倍速").arg(speed), speed);
    }
    download_->setObjectName(QStringLiteral("record_download"));
    download_cancel_->setObjectName(QStringLiteral("record_download_cancel"));
    download->addWidget(new QLabel(QStringLiteral("保存目录"), this));
    download->addWidget(directory_, 1);
    download->addWidget(download_speed_);
    download->addWidget(download_);
    download->addWidget(download_cancel_);
    layout->addLayout(download);
    download_result_->setObjectName(QStringLiteral("record_download_result"));
    download_result_->setTextFormat(Qt::PlainText);
    download_result_->setWordWrap(true);
    layout->addWidget(download_result_);
    download_progress_->setInterval(500);
    connect(download_progress_,
            &QTimer::timeout,
            this,
            [this]()
            {
                if (downloader_)
                {
                    const auto stats = downloader_->snapshot();
                    download_result_->setText(
                        QStringLiteral("正在下载：已写入 %1 KiB，媒体 %2 秒").arg(stats.bytes / 1024).arg(stats.media_us / 1000000));
                }
            });
    connect(download_, &QPushButton::clicked, this, &record_query_dialog::start_download);
    connect(download_cancel_,
            &QPushButton::clicked,
            this,
            [this]()
            {
                cancel_download();
                download_result_->setText(QStringLiteral("已取消下载；临时文件由下载线程删除"));
            });
    result_->setObjectName(QStringLiteral("record_result"));
    result_->setTextFormat(Qt::PlainText);
    result_->setWordWrap(true);
    result_->setText(QStringLiteral("请填写时间范围后查询；每次最多接收 1000 条，请按需缩小范围"));
    layout->addWidget(result_);
    connect(query_, &QPushButton::clicked, this, &record_query_dialog::query);
    connect(records_, &QTreeWidget::itemSelectionChanged, this, [this]() { set_busy(!query_->isEnabled()); });
    connect(play_,
            &QPushButton::clicked,
            this,
            [this]()
            {
                qint64 start_time = 0;
                qint64 end_time = 0;
                QString stem;
                if (!selected_range(start_time, end_time, stem))
                {
                    return;
                }
                emit playback_requested(start_time, end_time);
                result_->setText(QStringLiteral("已向选中画面申请独占录像回放，UTC 偏移 %1 分钟；未停止其他实时上游").arg(utc_offset_->value()));
            });
    connect(cancel_,
            &QPushButton::clicked,
            this,
            [this]()
            {
                cancel_query();
                set_busy(false);
                result_->setText(QStringLiteral("已取消本次查询，未停止实时观看或共享上游"));
            });
    set_busy(false);
}

record_query_dialog::~record_query_dialog()
{
    cancel_query();
    cancel_download();
}

void record_query_dialog::done(int result)
{
    cancel_query();
    cancel_download();
    QDialog::done(result);
}

bool record_query_dialog::selected_range(qint64& start_time, qint64& end_time, QString& stem)
{
    const auto* item = records_->currentItem();
    if (!item || !item->isSelected())
    {
        return false;
    }
    const QDateTime first(QDate::fromString(item->text(2).left(10), QStringLiteral("yyyy-MM-dd")),
                          QTime::fromString(item->text(2).mid(11), QStringLiteral("HH:mm:ss")),
                          Qt::OffsetFromUTC,
                          utc_offset_->value() * 60);
    const QDateTime last(QDate::fromString(item->text(3).left(10), QStringLiteral("yyyy-MM-dd")),
                         QTime::fromString(item->text(3).mid(11), QStringLiteral("HH:mm:ss")),
                         Qt::OffsetFromUTC,
                         utc_offset_->value() * 60);
    if (!first.isValid() || !last.isValid() || first.toSecsSinceEpoch() <= 0 || first >= last ||
        first.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) != item->text(2) ||
        last.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) != item->text(3))
    {
        result_->setText(QStringLiteral("该目录项缺少合法时间范围，不能回放或下载；未申请媒体或使用文件路径"));
        return false;
    }
    start_time = first.toSecsSinceEpoch();
    end_time = last.toSecsSinceEpoch();
    // 文件名使用设备当地墙上时间，只含数字与下划线。
    stem = QStringLiteral("%1_%2_%3")
               .arg(channel_id_, first.toString(QStringLiteral("yyyyMMddHHmmss")), last.toString(QStringLiteral("yyyyMMddHHmmss")));
    return true;
}

void record_query_dialog::start_download()
{
    qint64 start_time = 0;
    qint64 end_time = 0;
    QString stem;
    if (download_session_ || !selected_range(start_time, end_time, stem))
    {
        return;
    }
    const auto directory = directory_->text().trimmed();
    if (directory.isEmpty() || !QDir().mkpath(directory))
    {
        download_result_->setText(QStringLiteral("保存目录无效或无法创建"));
        return;
    }
    QSettings().setValue(QString::fromLatin1(record_directory_key), directory);
    const auto target = QDir(directory).filePath(stem + QStringLiteral(".mkv"));
    download_session_ = new QObject(this);
    set_busy(!query_->isEnabled());
    download_result_->setText(QStringLiteral("正在申请设备下载…"));
    const auto path = QStringLiteral("/api/devices/%1/channels/%2/download")
                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(device_id_)), QString::fromLatin1(QUrl::toPercentEncoding(channel_id_)));
    request_history(
        client_,
        path,
        {{QStringLiteral("start_time"), start_time},
         {QStringLiteral("end_time"), end_time},
         {QStringLiteral("speed"), download_speed_->currentData().toInt()}},
        download_session_,
        [this, target](const QUrl& url, platform_client*, const QString&)
        {
            // 下载线程独立保活至结束；取消只请求中断，由线程删除本次临时文件。
            auto* downloader = new record_thread(url, target, playback_mode::finite, QCoreApplication::instance());
            downloader_ = downloader;
            connect(downloader,
                    &QThread::finished,
                    this,
                    [this, downloader, target]()
                    {
                        const auto stats = downloader->snapshot();
                        if (downloader_ != downloader)
                        {
                            return;
                        }
                        downloader_.clear();
                        download_progress_->stop();
                        // 下载结束（含失败）都只回收本次实例。
                        delete download_session_;
                        download_session_ = nullptr;
                        set_busy(!query_->isEnabled());
                        download_result_->setText(stats.saved ? QStringLiteral("下载完成：%1；传输完整结束，不代表请求范围库存完整").arg(target)
                                                              : QStringLiteral("下载失败：%1；未保存文件").arg(stats.error));
                    });
            connect(downloader, &QThread::finished, downloader, &QObject::deleteLater);
            downloader->start();
            download_progress_->start();
        },
        [this](const platform_response& response)
        {
            delete download_session_;
            download_session_ = nullptr;
            set_busy(!query_->isEnabled());
            download_result_->setText(response.ok() ? QStringLiteral("平台返回的下载实例或媒体地址无效")
                                                    : QStringLiteral("下载申请失败：%1").arg(response.error));
        });
}

void record_query_dialog::cancel_download()
{
    if (downloader_)
    {
        downloader_->requestInterruption();
        downloader_.clear();
    }
    download_progress_->stop();
    delete download_session_;
    download_session_ = nullptr;
    set_busy(!query_->isEnabled());
}

void record_query_dialog::cancel_query()
{
    delete context_;
    context_ = nullptr;
    if (reply_)
    {
        reply_->abort();
        reply_.clear();
    }
}

void record_query_dialog::set_busy(bool busy)
{
    start_->setEnabled(!busy);
    end_->setEnabled(!busy);
    type_->setEnabled(!busy);
    query_->setEnabled(!busy);
    cancel_->setEnabled(busy);
    const bool selected = records_->currentItem() && records_->currentItem()->isSelected();
    play_->setEnabled(!busy && selected);
    download_->setEnabled(!busy && selected && !download_session_);
    download_cancel_->setEnabled(download_session_ != nullptr);
}

void record_query_dialog::query()
{
    const auto start = start_->dateTime().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
    const auto end = end_->dateTime().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
    const auto type = type_->currentData().toString();
    if (!start_->dateTime().isValid() || !end_->dateTime().isValid() || start >= end ||
        (type != QStringLiteral("all") && type != QStringLiteral("time") && type != QStringLiteral("alarm") && type != QStringLiteral("manual")))
    {
        result_->setText(QStringLiteral("查询参数无效：开始必须早于结束，且必须选择合法录像类型"));
        return;
    }
    cancel_query();
    context_ = new QObject(this);
    set_busy(true);
    records_->clear();
    result_->setText(QStringLiteral("等待设备目录，平台总期限 15 秒…"));
    const auto path = QStringLiteral("/api/devices/%1/channels/%2/records/query")
                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(device_id_)), QString::fromLatin1(QUrl::toPercentEncoding(channel_id_)));
    reply_ = client_.request(
        "POST",
        path,
        {{QStringLiteral("start_time"), start}, {QStringLiteral("end_time"), end}, {QStringLiteral("type"), type}},
        context_,
        [this](platform_response response)
        {
            set_busy(false);
            if (!response.ok())
            {
                result_->setText(response.status == 422 && response.document.object().value(QStringLiteral("error")).toString() ==
                                                               QStringLiteral("record_result_too_large")
                                     ? QStringLiteral("设备声明超过 1000 条，请缩小查询时间范围；未取得目录结果")
                                     : QStringLiteral("录像查询失败：%1；未取得目录结果").arg(response.error));
                return;
            }
            const auto result = response.document.object();
            const auto total = result.value(QStringLiteral("sum_num"));
            const auto sn = result.value(QStringLiteral("sn"));
            const auto records = result.value(QStringLiteral("records"));
            if (response.status != 200 || result.value(QStringLiteral("device_id")).toString() != device_id_ ||
                result.value(QStringLiteral("channel_id")).toString() != channel_id_ ||
                result.value(QStringLiteral("command")).toString() != QStringLiteral("RecordInfo") || !sn.isDouble() || sn.toDouble() <= 0 ||
                sn.toDouble() != static_cast<double>(sn.toInteger()) || !total.isDouble() || total.toDouble() < 0 || total.toDouble() > 1000 ||
                !records.isArray() || total.toDouble() != records.toArray().size())
            {
                result_->setText(QStringLiteral("录像查询失败：平台返回的身份或目录总数无效"));
                return;
            }
            const QString fields[] = {QStringLiteral("device_id"),
                                      QStringLiteral("name"),
                                      QStringLiteral("start_time"),
                                      QStringLiteral("end_time"),
                                      QStringLiteral("type"),
                                      QStringLiteral("file_path"),
                                      QStringLiteral("address"),
                                      QStringLiteral("secrecy"),
                                      QStringLiteral("recorder_id"),
                                      QStringLiteral("file_size")};
            for (const auto& value : records.toArray())
            {
                const auto record = value.toObject();
                QStringList values;
                bool valid = value.isObject() && record.value(fields[0]).toString() == channel_id_ && record.value(fields[1]).isString();
                for (const auto& field : fields)
                {
                    const auto entry = record.value(field);
                    valid = valid && (entry.isUndefined() || entry.isString());
                    values.append(entry.toString());
                }
                if (!valid)
                {
                    records_->clear();
                    result_->setText(QStringLiteral("录像查询失败：平台返回的目录项无效"));
                    return;
                }
                new QTreeWidgetItem(records_, values);
            }
            result_->setText(QStringLiteral("设备声明 %1 条，已收到 %2 条；不代表文件唯一、库存完整或录像可回放")
                                 .arg(total.toInt())
                                 .arg(records_->topLevelItemCount()));
        },
        20'000);
}

}    // namespace media_server
