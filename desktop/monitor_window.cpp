#include <utility>
#include <algorithm>

#include <QDir>
#include <QSet>
#include <SDL.h>
#include <QMenu>
#include <QLabel>
#include <QTimer>
#include <QVariant>
#include <QPointer>
#include <QSpinBox>
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
#include <QMessageBox>
#include <QGridLayout>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QStandardPaths>

#include "desktop/playback_view.h"
#include "desktop/talk_session.h"
#include "desktop/record_thread.h"
#include "desktop/monitor_window.h"
#include "desktop/record_query_dialog.h"

namespace media_server
{

monitor_window::monitor_window(QWidget* parent)
    : QMainWindow(parent),
      client_(this),
      grid_(new QGridLayout),
      view_layout_(new QComboBox(this)),
      sound_(new QPushButton(QStringLiteral("开启声音"), this)),
      talk_button_(new QPushButton(QStringLiteral("开始对讲"), this)),
      record_pause_(new QPushButton(QStringLiteral("暂停回放"), this)),
      record_position_(new QDoubleSpinBox(this)),
      record_seek_(new QPushButton(QStringLiteral("请求定位"), this)),
      record_range_(new QLabel(this)),
      record_live_(new QPushButton(QStringLiteral("开始录制"), this)),
      alarm_list_(new QTreeWidget(this)),
      alarm_status_(new QLabel(this)),
      alarm_timer_(new QTimer(this)),
      catalog_(new QPushButton(QStringLiteral("查询目录"), this)),
      record_query_(new QPushButton(QStringLiteral("录像查询"), this)),
      device_info_(new QPushButton(QStringLiteral("设备信息"), this)),
      device_status_(new QPushButton(QStringLiteral("设备状态"), this)),
      device_target_(new QLabel(this)),
      device_result_(new QPlainTextEdit(this)),
      preset_query_(new QPushButton(QStringLiteral("查询预置位"), this)),
      preset_list_(new QTreeWidget(this)),
      preset_result_(new QLabel(this)),
      preset_number_(new QSpinBox(this)),
      preset_set_(new QPushButton(QStringLiteral("设置"), this)),
      preset_goto_(new QPushButton(QStringLiteral("调用"), this)),
      preset_remove_(new QPushButton(QStringLiteral("删除"), this)),
      ptz_stop_(new QPushButton(QStringLiteral("停止"), this)),
      ptz_speed_(new QSpinBox(this)),
      ptz_zoom_speed_(new QSpinBox(this)),
      ptz_result_(new QLabel(this)),
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
                &playback_view::status_changed,
                this,
                [this, view]()
                {
                    if (selected_ == view)
                    {
                        update_view_controls();
                    }
                });
        connect(view,
                &playback_view::recording_changed,
                this,
                [this, view]()
                {
                    if (selected_ == view)
                    {
                        update_view_controls();
                    }
                });
        connect(view, &playback_view::recording_finished, this, [this](const QString& message) { statusBar()->showMessage(message); });
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
                    menu.addAction(QStringLiteral("设置录像目录…"),
                                   this,
                                   [this]()
                                   {
                                       const auto directory = QFileDialog::getExistingDirectory(this, QStringLiteral("录像目录"), record_directory());
                                       if (!directory.isEmpty())
                                       {
                                           QSettings().setValue(record_directory_key, directory);
                                       }
                                   });
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
    record_query_->setObjectName(QStringLiteral("open_record_query"));
    auto* queries = new QHBoxLayout;
    queries->addWidget(catalog_);
    queries->addWidget(record_query_);
    side->addLayout(queries);
    talk_button_->setObjectName(QStringLiteral("talk_device"));
    talk_button_->setToolTip(QStringLiteral("向选中国标设备上送麦克风；设备回传声音通过观看画面的“开启声音”独立控制"));
    side->addWidget(talk_button_);
    connect(talk_button_,
            &QPushButton::clicked,
            this,
            [this]()
            {
                if (talk_)
                {
                    talk_button_->setEnabled(false);
                    talk_->stop();
                    return;
                }
                const auto* item = devices_->currentItem();
                const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
                const auto id = data.value(QStringLiteral("id")).toString();
                auto* session = new talk_session(client_.base_url(), id, this);
                talk_ = session;
                talk_button_->setText(QStringLiteral("停止对讲"));
                statusBar()->showMessage(QStringLiteral("正在建立设备 %1 的音频上传…").arg(id));
                connect(session,
                        &talk_session::transmitting,
                        this,
                        [this, id]() { statusBar()->showMessage(QStringLiteral("正在向 %1 上送麦克风；设备听音未确认").arg(id)); });
                connect(session,
                        &talk_session::finished,
                        this,
                        [this, session](const QString& error)
                        {
                            if (talk_ == session)
                            {
                                talk_ = nullptr;
                                talk_button_->setText(QStringLiteral("开始对讲"));
                                const auto* item = devices_->currentItem();
                                const auto kind = item ? item->data(0, Qt::UserRole).toMap().value(QStringLiteral("kind")).toString() : QString{};
                                talk_button_->setEnabled(listing_ && (kind == QStringLiteral("gb_device") || kind == QStringLiteral("gb_channel")));
                                statusBar()->showMessage(error.isEmpty() ? QStringLiteral("对讲上传已停止") : error);
                            }
                            session->deleteLater();
                        });
                session->start();
            });
    devices_->setHeaderLabel(QStringLiteral("设备与通道"));
    devices_->setObjectName(QStringLiteral("device_tree"));
    devices_->setDragEnabled(true);
    devices_->setDragDropMode(QAbstractItemView::DragOnly);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    side->addWidget(devices_, 1);
    device_target_->setObjectName(QStringLiteral("device_query_target"));
    device_target_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    device_target_->setWordWrap(true);
    side->addWidget(device_target_);
    auto* controls = new QTabWidget(sidebar);
    auto* basic = new QWidget(controls);
    auto* basic_layout = new QVBoxLayout(basic);
    basic_layout->setContentsMargins(4, 4, 4, 4);
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
    auto* ptz = new QWidget(controls);
    auto* ptz_layout = new QVBoxLayout(ptz);
    ptz_layout->setContentsMargins(4, 4, 4, 4);
    auto* directions = new QGridLayout;
    const QString commands[] = {QStringLiteral("up_left"),
                                QStringLiteral("up"),
                                QStringLiteral("up_right"),
                                QStringLiteral("left"),
                                QStringLiteral("right"),
                                QStringLiteral("down_left"),
                                QStringLiteral("down"),
                                QStringLiteral("down_right"),
                                QStringLiteral("zoom_in"),
                                QStringLiteral("zoom_out")};
    const QString labels[] = {QStringLiteral("↖"),
                              QStringLiteral("↑"),
                              QStringLiteral("↗"),
                              QStringLiteral("←"),
                              QStringLiteral("→"),
                              QStringLiteral("↙"),
                              QStringLiteral("↓"),
                              QStringLiteral("↘"),
                              QStringLiteral("变倍 +"),
                              QStringLiteral("变倍 −")};
    for (std::size_t index = 0; index < ptz_actions_.size(); ++index)
    {
        auto* button = new QPushButton(labels[index], ptz);
        button->setObjectName(QStringLiteral("ptz_%1").arg(commands[index]));
        button->setToolTip(QStringLiteral("平台最多等待 300 毫秒，随后独立尝试 STOP；网络中断时不保证物理停止"));
        ptz_actions_[index] = button;
        const auto position = static_cast<int>(index < 4 ? index : index + 1);
        if (index < 8)
        {
            directions->addWidget(button, position / 3, position % 3);
        }
        connect(button, &QPushButton::clicked, this, [this, command = commands[index]]() { control_ptz(command); });
    }
    ptz_stop_->setObjectName(QStringLiteral("ptz_stop"));
    directions->addWidget(ptz_stop_, 1, 1);
    ptz_layout->addLayout(directions);
    auto* zoom = new QHBoxLayout;
    zoom->addWidget(ptz_actions_[8]);
    zoom->addWidget(ptz_actions_[9]);
    ptz_layout->addLayout(zoom);
    auto* speeds = new QHBoxLayout;
    ptz_speed_->setObjectName(QStringLiteral("ptz_speed"));
    ptz_speed_->setRange(1, 255);
    ptz_speed_->setValue(64);
    ptz_zoom_speed_->setObjectName(QStringLiteral("ptz_zoom_speed"));
    ptz_zoom_speed_->setRange(1, 15);
    ptz_zoom_speed_->setValue(4);
    speeds->addWidget(new QLabel(QStringLiteral("方向速度"), ptz));
    speeds->addWidget(ptz_speed_);
    speeds->addWidget(new QLabel(QStringLiteral("变倍速度"), ptz));
    speeds->addWidget(ptz_zoom_speed_);
    ptz_layout->addLayout(speeds);
    ptz_result_->setObjectName(QStringLiteral("ptz_result"));
    ptz_result_->setTextFormat(Qt::PlainText);
    ptz_result_->setWordWrap(true);
    ptz_layout->addWidget(ptz_result_);
    controls->addTab(ptz, QStringLiteral("云台"));
    auto* presets = new QWidget(controls);
    auto* preset_layout = new QVBoxLayout(presets);
    preset_layout->setContentsMargins(4, 4, 4, 4);
    preset_query_->setObjectName(QStringLiteral("preset_query"));
    preset_layout->addWidget(preset_query_);
    preset_list_->setObjectName(QStringLiteral("preset_list"));
    preset_list_->setHeaderLabels({QStringLiteral("编号"), QStringLiteral("名称")});
    preset_list_->setRootIsDecorated(false);
    preset_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    preset_layout->addWidget(preset_list_, 1);
    auto* preset_actions = new QHBoxLayout;
    preset_number_->setObjectName(QStringLiteral("preset_number"));
    preset_number_->setRange(1, 255);
    preset_number_->setValue(1);
    preset_number_->setToolTip(QStringLiteral("设置当前姿态为此编号；调用、删除使用列表选中编号"));
    preset_set_->setObjectName(QStringLiteral("preset_set"));
    preset_goto_->setObjectName(QStringLiteral("preset_goto"));
    preset_remove_->setObjectName(QStringLiteral("preset_remove"));
    preset_goto_->setToolTip(QStringLiteral("仅可调用列表中规范十进制 1–255 的编号；受理不等于到位"));
    preset_remove_->setToolTip(QStringLiteral("仅可删除列表中规范十进制 1–255 的编号"));
    preset_actions->addWidget(preset_number_);
    preset_actions->addWidget(preset_set_);
    preset_actions->addWidget(preset_goto_);
    preset_actions->addWidget(preset_remove_);
    preset_layout->addLayout(preset_actions);
    preset_result_->setObjectName(QStringLiteral("preset_result"));
    preset_result_->setTextFormat(Qt::PlainText);
    preset_result_->setWordWrap(true);
    preset_layout->addWidget(preset_result_);
    controls->addTab(presets, QStringLiteral("预置位"));
    auto* alarms = new QWidget(controls);
    auto* alarm_layout = new QVBoxLayout(alarms);
    alarm_layout->setContentsMargins(4, 4, 4, 4);
    alarm_list_->setObjectName(QStringLiteral("alarm_list"));
    alarm_list_->setHeaderLabels({QStringLiteral("报警时间"),
                                  QStringLiteral("设备"),
                                  QStringLiteral("报警源"),
                                  QStringLiteral("级别"),
                                  QStringLiteral("方式"),
                                  QStringLiteral("描述")});
    alarm_list_->setRootIsDecorated(false);
    alarm_layout->addWidget(alarm_list_, 1);
    alarm_status_->setObjectName(QStringLiteral("alarm_status"));
    alarm_status_->setTextFormat(Qt::PlainText);
    alarm_status_->setWordWrap(true);
    alarm_status_->setText(QStringLiteral("显示设备上报的近期报警；平台只在内存保留最近 1000 条，重启后清空"));
    alarm_layout->addWidget(alarm_status_);
    const auto alarm_index = controls->addTab(alarms, QStringLiteral("报警"));
    // 只在报警页可见时轮询；切回时按游标补读平台保留的近期报警。
    alarm_timer_->setInterval(2000);
    connect(alarm_timer_, &QTimer::timeout, this, &monitor_window::poll_alarms);
    connect(controls,
            &QTabWidget::currentChanged,
            this,
            [this, alarm_index](int index)
            {
                if (index != alarm_index)
                {
                    alarm_timer_->stop();
                    return;
                }
                alarm_timer_->start();
                poll_alarms();
            });
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
    record_pause_->setObjectName(QStringLiteral("record_pause"));
    record_live_->setObjectName(QStringLiteral("record_live"));
    auto* stop = new QPushButton(QStringLiteral("停止预览"), central);
    stop->setObjectName(QStringLiteral("stop_preview"));
    auto* stop_everything = new QPushButton(QStringLiteral("停止全部"), central);
    stop_everything->setObjectName(QStringLiteral("stop_all"));
    bottom->addWidget(view_layout_);
    bottom->addWidget(aspect);
    bottom->addWidget(screenshot);
    bottom->addWidget(fullscreen);
    bottom->addWidget(sound_);
    bottom->addWidget(record_pause_);
    bottom->addWidget(record_live_);
    bottom->addWidget(stop);
    bottom->addWidget(stop_everything);
    layout->addLayout(bottom);
    auto* record_controls = new QHBoxLayout;
    record_position_->setObjectName(QStringLiteral("record_position"));
    record_position_->setDecimals(3);
    record_position_->setSuffix(QStringLiteral(" 秒"));
    record_position_->setToolTip(QStringLiteral("相对原录像范围起点的请求秒数；输入后点击请求定位，不代表实际命中位置"));
    record_seek_->setObjectName(QStringLiteral("record_seek"));
    record_seek_->setToolTip(QStringLiteral("同会话请求设备定位；设备受理不代表精确命中，暂停时须先继续回放"));
    record_range_->setObjectName(QStringLiteral("record_range"));
    record_controls->addWidget(record_range_, 1);
    record_controls->addWidget(new QLabel(QStringLiteral("相对起点"), central));
    record_controls->addWidget(record_position_);
    record_controls->addWidget(record_seek_);
    layout->addLayout(record_controls);
    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("Linux x86_64 · H.264 · RTSP/TCP · 默认静音"));
    connect(refresh, &QPushButton::clicked, this, &monitor_window::reload_devices);
    connect(address_, &QLineEdit::returnPressed, this, &monitor_window::reload_devices);
    connect(devices_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { play_item(item); });
    connect(catalog_, &QPushButton::clicked, this, &monitor_window::query_catalog);
    connect(record_query_, &QPushButton::clicked, this, &monitor_window::open_record_query);
    connect(devices_, &QTreeWidget::currentItemChanged, this, [this]() { reset_device_query(); });
    connect(device_info_, &QPushButton::clicked, this, [this]() { query_device(QStringLiteral("DeviceInfo")); });
    connect(device_status_, &QPushButton::clicked, this, [this]() { query_device(QStringLiteral("DeviceStatus")); });
    connect(preset_query_, &QPushButton::clicked, this, &monitor_window::query_presets);
    connect(preset_set_, &QPushButton::clicked, this, [this]() { control_preset(QStringLiteral("preset_set")); });
    connect(preset_goto_, &QPushButton::clicked, this, [this]() { control_preset(QStringLiteral("preset_goto")); });
    connect(preset_remove_, &QPushButton::clicked, this, [this]() { control_preset(QStringLiteral("preset_remove")); });
    connect(preset_list_,
            &QTreeWidget::currentItemChanged,
            this,
            [this]()
            {
                const bool enabled = preset_list_->isEnabled() && selected_preset_id() != 0;
                preset_goto_->setEnabled(enabled);
                preset_remove_->setEnabled(enabled);
            });
    connect(ptz_stop_, &QPushButton::clicked, this, [this]() { control_ptz(QStringLiteral("stop")); });
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
    connect(record_pause_, &QPushButton::clicked, this, [this]() { selected_->set_record_paused(!selected_->record_paused()); });
    connect(record_seek_, &QPushButton::clicked, this, [this]() { selected_->seek_record(record_position_->value()); });
    connect(record_live_,
            &QPushButton::clicked,
            this,
            [this]()
            {
                if (selected_->recording())
                {
                    selected_->stop_recording();
                    return;
                }
                const auto directory = record_directory();
                if (!QDir().mkpath(directory))
                {
                    statusBar()->showMessage(QStringLiteral("无法创建录像目录：%1").arg(directory));
                    return;
                }
                selected_->start_recording(client_, directory);
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
    if (record_dialog_)
    {
        record_dialog_->close();
    }
    stop_all();
    // 基类析构删除子画面时 stop() 仍会发信号，此时本窗口已不完整；先断开画面到本窗口的回调。
    for (auto* view : views_)
    {
        disconnect(view, nullptr, this, nullptr);
    }
    if (audio_id_ != 0)
    {
        SDL_CloseAudioDevice(audio_id_);
    }
}

void monitor_window::select_view(playback_view* view)
{
    if (selected_ != view)
    {
        record_position_->setValue(0);
    }
    selected_ = view;
    for (auto* item : views_)
    {
        item->setProperty("selected", item == view);
        item->update();
    }
    const QSignalBlocker blocked(sound_);
    sound_->setChecked(view->audible());
    sound_->setText(view->audible() ? QStringLiteral("关闭声音") : QStringLiteral("开启声音"));
    update_view_controls();
}

void monitor_window::update_view_controls()
{
    record_live_->setEnabled(selected_->recording() || selected_->recording_ready());
    record_live_->setText(selected_->recording() ? QStringLiteral("停止录制") : QStringLiteral("开始录制"));
    record_pause_->setEnabled(selected_->record_control_ready());
    record_pause_->setText(selected_->record_paused() ? QStringLiteral("继续回放") : QStringLiteral("暂停回放"));
    const auto start = selected_->record_start();
    const auto end = selected_->record_end();
    const bool has_range = start > 0 && end > start;
    record_position_->setRange(0, has_range ? static_cast<double>(end - start) : 0);
    const bool seek_ready = selected_->record_seek_ready();
    record_position_->setEnabled(seek_ready);
    record_seek_->setEnabled(seek_ready);
    record_range_->setText(has_range ? QStringLiteral("原录像范围：%1 — %2（UTC）")
                                           .arg(QDateTime::fromSecsSinceEpoch(start, Qt::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                                QDateTime::fromSecsSinceEpoch(end, Qt::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                                     : QStringLiteral("未选择录像回放范围"));
}

void monitor_window::poll_alarms()
{
    if (!listing_ || alarm_reply_)
    {
        return;
    }
    alarm_reply_ = client_.request(
        "GET",
        QStringLiteral("/api/alarms/since/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(alarm_cursor_))),
        {},
        listing_,
        [this](platform_response response)
        {
            alarm_reply_.clear();
            const auto result = response.document.object();
            const auto alarms = result.value(QStringLiteral("alarms"));
            const auto next = result.value(QStringLiteral("next_cursor")).toString();
            if (!response.ok() || !alarms.isArray() || next.isEmpty())
            {
                alarm_status_->setText(
                    QStringLiteral("报警查询失败：%1；稍后自动再查").arg(response.ok() ? QStringLiteral("响应无效") : response.error));
                return;
            }
            // 游标包含平台实例身份，重启后即使序号已追平，也按新实例的记录重建。
            if (result.value(QStringLiteral("reset")).toBool())
            {
                alarm_list_->clear();
            }
            for (const auto& value : alarms.toArray())
            {
                const auto alarm = value.toObject();
                auto* item = new QTreeWidgetItem({alarm.value(QStringLiteral("alarm_time")).toString(),
                                                  alarm.value(QStringLiteral("device_id")).toString(),
                                                  alarm.value(QStringLiteral("source_id")).toString(),
                                                  alarm.value(QStringLiteral("priority")).toString(),
                                                  alarm.value(QStringLiteral("method")).toString(),
                                                  alarm.value(QStringLiteral("description")).toString()});
                alarm_list_->insertTopLevelItem(0, item);
            }
            alarm_cursor_ = next;
            while (alarm_list_->topLevelItemCount() > 500)
            {
                delete alarm_list_->takeTopLevelItem(alarm_list_->topLevelItemCount() - 1);
            }
            alarm_status_->setText(result.value(QStringLiteral("gap")).toBool()
                                       ? QStringLiteral("部分未读报警已被平台容量淘汰；当前显示 %1 条").arg(alarm_list_->topLevelItemCount())
                                       : QStringLiteral("报警 %1 条（最多显示 500 条）").arg(alarm_list_->topLevelItemCount()));
        },
        5'000);
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

void monitor_window::open_record_query()
{
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    if (!listing_ || data.value(QStringLiteral("kind")).toString() != QStringLiteral("gb_channel"))
    {
        return;
    }
    if (record_dialog_)
    {
        record_dialog_->close();
    }
    record_dialog_ =
        new record_query_dialog(client_, data.value(QStringLiteral("id")).toString(), data.value(QStringLiteral("channel_id")).toString(), this);
    record_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(listing_, &QObject::destroyed, record_dialog_, &QDialog::reject);
    connect(record_dialog_,
            &record_query_dialog::playback_requested,
            listing_,
            [this, data, dialog = record_dialog_](qint64 start_time, qint64 end_time)
            {
                if (selected_ && record_dialog_ == dialog && dialog->isVisible())
                {
                    selected_->play_record(client_,
                                           {play_target::kind::gb_channel,
                                            data.value(QStringLiteral("id")).toString(),
                                            data.value(QStringLiteral("channel_id")).toString(),
                                            data.value(QStringLiteral("name")).toString()},
                                           start_time,
                                           end_time);
                }
            });
    record_dialog_->show();
}

void monitor_window::reset_device_query()
{
    delete control_context_;
    control_context_ = nullptr;
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    const auto kind = data.value(QStringLiteral("kind")).toString();
    record_query_->setEnabled(listing_ && kind == QStringLiteral("gb_channel"));
    set_control_busy(false);
    const auto id = data.value(QStringLiteral("id")).toString();
    device_target_->setText(kind == QStringLiteral("gb_channel")  ? id + QLatin1Char('\n') + data.value(QStringLiteral("channel_id")).toString()
                            : kind == QStringLiteral("gb_device") ? id
                                                                  : QStringLiteral("请选择国标设备或通道"));
    device_result_->clear();
    preset_list_->clear();
    preset_result_->setText(kind == QStringLiteral("gb_channel") ? QStringLiteral("请查询当前通道的预置位") : QStringLiteral("请选择国标通道"));
    ptz_result_->setText(kind == QStringLiteral("gb_channel") ? QStringLiteral("点动等待上限 300 毫秒；物理结果需现场确认")
                                                              : QStringLiteral("请选择国标通道"));
}

void monitor_window::set_control_busy(bool busy)
{
    const auto* item = devices_->currentItem();
    const auto kind = item ? item->data(0, Qt::UserRole).toMap().value(QStringLiteral("kind")).toString() : QString{};
    const bool channel = listing_ && kind == QStringLiteral("gb_channel");
    const bool device = listing_ && (channel || kind == QStringLiteral("gb_device"));
    talk_button_->setEnabled(talk_ || device);
    device_info_->setEnabled(device && !busy);
    device_status_->setEnabled(device && !busy);
    preset_query_->setEnabled(channel && !busy);
    preset_list_->setEnabled(channel && !busy);
    preset_number_->setEnabled(channel && !busy);
    preset_set_->setEnabled(channel && !busy);
    preset_goto_->setEnabled(channel && !busy && selected_preset_id() != 0);
    preset_remove_->setEnabled(channel && !busy && selected_preset_id() != 0);
    for (auto* button : ptz_actions_)
    {
        button->setEnabled(channel && !busy);
    }
    ptz_speed_->setEnabled(channel && !busy);
    ptz_zoom_speed_->setEnabled(channel && !busy);
    ptz_stop_->setEnabled(channel);
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
    set_control_busy(true);
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
            set_control_busy(false);
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

void monitor_window::query_presets()
{
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    if (!listing_ || data.value(QStringLiteral("kind")).toString() != QStringLiteral("gb_channel"))
    {
        return;
    }
    const auto id = data.value(QStringLiteral("id")).toString();
    const auto channel = data.value(QStringLiteral("channel_id")).toString();
    delete control_context_;
    control_context_ = new QObject(listing_);
    set_control_busy(true);
    preset_list_->clear();
    preset_result_->setText(QStringLiteral("等待通道 %1 返回预置位…").arg(channel));
    const auto path = QStringLiteral("/api/devices/%1/channels/%2/presets/query")
                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), QString::fromLatin1(QUrl::toPercentEncoding(channel)));
    client_.request(
        "POST",
        path,
        {},
        control_context_,
        [this, id, channel](platform_response response)
        {
            set_control_busy(false);
            if (!response.ok())
            {
                preset_result_->setText(QStringLiteral("预置位查询失败：%1").arg(response.error));
                return;
            }
            const auto result = response.document.object();
            const auto presets = result.value(QStringLiteral("presets"));
            if (result.value(QStringLiteral("device_id")).toString() != id || result.value(QStringLiteral("channel_id")).toString() != channel ||
                result.value(QStringLiteral("command")).toString() != QStringLiteral("PresetQuery") || !presets.isArray())
            {
                preset_result_->setText(QStringLiteral("预置位查询失败：平台返回的通道结果无效"));
                return;
            }
            QSet<QString> identifiers;
            for (const auto& value : presets.toArray())
            {
                const auto preset = value.toObject();
                const auto identifier = preset.value(QStringLiteral("preset_id"));
                const auto name = preset.value(QStringLiteral("preset_name"));
                if (!value.isObject() || !identifier.isString() || identifier.toString().isEmpty() || !name.isString() ||
                    identifiers.contains(identifier.toString()))
                {
                    preset_list_->clear();
                    preset_result_->setText(QStringLiteral("预置位查询失败：平台返回的预置位列表无效"));
                    return;
                }
                identifiers.insert(identifier.toString());
                new QTreeWidgetItem(preset_list_, {identifier.toString(), name.toString()});
            }
            preset_result_->setText(identifiers.isEmpty() ? QStringLiteral("通道 %1 没有预置位").arg(channel)
                                                          : QStringLiteral("通道 %1 返回 %2 个预置位").arg(channel).arg(identifiers.size()));
        });
}

int monitor_window::selected_preset_id() const
{
    const auto* item = preset_list_->currentItem();
    if (!item)
    {
        return 0;
    }
    const auto text = item->text(0);
    bool valid = false;
    const auto number = text.toInt(&valid);
    return valid && number >= 1 && number <= 255 && QString::number(number) == text ? number : 0;
}

void monitor_window::control_preset(const QString& command)
{
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    const auto number = command == QStringLiteral("preset_set") ? preset_number_->value() : selected_preset_id();
    if (!listing_ || data.value(QStringLiteral("kind")).toString() != QStringLiteral("gb_channel") || number == 0)
    {
        return;
    }
    const auto id = data.value(QStringLiteral("id")).toString();
    const auto channel = data.value(QStringLiteral("channel_id")).toString();
    delete control_context_;
    control_context_ = new QObject(listing_);
    const auto context = QPointer<QObject>(control_context_);
    set_control_busy(true);
    device_result_->clear();
    ptz_result_->clear();
    auto send = [this, id, channel, command, number, context]()
    {
        if (!context)
        {
            return;
        }
        if (command != QStringLiteral("preset_goto"))
        {
            preset_list_->clear();
        }
        preset_result_->setText(QStringLiteral("正在请求预置位控制，物理结果未确认…"));
        const auto path = QStringLiteral("/api/devices/%1/channels/%2/ptz")
                              .arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), QString::fromLatin1(QUrl::toPercentEncoding(channel)));
        client_.request("POST",
                        path,
                        {{QStringLiteral("command"), command}, {QStringLiteral("preset_id"), number}},
                        context.data(),
                        [this, id, channel, command, number](platform_response response)
                        {
                            set_control_busy(false);
                            if (!response.ok())
                            {
                                preset_result_->setText(QStringLiteral("预置位请求失败：%1；物理结果未确认").arg(response.error));
                                return;
                            }
                            const auto result = response.document.object();
                            const auto accepted = result.value(QStringLiteral("accepted"));
                            const auto preset = result.value(QStringLiteral("preset_id"));
                            if (response.status != 200 || result.value(QStringLiteral("device_id")).toString() != id ||
                                result.value(QStringLiteral("channel_id")).toString() != channel ||
                                result.value(QStringLiteral("command")).toString() != command || !preset.isDouble() || preset.toDouble() != number ||
                                !accepted.isBool() || !accepted.toBool() ||
                                result.value(QStringLiteral("physical_result")).toString() != QStringLiteral("unconfirmed"))
                            {
                                preset_result_->setText(QStringLiteral("预置位请求失败：平台返回的控制结果无效；物理结果未确认"));
                                return;
                            }
                            preset_result_->setText(command == QStringLiteral("preset_goto")
                                                        ? QStringLiteral("调用预置位 %1 已受理，实际到位未确认；需要时请显式 STOP").arg(number)
                                                        : QStringLiteral("预置位 %1 控制已受理，物理结果未确认；请手动查询列表确认").arg(number));
                        });
    };
    if (command == QStringLiteral("preset_goto"))
    {
        send();
        return;
    }
    preset_result_->setText(QStringLiteral("等待确认，尚未发送预置位指令"));
    auto* confirmation =
        new QMessageBox(QMessageBox::Warning,
                        QStringLiteral("确认预置位操作"),
                        QStringLiteral("%1预置位 %2？\n父设备：%3\n通道：%4\n设置可能覆盖原位置，删除不可回滚；SIP 受理不等于设备生效。")
                            .arg(command == QStringLiteral("preset_set") ? QStringLiteral("设置") : QStringLiteral("删除"))
                            .arg(number)
                            .arg(id, channel),
                        QMessageBox::Yes | QMessageBox::No,
                        this);
    confirmation->setObjectName(QStringLiteral("preset_confirm"));
    confirmation->setTextFormat(Qt::PlainText);
    confirmation->setDefaultButton(QMessageBox::No);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setWindowModality(Qt::NonModal);
    connect(context.data(), &QObject::destroyed, confirmation, &QMessageBox::reject);
    connect(confirmation,
            &QMessageBox::finished,
            context.data(),
            [this, context, send = std::move(send)](int result)
            {
                if (!context)
                {
                    return;
                }
                if (result == QMessageBox::Yes)
                {
                    send();
                    return;
                }
                set_control_busy(false);
                preset_result_->setText(QStringLiteral("已取消，未发送预置位指令"));
            });
    confirmation->show();
}

void monitor_window::control_ptz(const QString& command)
{
    const auto* item = devices_->currentItem();
    const auto data = item ? item->data(0, Qt::UserRole).toMap() : QVariantMap{};
    if (!listing_ || data.value(QStringLiteral("kind")).toString() != QStringLiteral("gb_channel"))
    {
        return;
    }
    const auto id = data.value(QStringLiteral("id")).toString();
    const auto channel = data.value(QStringLiteral("channel_id")).toString();
    delete control_context_;
    control_context_ = new QObject(listing_);
    set_control_busy(true);
    device_result_->clear();
    preset_result_->clear();
    ptz_result_->setText(command == QStringLiteral("stop") ? QStringLiteral("正在请求停止，结果未确认…")
                                                           : QStringLiteral("正在请求点动及 STOP，结果未确认…"));
    QJsonObject body{{QStringLiteral("command"), command}};
    if (command != QStringLiteral("stop"))
    {
        body.insert(QStringLiteral("speed"), command.startsWith(QStringLiteral("zoom_")) ? ptz_zoom_speed_->value() : ptz_speed_->value());
    }
    const auto path = QStringLiteral("/api/devices/%1/channels/%2/ptz")
                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), QString::fromLatin1(QUrl::toPercentEncoding(channel)));
    client_.request(
        "POST",
        path,
        body,
        control_context_,
        [this, id, channel, command](platform_response response)
        {
            set_control_busy(false);
            if (!response.ok())
            {
                if (response.status == 502 && response.document.object().value(QStringLiteral("error")) == QStringLiteral("ptz_start_unconfirmed"))
                {
                    ptz_result_->setText(QStringLiteral("云台动作受理未确认；STOP 已受理，物理结果未确认"));
                    return;
                }
                ptz_result_->setText(QStringLiteral("云台请求失败：%1；物理结果未确认").arg(response.error));
                return;
            }
            const auto result = response.document.object();
            const bool requested = response.status == 202 && command == QStringLiteral("stop") &&
                                   result.value(QStringLiteral("stop")).toString() == QStringLiteral("requested");
            const bool accepted =
                response.status == 200 && result.value(QStringLiteral("stop")).toString() == QStringLiteral("accepted") &&
                (command == QStringLiteral("stop") || result.value(QStringLiteral("start")).toString() == QStringLiteral("accepted"));
            if (result.value(QStringLiteral("device_id")).toString() != id || result.value(QStringLiteral("channel_id")).toString() != channel ||
                result.value(QStringLiteral("command")).toString() != command ||
                result.value(QStringLiteral("physical_result")).toString() != QStringLiteral("unconfirmed") || (!requested && !accepted))
            {
                ptz_result_->setText(QStringLiteral("云台请求失败：平台返回的控制结果无效；物理结果未确认"));
                return;
            }
            ptz_result_->setText(requested                           ? QStringLiteral("已请求停止，结果未确认；最终 STOP 结果请查看平台日志")
                                 : command == QStringLiteral("stop") ? QStringLiteral("STOP 已受理，物理结果未确认")
                                                                     : QStringLiteral("点动与 STOP 均已受理，物理结果未确认"));
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
        if (talk_)
        {
            talk_->stop();
        }
        stop_all();
    }
    QSettings().setValue(QStringLiteral("platform_url"), base.toString());
    delete listing_;
    control_context_ = nullptr;
    listing_ = new QObject(this);
    if (alarm_reply_)
    {
        alarm_reply_->abort();
        alarm_reply_.clear();
    }
    alarm_cursor_ = QStringLiteral("0");
    alarm_list_->clear();
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
    if (talk_)
    {
        talk_->stop();
        event->ignore();
        setEnabled(false);
        statusBar()->showMessage(QStringLiteral("正在关闭设备对讲…"));
        QTimer::singleShot(30, this, &QWidget::close);
        return;
    }
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
    // stop_all() 已让实时录制收尾保存，关闭录像窗口已取消下载；等它们写完或删除临时文件再退出。
    for (auto* recorder : qApp->findChildren<record_thread*>())
    {
        if (recorder->isRunning())
        {
            event->ignore();
            setEnabled(false);
            statusBar()->showMessage(QStringLiteral("正在保存或清理录像文件…"));
            QTimer::singleShot(30, this, &QWidget::close);
            return;
        }
    }
    QSettings().setValue(QStringLiteral("geometry"), isFullScreen() ? normalGeometry() : geometry());
    event->accept();
}

}    // namespace media_server
