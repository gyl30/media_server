#include <utility>

#include <QUrl>
#include <QLabel>
#include <QVariant>
#include <QComboBox>
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

#include "desktop/platform_client.h"
#include "desktop/record_query_dialog.h"

namespace media_server
{

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
      records_(new QTreeWidget(this)),
      result_(new QLabel(this))
{
    setObjectName(QStringLiteral("record_query_dialog"));
    setWindowTitle(QStringLiteral("录像查询 · %1").arg(channel_id_));
    setWindowModality(Qt::NonModal);
    resize(1000, 480);
    auto* layout = new QVBoxLayout(this);
    auto* target = new QLabel(QStringLiteral("父设备：%1\n通道：%2\n时间按设备当地墙上时间填写，不作时区转换；这里只检索目录，不代表文件可回放。")
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
    result_->setObjectName(QStringLiteral("record_result"));
    result_->setTextFormat(Qt::PlainText);
    result_->setWordWrap(true);
    result_->setText(QStringLiteral("请填写时间范围后查询；每次最多接收 1000 条，请按需缩小范围"));
    layout->addWidget(result_);
    connect(query_, &QPushButton::clicked, this, &record_query_dialog::query);
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

record_query_dialog::~record_query_dialog() { cancel_query(); }

void record_query_dialog::done(int result)
{
    cancel_query();
    QDialog::done(result);
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
