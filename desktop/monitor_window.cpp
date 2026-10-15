#include <utility>
#include <algorithm>

#include <SDL.h>
#include <QMenu>
#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QShortcut>
#include <QTabWidget>
#include <QStatusBar>
#include <QJsonArray>
#include <QDateTime>
#include <QPushButton>
#include <QCloseEvent>
#include <QStringList>
#include <QFileDialog>
#include <QTreeWidget>
#include <QGridLayout>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QApplication>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QStandardPaths>

#include "desktop/playback_view.h"
#include "desktop/monitor_window.h"

namespace media_server
{

monitor_window::monitor_window(QWidget* parent)
    : QMainWindow(parent),
      client_(this),
      grid_(new QGridLayout),
      view_layout_(new QComboBox(this)),
      sound_(new QPushButton(QStringLiteral("开启声音"), this)),
      catalog_(new QPushButton(QStringLiteral("查询目录"), this)),
      device_info_(new QPushButton(QStringLiteral("设备信息"), this)),
      device_status_(new QPushButton(QStringLiteral("设备状态"), this)),
      device_target_(new QLabel(this)),
      device_result_(new QPlainTextEdit(this)),
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
    auto* videos = new QWidget(split);
    videos->setLayout(grid_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(2);
    for (std::size_t index = 0; index < views_.size(); ++index)
    {
        auto* view = new playback_view(videos);
        views_[index] = view;
        view->setObjectName(QStringLiteral("view_%1").arg(index + 1, 2, 10, QLatin1Char('0')));
        connect(view, &playback_view::selected, this, [this, view]() { select_view(view); });
        connect(view,
                &playback_view::dropped,
                this,
                [this, view](const QVariantMap& data)
                {
                    select_view(view);
                    play_data(data, view);
                });
        connect(view,
                &playback_view::audible_changed,
                this,
                [this, view](bool)
                {
                    if (selected_ == view)
                    {
                        const QSignalBlocker blocked(sound_);
                        sound_->setChecked(view->audible());
                        sound_->setText(view->audible() ? QStringLiteral("关闭声音") : QStringLiteral("开启声音"));
                    }
                });
        connect(view,
                &playback_view::playback_error,
                this,
                [this](const QString& error)
                {
                    if (audio_id_ != 0 && SDL_GetAudioDeviceStatus(audio_id_) == SDL_AUDIO_STOPPED)
                    {
                        SDL_CloseAudioDevice(audio_id_);
                        audio_id_ = 0;
                    }
                    statusBar()->showMessage(error);
                });
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view,
                &QWidget::customContextMenuRequested,
                this,
                [this, view](const QPoint& position)
                {
                    select_view(view);
                    QMenu menu(this);
                    for (int count : {1, 4, 9, 16})
                    {
                        auto* action = menu.addAction(QStringLiteral("%1画面").arg(count));
                        action->setCheckable(true);
                        action->setChecked(view_layout_->currentData().toInt() == count);
                        connect(action, &QAction::triggered, this, [this, count]() { view_layout_->setCurrentIndex(view_layout_->findData(count)); });
                    }
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("停止此画面"), view, &playback_view::stop);
                    menu.addAction(QStringLiteral("停止全部"), this, &monitor_window::stop_all);
                    menu.addAction(QStringLiteral("抓图"), this, &monitor_window::take_snapshot);
                    menu.exec(view->mapToGlobal(position));
                });
    }
    split->addWidget(videos);
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
    catalog_->setObjectName(QStringLiteral("catalog_query"));
    side->addWidget(catalog_);
    devices_->setHeaderLabel(QStringLiteral("设备与通道"));
    devices_->setObjectName(QStringLiteral("device_tree"));
    devices_->setDragEnabled(true);
    devices_->setDragDropMode(QAbstractItemView::DragOnly);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    side->addWidget(devices_, 1);
    auto* controls = new QTabWidget(sidebar);
    auto* basic = new QWidget(controls);
    auto* basic_layout = new QVBoxLayout(basic);
    basic_layout->setContentsMargins(4, 4, 4, 4);
    device_target_->setObjectName(QStringLiteral("device_query_target"));
    device_target_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    basic_layout->addWidget(device_target_);
    auto* query_buttons = new QHBoxLayout;
    device_info_->setObjectName(QStringLiteral("device_info_query"));
    device_status_->setObjectName(QStringLiteral("device_status_query"));
    query_buttons->addWidget(device_info_);
    query_buttons->addWidget(device_status_);
    basic_layout->addLayout(query_buttons);
    device_result_->setObjectName(QStringLiteral("device_query_result"));
    device_result_->setReadOnly(true);
    basic_layout->addWidget(device_result_);
    controls->addTab(basic, QStringLiteral("基本"));
    for (const auto& title : {QStringLiteral("云台"), QStringLiteral("预置位")})
    {
        auto* page = new QWidget(controls);
        auto* page_layout = new QVBoxLayout(page);
        page_layout->addWidget(new QLabel(QStringLiteral("%1功能将在后续阶段接入真实设备").arg(title), page));
        page_layout->addStretch();
        controls->addTab(page, title);
    }
    controls->setMaximumHeight(240);
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
    view_layout_->setObjectName(QStringLiteral("view_layout"));
    for (int count : {1, 4, 9, 16})
    {
        view_layout_->addItem(QStringLiteral("%1画面").arg(count), count);
    }
    auto* aspect = new QCheckBox(QStringLiteral("保持比例"), central);
    aspect->setObjectName(QStringLiteral("keep_aspect"));
    aspect->setChecked(QSettings().value(QStringLiteral("keep_aspect"), true).toBool());
    auto* screenshot = new QPushButton(QStringLiteral("抓图"), central);
    screenshot->setObjectName(QStringLiteral("snapshot"));
    auto* fullscreen = new QPushButton(QStringLiteral("全屏"), central);
    fullscreen->setObjectName(QStringLiteral("fullscreen"));
    sound_->setCheckable(true);
    sound_->setObjectName(QStringLiteral("sound_toggle"));
    auto* stop = new QPushButton(QStringLiteral("停止预览"), central);
    stop->setObjectName(QStringLiteral("stop_preview"));
    auto* stop_everything = new QPushButton(QStringLiteral("停止全部"), central);
    stop_everything->setObjectName(QStringLiteral("stop_all"));
    bottom->addWidget(view_layout_);
    bottom->addWidget(aspect);
    bottom->addWidget(screenshot);
    bottom->addWidget(fullscreen);
    bottom->addWidget(sound_);
    bottom->addWidget(stop);
    bottom->addWidget(stop_everything);
    layout->addLayout(bottom);
    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("Linux x86_64 · H.264 · RTSP/TCP · 默认静音"));
    connect(refresh, &QPushButton::clicked, this, &monitor_window::reload_devices);
    connect(address_, &QLineEdit::returnPressed, this, &monitor_window::reload_devices);
    connect(devices_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { play_item(item); });
    connect(catalog_, &QPushButton::clicked, this, &monitor_window::query_catalog);
    connect(devices_, &QTreeWidget::currentItemChanged, this, [this]() { reset_device_query(); });
    connect(device_info_, &QPushButton::clicked, this, [this]() { query_device(QStringLiteral("DeviceInfo")); });
    connect(device_status_, &QPushButton::clicked, this, [this]() { query_device(QStringLiteral("DeviceStatus")); });
    reset_device_query();
    connect(stop, &QPushButton::clicked, this, [this]() { selected_->stop(); });
    connect(stop_everything, &QPushButton::clicked, this, &monitor_window::stop_all);
    connect(screenshot, &QPushButton::clicked, this, &monitor_window::take_snapshot);
    connect(view_layout_, &QComboBox::currentIndexChanged, this, [this]() { set_layout(view_layout_->currentData().toInt()); });
    connect(aspect,
            &QCheckBox::toggled,
            this,
            [this](bool keep)
            {
                for (auto* view : views_)
                {
                    view->set_keep_aspect(keep);
                }
                QSettings().setValue(QStringLiteral("keep_aspect"), keep);
            });
    connect(fullscreen, &QPushButton::clicked, this, [this]() { isFullScreen() ? showNormal() : showFullScreen(); });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape,
            &QShortcut::activated,
            this,
            [this]()
            {
                if (isFullScreen())
                {
                    showNormal();
                }
            });
    connect(sound_,
            &QPushButton::toggled,
            this,
            [this](bool enabled)
            {
                for (auto* view : views_)
                {
                    view->set_audible(0, 0);
                }
                if (!enabled)
                {
                    return;
                }
                if (audio_id_ == 0)
                {
                    SDL_AudioSpec wanted{};
                    wanted.freq = 48'000;
                    wanted.format = AUDIO_S16SYS;
                    wanted.channels = 2;
                    wanted.samples = 512;
                    SDL_AudioSpec obtained{};
                    audio_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
                    if (audio_id_ == 0)
                    {
                        const QSignalBlocker blocked(sound_);
                        sound_->setChecked(false);
                        statusBar()->showMessage(QStringLiteral("无法打开音频设备，继续静音观看"));
                        return;
                    }
                    audio_period_bytes_ = obtained.samples * 4;
                }
                selected_->set_audible(audio_id_, audio_period_bytes_);
            });
    select_view(views_.front());
    for (auto* view : views_)
    {
        view->set_keep_aspect(aspect->isChecked());
    }
    const auto saved_count = QSettings().value(QStringLiteral("view_count"), 1).toInt();
    const auto saved_index = view_layout_->findData(saved_count);
    view_layout_->setCurrentIndex(saved_index < 0 ? 0 : saved_index);
    set_layout(view_layout_->currentData().toInt());
    const auto geometry = QSettings().value(QStringLiteral("geometry")).toRect();
    if (geometry.isValid())
    {
        setGeometry(geometry);
    }
    QTimer::singleShot(0, this, &monitor_window::reload_devices);
}

monitor_window::~monitor_window()
{
    stop_all();
    if (audio_id_ != 0)
    {
        SDL_CloseAudioDevice(audio_id_);
    }
}

void monitor_window::select_view(playback_view* view)
{
    selected_ = view;
    for (auto* item : views_)
    {
        item->setProperty("selected", item == view);
        item->update();
    }
    const QSignalBlocker blocked(sound_);
    sound_->setChecked(view->audible());
    sound_->setText(view->audible() ? QStringLiteral("关闭声音") : QStringLiteral("开启声音"));
}

void monitor_window::set_layout(int count)
{
    const int columns = count == 1 ? 1 : count == 4 ? 2 : count == 9 ? 3 : 4;
    for (int index = 0; index < static_cast<int>(views_.size()); ++index)
    {
        auto* view = views_[index];
        grid_->removeWidget(view);
        if (index >= count)
        {
            view->stop();
            view->hide();
            continue;
        }
        grid_->addWidget(view, index / columns, index % columns);
        view->show();
    }
    // QGridLayout 保留曾使用过的行列；显式清除旧 stretch，避免缩布局后留空白。
    for (int index = 0; index < 4; ++index)
    {
        grid_->setRowStretch(index, index < columns ? 1 : 0);
        grid_->setColumnStretch(index, index < columns ? 1 : 0);
    }
    if (std::find(views_.begin(), views_.begin() + count, selected_) == views_.begin() + count)
    {
        select_view(views_.front());
    }
    QSettings().setValue(QStringLiteral("view_count"), count);
}

void monitor_window::stop_all()
{
    for (auto* view : views_)
    {
        view->stop();
    }
}

void monitor_window::take_snapshot()
{
    const auto image = selected_->image();
    if (image.isNull())
    {
        statusBar()->showMessage(QStringLiteral("当前画面没有可保存的图像"));
        return;
    }
    const auto path =
        QFileDialog::getSaveFileName(this,
                                     QStringLiteral("保存当前画面"),
                                     QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/监控-") +
                                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) + QStringLiteral(".png"),
                                     QStringLiteral("PNG 图像 (*.png)"));
    if (path.isEmpty())
    {
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
    {
        statusBar()->showMessage(QStringLiteral("保存抓图失败，请检查路径和可用空间"));
        return;
    }
    statusBar()->showMessage(QStringLiteral("已保存当前显示分辨率的图像"));
}

void monitor_window::query_catalog()
{
    auto* item = devices_->currentItem();
    if (!item)
    {
        connection_->setText(QStringLiteral("请先选择国标设备或通道"));
        return;
    }
    const auto data = item->data(0, Qt::UserRole).toMap();
    const auto kind = data.value(QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("gb_device") && kind != QStringLiteral("gb_channel"))
    {
        connection_->setText(QStringLiteral("目录查询仅用于国标设备"));
        return;
    }
    catalog_->setEnabled(false);
    const auto path =
        QStringLiteral("/api/devices/%1/catalog").arg(QString::fromLatin1(QUrl::toPercentEncoding(data.value(QStringLiteral("id")).toString())));
    client_.request("POST",
                    path,
                    {},
                    listing_,
                    [this](platform_response response)
                    {
                        catalog_->setEnabled(true);
                        connection_->setText(response.ok() ? QStringLiteral("目录查询已受理，请稍后双击设备刷新通道")
                                                           : QStringLiteral("目录查询失败：%1").arg(response.error));
                    });
}

void monitor_window::reset_device_query()
{
    delete control_context_;
    control_context_ = nullptr;
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    const auto kind = data.value(QStringLiteral("kind")).toString();
    const bool available = listing_ && (kind == QStringLiteral("gb_device") || kind == QStringLiteral("gb_channel"));
    device_info_->setEnabled(available);
    device_status_->setEnabled(available);
    device_target_->setText(available ? data.value(QStringLiteral("id")).toString() : QStringLiteral("请选择国标设备或通道"));
    device_result_->clear();
}

void monitor_window::query_device(const QString& command)
{
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    const auto kind = data.value(QStringLiteral("kind")).toString();
    if (!listing_ || (kind != QStringLiteral("gb_device") && kind != QStringLiteral("gb_channel")))
    {
        return;
    }
    const auto id = data.value(QStringLiteral("id")).toString();
    delete control_context_;
    control_context_ = new QObject(listing_);
    device_info_->setEnabled(false);
    device_status_->setEnabled(false);
    device_result_->setPlainText(
        QStringLiteral("等待设备返回%1…").arg(command == QStringLiteral("DeviceInfo") ? QStringLiteral("信息") : QStringLiteral("状态")));
    const auto path = QStringLiteral("/api/devices/%1/query").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)));
    client_.request(
        "POST",
        path,
        {{QStringLiteral("command"), command}},
        control_context_,
        [this, id, command](platform_response response)
        {
            device_info_->setEnabled(true);
            device_status_->setEnabled(true);
            if (!response.ok())
            {
                device_result_->setPlainText(QStringLiteral("查询失败：%1").arg(response.error));
                return;
            }
            const auto result = response.document.object();
            const auto outcome = result.value(QStringLiteral("result")).toString();
            if (result.value(QStringLiteral("device_id")).toString() != id || result.value(QStringLiteral("command")).toString() != command ||
                (outcome != QStringLiteral("OK") && outcome != QStringLiteral("ERROR")))
            {
                device_result_->setPlainText(QStringLiteral("查询失败：平台返回的设备信息无效"));
                return;
            }
            if (outcome != QStringLiteral("OK"))
            {
                device_result_->setPlainText(QStringLiteral("设备拒绝本次查询（ERROR），未取得有效结果"));
                return;
            }
            QStringList lines;
            lines << QStringLiteral("%1 · %2").arg(
                command == QStringLiteral("DeviceInfo") ? QStringLiteral("设备信息") : QStringLiteral("设备自报状态"),
                QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
            const auto payload = result.value(command == QStringLiteral("DeviceInfo") ? QStringLiteral("info") : QStringLiteral("status"));
            if (!payload.isObject())
            {
                device_result_->setPlainText(QStringLiteral("查询失败：平台返回的查询结果无效"));
                return;
            }
            const auto values = payload.toObject();
            const std::pair<QString, QString> fields[] = {
                {QStringLiteral("device_name"), QStringLiteral("名称")},
                {QStringLiteral("manufacturer"), QStringLiteral("厂商")},
                {QStringLiteral("model"), QStringLiteral("型号")},
                {QStringLiteral("firmware"), QStringLiteral("固件")},
                {QStringLiteral("channel"), QStringLiteral("输入通道数")},
                {QStringLiteral("online"), QStringLiteral("设备自报在线")},
                {QStringLiteral("status"), QStringLiteral("工作状态")},
                {QStringLiteral("reason"), QStringLiteral("原因")},
                {QStringLiteral("encode"), QStringLiteral("编码")},
                {QStringLiteral("record"), QStringLiteral("录像")},
                {QStringLiteral("device_time"), QStringLiteral("设备时间")},
            };
            for (const auto& field : fields)
            {
                if (values.contains(field.first))
                {
                    lines << field.second + QStringLiteral("：") + values.value(field.first).toVariant().toString();
                }
            }
            device_result_->setPlainText(lines.join(QLatin1Char('\n')));
        });
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
        stop_all();
    }
    QSettings().setValue(QStringLiteral("platform_url"), base.toString());
    delete listing_;
    control_context_ = nullptr;
    listing_ = new QObject(this);
    catalog_->setEnabled(true);
    devices_->clear();
    reset_device_query();
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
    play_data(data, selected_);
}

void monitor_window::play_data(const QVariantMap& data, playback_view* view)
{
    const auto kind = data.value(QStringLiteral("kind")).toString();
    const auto id = data.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || (kind != QStringLiteral("gb_channel") && kind != QStringLiteral("source") && kind != QStringLiteral("push_device")))
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
    view->play(client_, target);
}

void monitor_window::closeEvent(QCloseEvent* event)
{
    stop_all();
    delete listing_;
    listing_ = nullptr;
    control_context_ = nullptr;
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
    QSettings().setValue(QStringLiteral("geometry"), isFullScreen() ? normalGeometry() : geometry());
    event->accept();
}

}    // namespace media_server
