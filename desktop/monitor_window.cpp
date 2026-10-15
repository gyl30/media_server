#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QLineEdit>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QStatusBar>
#include <QJsonArray>
#include <QPushButton>
#include <QCloseEvent>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QApplication>
#include <QSignalBlocker>

#include "desktop/playback_view.h"
#include "desktop/monitor_window.h"

namespace media_server
{

monitor_window::monitor_window(QWidget* parent)
    : QMainWindow(parent),
      client_(this),
      view_(new playback_view(this)),
      devices_(new QTreeWidget(this)),
      address_(new QLineEdit(this)),
      connection_(new QLabel(this))
{
    setWindowTitle(QStringLiteral("国标视频监控客户端 — Media Server"));
    resize(1280, 800);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* split = new QSplitter(central);
    split->addWidget(view_);
    auto* sidebar = new QWidget(split);
    sidebar->setMinimumWidth(260);
    sidebar->setMaximumWidth(420);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(6, 0, 2, 0);
    auto* actions = new QHBoxLayout;
    auto* service = new QPushButton(QStringLiteral("停止服务"), sidebar);
    service->setEnabled(false);
    service->setToolTip(QStringLiteral("桌面观看端不停止后台平台服务"));
    auto* refresh = new QPushButton(QStringLiteral("连接 / 刷新"), sidebar);
    refresh->setObjectName(QStringLiteral("refresh_devices"));
    actions->addWidget(service);
    actions->addWidget(refresh);
    side->addLayout(actions);
    address_->setPlaceholderText(QStringLiteral("平台 HTTP 地址"));
    address_->setObjectName(QStringLiteral("platform_address"));
    address_->setText(QSettings().value(QStringLiteral("platform_url"), QStringLiteral("http://127.0.0.1:9090")).toString());
    side->addWidget(address_);
    side->addWidget(connection_);
    devices_->setHeaderLabel(QStringLiteral("设备与通道"));
    devices_->setObjectName(QStringLiteral("device_tree"));
    side->addWidget(devices_, 1);
    auto* controls = new QTabWidget(sidebar);
    for (const auto& title : {QStringLiteral("基本"), QStringLiteral("云台"), QStringLiteral("预置位")})
    {
        auto* page = new QWidget(controls);
        auto* page_layout = new QVBoxLayout(page);
        page_layout->addWidget(new QLabel(QStringLiteral("%1功能将在后续阶段接入真实设备").arg(title), page));
        page_layout->addStretch();
        controls->addTab(page, title);
    }
    controls->setCurrentIndex(2);
    controls->setMaximumHeight(180);
    side->addWidget(controls);
    split->addWidget(sidebar);
    split->setStretchFactor(0, 1);
    split->setSizes({990, 280});
    layout->addWidget(split, 1);
    auto* bottom = new QHBoxLayout;
    auto* tabs = new QTabWidget(central);
    for (const auto& title : {QStringLiteral("实时预览"),
                              QStringLiteral("录像回放"),
                              QStringLiteral("录像下载"),
                              QStringLiteral("推流分发"),
                              QStringLiteral("警情信息"),
                              QStringLiteral("调试打印"),
                              QStringLiteral("后台服务")})
    {
        const auto index = tabs->addTab(new QWidget(tabs), title);
        tabs->setTabEnabled(index, index == 0);
    }
    tabs->setFixedHeight(34);
    bottom->addWidget(tabs, 1);
    auto* sound = new QPushButton(QStringLiteral("开启声音"), central);
    sound->setCheckable(true);
    auto* stop = new QPushButton(QStringLiteral("停止预览"), central);
    bottom->addWidget(sound);
    bottom->addWidget(stop);
    layout->addLayout(bottom);
    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("Linux x86_64 · H.264 · RTSP/TCP · 默认静音"));
    connect(refresh, &QPushButton::clicked, this, &monitor_window::reload_devices);
    connect(address_, &QLineEdit::returnPressed, this, &monitor_window::reload_devices);
    connect(devices_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { play_item(item); });
    connect(stop, &QPushButton::clicked, view_, &playback_view::stop);
    connect(sound,
            &QPushButton::toggled,
            this,
            [this, sound](bool enabled)
            {
                view_->set_audible(enabled);
                sound->setText(enabled ? QStringLiteral("关闭声音") : QStringLiteral("开启声音"));
            });
    connect(view_,
            &playback_view::audible_changed,
            sound,
            [sound](bool enabled)
            {
                const QSignalBlocker blocked(sound);
                sound->setChecked(enabled);
                sound->setText(enabled ? QStringLiteral("关闭声音") : QStringLiteral("开启声音"));
            });
    connect(view_, &playback_view::playback_error, this, [this](const QString& error) { statusBar()->showMessage(error); });
    QTimer::singleShot(0, this, &monitor_window::reload_devices);
}

void monitor_window::reload_devices()
{
    const QUrl base(address_->text().trimmed(), QUrl::StrictMode);
    const auto previous = client_.base_url();
    if (!platform_client::valid_base_url(base))
    {
        connection_->setText(QStringLiteral("平台地址无效"));
        return;
    }
    client_.set_base_url(base);
    if (client_.base_url() != previous)
    {
        view_->stop();
    }
    QSettings().setValue(QStringLiteral("platform_url"), base.toString());
    delete listing_;
    listing_ = new QObject(this);
    devices_->clear();
    connection_->setText(QStringLiteral("正在查询设备…"));
    auto* gb = new QTreeWidgetItem(devices_, {QStringLiteral("GB28181 设备")});
    gb->setExpanded(true);
    client_.request(
        "GET",
        QStringLiteral("/api/devices"),
        {},
        listing_,
        [this, gb](platform_response response)
        {
            if (!response.ok())
            {
                gb->setText(0, QStringLiteral("GB28181 查询失败：%1").arg(response.error));
                return;
            }
            for (const auto& value : response.document.object().value(QStringLiteral("devices")).toArray())
            {
                const auto device = value.toObject();
                const auto id = device.value(QStringLiteral("device_id")).toString();
                const auto name = device.value(QStringLiteral("name")).toString();
                auto* item =
                    new QTreeWidgetItem(gb,
                                        {QStringLiteral("%1 [%2]").arg(
                                            name.isEmpty() ? id : name,
                                            device.value(QStringLiteral("online")).toBool() ? QStringLiteral("在线") : QStringLiteral("离线"))});
                item->setData(0, Qt::UserRole, QVariantMap{{QStringLiteral("kind"), QStringLiteral("gb_device")}, {QStringLiteral("id"), id}});
            }
            connection_->setText(QStringLiteral("平台已连接；双击设备查看缓存通道"));
        });
    for (const auto& kind : {QStringLiteral("source"), QStringLiteral("push_device")})
    {
        const bool source = kind == QStringLiteral("source");
        auto* root = new QTreeWidgetItem(devices_, {source ? QStringLiteral("主动拉流") : QStringLiteral("被动收流")});
        root->setExpanded(true);
        client_.request(
            "GET",
            source ? QStringLiteral("/api/sources") : QStringLiteral("/api/push-devices"),
            {},
            listing_,
            [root, kind, source](platform_response response)
            {
                if (!response.ok())
                {
                    root->setText(0, QStringLiteral("设备查询失败：%1").arg(response.error));
                    return;
                }
                const auto key = source ? QStringLiteral("sources") : QStringLiteral("devices");
                for (const auto& value : response.document.object().value(key).toArray())
                {
                    const auto device = value.toObject();
                    const auto id = device.value(source ? QStringLiteral("source_id") : QStringLiteral("device_id")).toString();
                    auto name = device.value(source ? QStringLiteral("stream_name") : QStringLiteral("name")).toString();
                    if (name.isEmpty())
                    {
                        name = id;
                    }
                    auto* item = new QTreeWidgetItem(root, {name});
                    item->setData(
                        0, Qt::UserRole, QVariantMap{{QStringLiteral("kind"), kind}, {QStringLiteral("id"), id}, {QStringLiteral("name"), name}});
                }
            });
    }
}

void monitor_window::play_item(QTreeWidgetItem* item)
{
    const auto data = item->data(0, Qt::UserRole).toMap();
    const auto kind = data.value(QStringLiteral("kind")).toString();
    const auto id = data.value(QStringLiteral("id")).toString();
    if (kind == QStringLiteral("gb_device"))
    {
        while (item->childCount() != 0)
        {
            delete item->takeChild(0);
        }
        const auto path = QStringLiteral("/api/devices/%1/channels").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)));
        // 请求归属于该树项的独立 context；刷新设备树会先销毁 listing_。
        auto* context = new QObject(listing_);
        item->setDisabled(true);
        client_.request("GET",
                        path,
                        {},
                        context,
                        [this, item, id, context](platform_response response)
                        {
                            item->setDisabled(false);
                            context->deleteLater();
                            if (!response.ok())
                            {
                                connection_->setText(QStringLiteral("通道查询失败：%1；可手动重试").arg(response.error));
                                return;
                            }
                            for (const auto& value : response.document.object().value(QStringLiteral("channels")).toArray())
                            {
                                const auto channel = value.toObject();
                                const auto channel_id = channel.value(QStringLiteral("channel_id")).toString();
                                const auto name = channel.value(QStringLiteral("name")).toString();
                                auto* child = new QTreeWidgetItem(item, {name.isEmpty() ? channel_id : name});
                                child->setData(0,
                                               Qt::UserRole,
                                               QVariantMap{{QStringLiteral("kind"), QStringLiteral("gb_channel")},
                                                           {QStringLiteral("id"), id},
                                                           {QStringLiteral("channel_id"), channel_id},
                                                           {QStringLiteral("name"), name.isEmpty() ? channel_id : name}});
                            }
                            item->setExpanded(true);
                        });
        return;
    }
    if (kind != QStringLiteral("gb_channel") && kind != QStringLiteral("source") && kind != QStringLiteral("push_device"))
    {
        return;
    }
    play_target target;
    target.type = kind == QStringLiteral("gb_channel") ? play_target::kind::gb_channel
                  : kind == QStringLiteral("source")   ? play_target::kind::source
                                                       : play_target::kind::push_device;
    target.id = id;
    target.channel_id = data.value(QStringLiteral("channel_id")).toString();
    target.name = data.value(QStringLiteral("name")).toString();
    view_->play(client_, target);
}

void monitor_window::closeEvent(QCloseEvent* event)
{
    view_->stop();
    delete listing_;
    listing_ = nullptr;
    for (auto* worker : qApp->findChildren<decode_thread*>())
    {
        if (worker->isRunning())
        {
            worker->requestInterruption();
            event->ignore();
            setEnabled(false);
            statusBar()->showMessage(QStringLiteral("正在关闭媒体连接…"));
            QTimer::singleShot(30, this, &QWidget::close);
            return;
        }
    }
    event->accept();
}

}    // namespace media_server
