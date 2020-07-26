﻿#if _MSC_VER >= 1600
#pragma execution_character_set("utf-8")
#endif

// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <clocale>
#include <memory>
#include <thread>
#ifdef __APPLE__
#include <unistd.h> // for chdir
#endif

// VFS includes must be before glad as they will conflict with Windows file api, which uses defines.
#include "applets/error.h"
#include "applets/profile_select.h"
#include "applets/software_keyboard.h"
#include "applets/web_browser.h"
#include "configuration/configure_input.h"
#include "configuration/configure_per_game.h"
#include "core/file_sys/vfs.h"
#include "core/file_sys/vfs_real.h"
#include "core/frontend/applets/general_frontend.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_ae.h"
#include "core/hle/service/am/applet_oe.h"
#include "core/hle/service/am/applets/applets.h"
#include "core/hle/service/hid/controllers/npad.h"
#include "core/hle/service/hid/hid.h"

// These are wrappers to avoid the calls to CreateDirectory and CreateFile because of the Windows
// defines.
static FileSys::VirtualDir VfsFilesystemCreateDirectoryWrapper(
    const FileSys::VirtualFilesystem& vfs, const std::string& path, FileSys::Mode mode) {
    return vfs->CreateDirectory(path, mode);
}

static FileSys::VirtualFile VfsDirectoryCreateFileWrapper(const FileSys::VirtualDir& dir,
                                                          const std::string& path) {
    return dir->CreateFile(path);
}

#include <fmt/ostream.h>
#include <glad/glad.h>

#define QT_NO_OPENGL
#include <QClipboard>
#include <QDesktopServices>
#include <QDesktopWidget>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QShortcut>
#include <QStatusBar>
#include <QSysInfo>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>

#include <fmt/format.h>
#include "common/common_paths.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#ifdef ARCHITECTURE_x86_64
#include "common/x64/cpu_detect.h"
#endif
#include "common/telemetry.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/submission_package.h"
#include "core/frontend/applets/software_keyboard.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/nfp/nfp.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "core/settings.h"
#include "core/telemetry_session.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"
#include "yuzu/about_dialog.h"
#include "yuzu/bootmanager.h"
#include "yuzu/compatdb.h"
#include "yuzu/compatibility_list.h"
#include "yuzu/configuration/config.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/debugger/console.h"
#include "yuzu/debugger/profiler.h"
#include "yuzu/debugger/wait_tree.h"
#include "yuzu/discord.h"
#include "yuzu/game_list.h"
#include "yuzu/game_list_p.h"
#include "yuzu/hotkeys.h"
#include "yuzu/install_dialog.h"
#include "yuzu/loading_screen.h"
#include "yuzu/main.h"
#include "yuzu/uisettings.h"

#ifdef USE_DISCORD_PRESENCE
#include "yuzu/discord_impl.h"
#endif

#ifdef YUZU_USE_QT_WEB_ENGINE
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#endif

#ifdef QT_STATICPLUGIN
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#endif

#ifdef _WIN32
#include <windows.h>
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

constexpr int default_mouse_timeout = 2500;

constexpr u64 DLC_BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

/**
 * "Callouts" are one-time instructional messages shown to the user. In the config settings, there
 * is a bitfield "callout_flags" options, used to track if a message has already been shown to the
 * user. This is 32-bits - if we have more than 32 callouts, we should retire and recyle old ones.
 */
enum class CalloutFlag : uint32_t {
    Telemetry = 0x1,
    DRDDeprecation = 0x2,
};

void GMainWindow::ShowTelemetryCallout() {
    if (UISettings::values.callout_flags & static_cast<uint32_t>(CalloutFlag::Telemetry)) {
        return;
    }

    UISettings::values.callout_flags |= static_cast<uint32_t>(CalloutFlag::Telemetry);
    const QString telemetry_message =
        tr("<a href='https://yuzu-emu.org/help/feature/telemetry/'>匿名 "
           "收集数据</a> 以帮助改善. "
           "<br/><br/>你想与我们分享您的使用情况的数据？");
    if (QMessageBox::question(this, tr("数据"), telemetry_message) != QMessageBox::Yes) {
        Settings::values.enable_telemetry = false;
        Settings::Apply();
    }
}

const int GMainWindow::max_recent_files_item;

static void InitializeLogging() {
    Log::Filter log_filter;
    log_filter.ParseFilterString(Settings::values.log_filter);
    Log::SetGlobalFilter(log_filter);

    const std::string& log_dir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    FileUtil::CreateFullPath(log_dir);
    Log::AddBackend(std::make_unique<Log::FileBackend>(log_dir + LOG_FILE));
#ifdef _WIN32
    Log::AddBackend(std::make_unique<Log::DebuggerBackend>());
#endif
}

GMainWindow::GMainWindow()
    : config(new Config()), emu_thread(nullptr),
      vfs(std::make_shared<FileSys::RealVfsFilesystem>()),
      provider(std::make_unique<FileSys::ManualContentProvider>()) {
    InitializeLogging();

    LoadTranslation();

    setAcceptDrops(true);
    ui.setupUi(this);
    statusBar()->hide();

    default_theme_paths = QIcon::themeSearchPaths();
    UpdateUITheme();

    SetDiscordEnabled(UISettings::values.enable_discord_presence);
    discord_rpc->Update();

    InitializeWidgets();
    InitializeDebugWidgets();
    InitializeRecentFileMenuActions();
    InitializeHotkeys();

    SetDefaultUIGeometry();
    RestoreUIState();

    ConnectMenuEvents();
    ConnectWidgetEvents();

    const auto build_id = std::string(Common::g_build_id);
    const auto fmt = std::string(Common::g_title_bar_format_idle);
    const auto yuzu_build_version =
        fmt::format(fmt.empty() ? "yuzu Early Access" : fmt, std::string{}, std::string{},
                    std::string{}, std::string{}, std::string{}, build_id);

    LOG_INFO(Frontend, "yuzu Version: {} | {}-{}", yuzu_build_version, Common::g_scm_branch,
             Common::g_scm_desc);
#ifdef ARCHITECTURE_x86_64
    const auto& caps = Common::GetCPUCaps();
    std::string cpu_string = caps.cpu_string;
    if (caps.avx || caps.avx2 || caps.avx512) {
        cpu_string += " | AVX";
        if (caps.avx512) {
            cpu_string += "512";
        } else if (caps.avx2) {
            cpu_string += '2';
        }
        if (caps.fma || caps.fma4) {
            cpu_string += " | FMA";
        }
    }
    LOG_INFO(Frontend, "Host CPU: {}", cpu_string);
#endif
    LOG_INFO(Frontend, "Host OS: {}", QSysInfo::prettyProductName().toStdString());
    LOG_INFO(Frontend, "Host RAM: {:.2f} GB",
             Common::GetMemInfo().TotalPhysicalMemory / 1024.0f / 1024 / 1024);
    LOG_INFO(Frontend, "Host Swap: {:.2f} GB",
             Common::GetMemInfo().TotalSwapMemory / 1024.0f / 1024 / 1024);
    UpdateWindowTitle();

    show();

    Core::System::GetInstance().SetContentProvider(
        std::make_unique<FileSys::ContentProviderUnion>());
    Core::System::GetInstance().RegisterContentProvider(
        FileSys::ContentProviderUnionSlot::FrontendManual, provider.get());
    Core::System::GetInstance().GetFileSystemController().CreateFactories(*vfs);

    // Gen keys if necessary
    OnReinitializeKeys(ReinitializeKeyBehavior::NoWarning);

    game_list->LoadCompatibilityList();
    game_list->PopulateAsync(UISettings::values.game_dirs);

    // Show one-time "callout" messages to the user
    ShowTelemetryCallout();

    // make sure menubar has the arrow cursor instead of inheriting from this
    ui.menubar->setCursor(QCursor());
    statusBar()->setCursor(QCursor());

    mouse_hide_timer.setInterval(default_mouse_timeout);
    connect(&mouse_hide_timer, &QTimer::timeout, this, &GMainWindow::HideMouseCursor);
    connect(ui.menubar, &QMenuBar::hovered, this, &GMainWindow::ShowMouseCursor);

    QStringList args = QApplication::arguments();
    if (args.length() >= 2) {
        BootGame(args[1]);
    }
}

GMainWindow::~GMainWindow() {
    // will get automatically deleted otherwise
    if (render_window->parent() == nullptr)
        delete render_window;
}

void GMainWindow::ProfileSelectorSelectProfile() {
    const Service::Account::ProfileManager manager;
    int index = 0;
    if (manager.GetUserCount() != 1) {
        QtProfileSelectionDialog dialog(this);
        dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                              Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
        dialog.setWindowModality(Qt::WindowModal);
        if (dialog.exec() == QDialog::Rejected) {
            emit ProfileSelectorFinishedSelection(std::nullopt);
            return;
        }
        index = dialog.GetIndex();
    }

    const auto uuid = manager.GetUser(static_cast<std::size_t>(index));
    if (!uuid.has_value()) {
        emit ProfileSelectorFinishedSelection(std::nullopt);
        return;
    }

    emit ProfileSelectorFinishedSelection(uuid);
}

void GMainWindow::SoftwareKeyboardGetText(
    const Core::Frontend::SoftwareKeyboardParameters& parameters) {
    QtSoftwareKeyboardDialog dialog(this, parameters);
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Rejected) {
        emit SoftwareKeyboardFinishedText(std::nullopt);
        return;
    }

    emit SoftwareKeyboardFinishedText(dialog.GetText());
}

void GMainWindow::SoftwareKeyboardInvokeCheckDialog(std::u16string error_message) {
    QMessageBox::warning(this, tr("文本检查失败"), QString::fromStdU16String(error_message));
    emit SoftwareKeyboardFinishedCheckDialog();
}

#ifdef YUZU_USE_QT_WEB_ENGINE

void GMainWindow::WebBrowserOpenPage(std::string_view filename, std::string_view additional_args) {
    NXInputWebEngineView web_browser_view(this);

    // Scope to contain the QProgressDialog for initialization
    {
        QProgressDialog progress(this);
        progress.setMinimumDuration(200);
        progress.setLabelText(tr("加载Web小型应用程序..."));
        progress.setRange(0, 4);
        progress.setValue(0);
        progress.show();

        auto future = QtConcurrent::run([this] { emit WebBrowserUnpackRomFS(); });

        while (!future.isFinished())
            QApplication::processEvents();

        progress.setValue(1);

        // Load the special shim script to handle input and exit.
        QWebEngineScript nx_shim;
        nx_shim.setSourceCode(GetNXShimInjectionScript());
        nx_shim.setWorldId(QWebEngineScript::MainWorld);
        nx_shim.setName(QStringLiteral("nx_inject.js"));
        nx_shim.setInjectionPoint(QWebEngineScript::DocumentCreation);
        nx_shim.setRunsOnSubFrames(true);
        web_browser_view.page()->profile()->scripts()->insert(nx_shim);

        web_browser_view.load(
            QUrl(QUrl::fromLocalFile(QString::fromStdString(std::string(filename))).toString() +
                 QString::fromStdString(std::string(additional_args))));

        progress.setValue(2);

        render_window->hide();
        web_browser_view.setFocus();

        const auto& layout = render_window->GetFramebufferLayout();
        web_browser_view.resize(layout.screen.GetWidth(), layout.screen.GetHeight());
        web_browser_view.move(layout.screen.left, layout.screen.top + menuBar()->height());
        web_browser_view.setZoomFactor(static_cast<qreal>(layout.screen.GetWidth()) /
                                       Layout::ScreenUndocked::Width);
        web_browser_view.settings()->setAttribute(
            QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

        web_browser_view.show();

        progress.setValue(3);

        QApplication::processEvents();

        progress.setValue(4);
    }

    bool finished = false;
    QAction* exit_action = new QAction(tr("退出Web小型应用程序"), this);
    connect(exit_action, &QAction::triggered, this, [&finished] { finished = true; });
    ui.menubar->addAction(exit_action);

    auto& npad =
        Core::System::GetInstance()
            .ServiceManager()
            .GetService<Service::HID::Hid>("hid")
            ->GetAppletResource()
            ->GetController<Service::HID::Controller_NPad>(Service::HID::HidController::NPad);

    const auto fire_js_keypress = [&web_browser_view](u32 key_code) {
        web_browser_view.page()->runJavaScript(
            QStringLiteral("document.dispatchEvent(new KeyboardEvent('keydown', {'key': %1}));")
                .arg(key_code));
    };

    QMessageBox::information(
        this, tr("关闭"),
        tr("退出Web应用程序，使用游戏提供的控件来选择退出，选择 "
           "'退出Web小型应用程序”菜单栏中的选项, 或按 'Enter' 键."));

    bool running_exit_check = false;
    while (!finished) {
        QApplication::processEvents();

        if (!running_exit_check) {
            web_browser_view.page()->runJavaScript(QStringLiteral("applet_done;"),
                                                   [&](const QVariant& res) {
                                                       running_exit_check = false;
                                                       if (res.toBool())
                                                           finished = true;
                                                   });
            running_exit_check = true;
        }

        const auto input = npad.GetAndResetPressState();
        for (std::size_t i = 0; i < Settings::NativeButton::NumButtons; ++i) {
            if ((input & (1 << i)) != 0) {
                LOG_DEBUG(Frontend, "firing input for button id={:02X}", i);
                web_browser_view.page()->runJavaScript(
                    QStringLiteral("yuzu_key_callbacks[%1]();").arg(i));
            }
        }

        if (input & 0x00888000)      // RStick Down | LStick Down | DPad Down
            fire_js_keypress(40);    // Down Arrow Key
        else if (input & 0x00444000) // RStick Right | LStick Right | DPad Right
            fire_js_keypress(39);    // Right Arrow Key
        else if (input & 0x00222000) // RStick Up | LStick Up | DPad Up
            fire_js_keypress(38);    // Up Arrow Key
        else if (input & 0x00111000) // RStick Left | LStick Left | DPad Left
            fire_js_keypress(37);    // Left Arrow Key
        else if (input & 0x00000001) // A Button
            fire_js_keypress(13);    // Enter Key
    }

    web_browser_view.hide();
    render_window->show();
    render_window->setFocus();
    ui.menubar->removeAction(exit_action);

    // Needed to update render window focus/show and remove menubar action
    QApplication::processEvents();
    emit WebBrowserFinishedBrowsing();
}

#else

void GMainWindow::WebBrowserOpenPage(std::string_view filename, std::string_view additional_args) {
    QMessageBox::warning(
        this, tr("Web小型应用程序"),
        tr("这yuzu的版本不支持 QtWebEngine 这意味着柚子不能 "
           "正常显示要求的游戏手册或网页."),
        QMessageBox::Ok, QMessageBox::Ok);

    LOG_INFO(Frontend,
             "(STUBBED) called - Missing QtWebEngine dependency needed to open website page at "
             "'{}' with arguments '{}'!",
             filename, additional_args);

    emit WebBrowserFinishedBrowsing();
}

#endif

void GMainWindow::InitializeWidgets() {
#ifdef YUZU_ENABLE_COMPATIBILITY_REPORTING
    ui.action_Report_Compatibility->setVisible(true);
#endif
    render_window = new GRenderWindow(this, emu_thread.get());
    render_window->hide();

    game_list = new GameList(vfs, provider.get(), this);
    ui.horizontalLayout->addWidget(game_list);

    game_list_placeholder = new GameListPlaceholder(this);
    ui.horizontalLayout->addWidget(game_list_placeholder);
    game_list_placeholder->setVisible(false);

    loading_screen = new LoadingScreen(this);
    loading_screen->hide();
    ui.horizontalLayout->addWidget(loading_screen);
    connect(loading_screen, &LoadingScreen::Hidden, [&] {
        loading_screen->Clear();
        if (emulation_running) {
            render_window->show();
            render_window->setFocus();
        }
    });

    // Create status bar
    message_label = new QLabel();
    // Configured separately for left alignment
    message_label->setFrameStyle(QFrame::NoFrame);
    message_label->setContentsMargins(4, 0, 4, 0);
    message_label->setAlignment(Qt::AlignLeft);
    statusBar()->addPermanentWidget(message_label, 1);

    shader_building_label = new QLabel();
    shader_building_label->setToolTip(tr("当前正在构建的着色器数量"));
    emu_speed_label = new QLabel();
    emu_speed_label->setToolTip(
        tr("目前模拟速度。值高或低于 100% "
           "表明模拟的运行速度低于交换机更快或更慢."));
    game_fps_label = new QLabel();
    game_fps_label->setToolTip(tr("多少帧每秒游戏目前显示. "
                                  "这将改变从游戏到游戏和现场场景."));
    emu_frametime_label = new QLabel();
    emu_frametime_label->setToolTip(
        tr("时间采取模拟开关框架，不计算框架限制或垂直刷新同步 "
           "对于全速仿真，这应该是最多 16.67 ms."));

    for (auto& label :
         {shader_building_label, emu_speed_label, game_fps_label, emu_frametime_label}) {
        label->setVisible(false);
        label->setFrameStyle(QFrame::NoFrame);
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label);
    }

    // Setup Dock button
    dock_status_button = new QPushButton();
    dock_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    dock_status_button->setFocusPolicy(Qt::NoFocus);
    connect(dock_status_button, &QPushButton::clicked, [&] {
        Settings::values.use_docked_mode = !Settings::values.use_docked_mode;
        dock_status_button->setChecked(Settings::values.use_docked_mode);
        OnDockedModeChanged(!Settings::values.use_docked_mode, Settings::values.use_docked_mode);
    });
    dock_status_button->setText(tr("主机模式"));
    dock_status_button->setCheckable(true);
    dock_status_button->setChecked(Settings::values.use_docked_mode);
    statusBar()->insertPermanentWidget(0, dock_status_button);

    // Setup ASync button
    async_status_button = new QPushButton();
    async_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    async_status_button->setFocusPolicy(Qt::NoFocus);
    connect(async_status_button, &QPushButton::clicked, [&] {
        if (emulation_running) {
            return;
        }
        bool is_async = !Settings::values.use_asynchronous_gpu_emulation.GetValue() ||
                        Settings::values.use_multi_core.GetValue();
        Settings::values.use_asynchronous_gpu_emulation.SetValue(is_async);
        async_status_button->setChecked(Settings::values.use_asynchronous_gpu_emulation.GetValue());
        Settings::Apply();
    });
    async_status_button->setText(tr("异步模式"));
    async_status_button->setCheckable(true);
    async_status_button->setChecked(Settings::values.use_asynchronous_gpu_emulation.GetValue());

    // Setup Multicore button
    multicore_status_button = new QPushButton();
    multicore_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    multicore_status_button->setFocusPolicy(Qt::NoFocus);
    connect(multicore_status_button, &QPushButton::clicked, [&] {
        if (emulation_running) {
            return;
        }
        Settings::values.use_multi_core.SetValue(!Settings::values.use_multi_core.GetValue());
        bool is_async = Settings::values.use_asynchronous_gpu_emulation.GetValue() ||
                        Settings::values.use_multi_core.GetValue();
        Settings::values.use_asynchronous_gpu_emulation.SetValue(is_async);
        async_status_button->setChecked(Settings::values.use_asynchronous_gpu_emulation.GetValue());
        multicore_status_button->setChecked(Settings::values.use_multi_core.GetValue());
        Settings::Apply();
    });
    multicore_status_button->setText(tr("多核运行"));
    multicore_status_button->setCheckable(true);
    multicore_status_button->setChecked(Settings::values.use_multi_core.GetValue());
    statusBar()->insertPermanentWidget(0, multicore_status_button);
    statusBar()->insertPermanentWidget(0, async_status_button);

    // Setup Renderer API button
    renderer_status_button = new QPushButton();
    renderer_status_button->setObjectName(QStringLiteral("RendererStatusBarButton"));
    renderer_status_button->setCheckable(true);
    renderer_status_button->setFocusPolicy(Qt::NoFocus);
    connect(renderer_status_button, &QPushButton::toggled, [=](bool checked) {
        renderer_status_button->setText(checked ? tr("VULKAN") : tr("OPENGL"));
    });
    renderer_status_button->toggle();

#ifndef HAS_VULKAN
    renderer_status_button->setChecked(false);
    renderer_status_button->setCheckable(false);
    renderer_status_button->setDisabled(true);
#else
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
    connect(renderer_status_button, &QPushButton::clicked, [=] {
        if (emulation_running) {
            return;
        }
        if (renderer_status_button->isChecked()) {
            Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
        } else {
            Settings::values.renderer_backend.SetValue(Settings::RendererBackend::OpenGL);
        }

        Settings::Apply();
    });
#endif // HAS_VULKAN
    statusBar()->insertPermanentWidget(0, renderer_status_button);

    statusBar()->setVisible(true);
    setStyleSheet(QStringLiteral("QStatusBar::item{border: none;}"));
}

void GMainWindow::InitializeDebugWidgets() {
    QMenu* debug_menu = ui.menu_View_Debugging;

#if MICROPROFILE_ENABLED
    microProfileDialog = new MicroProfileDialog(this);
    microProfileDialog->hide();
    debug_menu->addAction(microProfileDialog->toggleViewAction());
#endif

    waitTreeWidget = new WaitTreeWidget(this);
    addDockWidget(Qt::LeftDockWidgetArea, waitTreeWidget);
    waitTreeWidget->hide();
    debug_menu->addAction(waitTreeWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStopping);
}

void GMainWindow::InitializeRecentFileMenuActions() {
    for (int i = 0; i < max_recent_files_item; ++i) {
        actions_recent_files[i] = new QAction(this);
        actions_recent_files[i]->setVisible(false);
        connect(actions_recent_files[i], &QAction::triggered, this, &GMainWindow::OnMenuRecentFile);

        ui.menu_recent_files->addAction(actions_recent_files[i]);
    }
    ui.menu_recent_files->addSeparator();
    QAction* action_clear_recent_files = new QAction(this);
    action_clear_recent_files->setText(tr("清除最近打开的文件记录"));
    connect(action_clear_recent_files, &QAction::triggered, this, [this] {
        UISettings::values.recent_files.clear();
        UpdateRecentFiles();
    });
    ui.menu_recent_files->addAction(action_clear_recent_files);

    UpdateRecentFiles();
}

void GMainWindow::InitializeHotkeys() {
    hotkey_registry.LoadHotkeys();

    const QString main_window = QStringLiteral("Main Window");
    const QString load_file = QStringLiteral("Load File");
    const QString load_amiibo = QStringLiteral("Load Amiibo");
    const QString exit_yuzu = QStringLiteral("Exit yuzu");
    const QString restart_emulation = QStringLiteral("Restart Emulation");
    const QString stop_emulation = QStringLiteral("Stop Emulation");
    const QString toggle_filter_bar = QStringLiteral("Toggle Filter Bar");
    const QString toggle_status_bar = QStringLiteral("Toggle Status Bar");
    const QString fullscreen = QStringLiteral("Fullscreen");
    const QString capture_screenshot = QStringLiteral("Capture Screenshot");

    ui.action_Load_File->setShortcut(hotkey_registry.GetKeySequence(main_window, load_file));
    ui.action_Load_File->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, load_file));

    ui.action_Load_Amiibo->setShortcut(hotkey_registry.GetKeySequence(main_window, load_amiibo));
    ui.action_Load_Amiibo->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, load_amiibo));

    ui.action_Exit->setShortcut(hotkey_registry.GetKeySequence(main_window, exit_yuzu));
    ui.action_Exit->setShortcutContext(hotkey_registry.GetShortcutContext(main_window, exit_yuzu));

    ui.action_Restart->setShortcut(hotkey_registry.GetKeySequence(main_window, restart_emulation));
    ui.action_Restart->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, restart_emulation));

    ui.action_Stop->setShortcut(hotkey_registry.GetKeySequence(main_window, stop_emulation));
    ui.action_Stop->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, stop_emulation));

    ui.action_Show_Filter_Bar->setShortcut(
        hotkey_registry.GetKeySequence(main_window, toggle_filter_bar));
    ui.action_Show_Filter_Bar->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, toggle_filter_bar));

    ui.action_Show_Status_Bar->setShortcut(
        hotkey_registry.GetKeySequence(main_window, toggle_status_bar));
    ui.action_Show_Status_Bar->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, toggle_status_bar));

    ui.action_Capture_Screenshot->setShortcut(
        hotkey_registry.GetKeySequence(main_window, capture_screenshot));
    ui.action_Capture_Screenshot->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, capture_screenshot));

    ui.action_Fullscreen->setShortcut(
        hotkey_registry.GetHotkey(main_window, fullscreen, this)->key());
    ui.action_Fullscreen->setShortcutContext(
        hotkey_registry.GetShortcutContext(main_window, fullscreen));

    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Load File"), this),
            &QShortcut::activated, this, &GMainWindow::OnMenuLoadFile);
    connect(
        hotkey_registry.GetHotkey(main_window, QStringLiteral("Continue/Pause Emulation"), this),
        &QShortcut::activated, this, [&] {
            if (emulation_running) {
                if (emu_thread->IsRunning()) {
                    OnPauseGame();
                } else {
                    OnStartGame();
                }
            }
        });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Restart Emulation"), this),
            &QShortcut::activated, this, [this] {
                if (!Core::System::GetInstance().IsPoweredOn()) {
                    return;
                }
                BootGame(game_path);
            });
    connect(hotkey_registry.GetHotkey(main_window, fullscreen, render_window),
            &QShortcut::activated, ui.action_Fullscreen, &QAction::trigger);
    connect(hotkey_registry.GetHotkey(main_window, fullscreen, render_window),
            &QShortcut::activatedAmbiguously, ui.action_Fullscreen, &QAction::trigger);
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Exit Fullscreen"), this),
            &QShortcut::activated, this, [&] {
                if (emulation_running) {
                    ui.action_Fullscreen->setChecked(false);
                    ToggleFullscreen();
                }
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Toggle Speed Limit"), this),
            &QShortcut::activated, this, [&] {
                Settings::values.use_frame_limit.SetValue(
                    !Settings::values.use_frame_limit.GetValue());
                UpdateStatusBar();
            });
    constexpr u16 SPEED_LIMIT_STEP = 5;
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Increase Speed Limit"), this),
            &QShortcut::activated, this, [&] {
                if (Settings::values.frame_limit.GetValue() < 9999 - SPEED_LIMIT_STEP) {
                    Settings::values.frame_limit.SetValue(SPEED_LIMIT_STEP +
                                                          Settings::values.frame_limit.GetValue());
                    UpdateStatusBar();
                }
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Decrease Speed Limit"), this),
            &QShortcut::activated, this, [&] {
                if (Settings::values.frame_limit.GetValue() > SPEED_LIMIT_STEP) {
                    Settings::values.frame_limit.SetValue(Settings::values.frame_limit.GetValue() -
                                                          SPEED_LIMIT_STEP);
                    UpdateStatusBar();
                }
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Load Amiibo"), this),
            &QShortcut::activated, this, [&] {
                if (ui.action_Load_Amiibo->isEnabled()) {
                    OnLoadAmiibo();
                }
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Capture Screenshot"), this),
            &QShortcut::activated, this, [&] {
                if (emu_thread != nullptr && emu_thread->IsRunning()) {
                    OnCaptureScreenshot();
                }
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Change Docked Mode"), this),
            &QShortcut::activated, this, [&] {
                Settings::values.use_docked_mode = !Settings::values.use_docked_mode;
                OnDockedModeChanged(!Settings::values.use_docked_mode,
                                    Settings::values.use_docked_mode);
                dock_status_button->setChecked(Settings::values.use_docked_mode);
            });
    connect(hotkey_registry.GetHotkey(main_window, QStringLiteral("Mute Audio"), this),
            &QShortcut::activated, this,
            [] { Settings::values.audio_muted = !Settings::values.audio_muted; });
}

void GMainWindow::SetDefaultUIGeometry() {
    // geometry: 53% of the window contents are in the upper screen half, 47% in the lower half
    const QRect screenRect = QApplication::desktop()->screenGeometry(this);

    const int w = screenRect.width() * 2 / 3;
    const int h = screenRect.height() * 2 / 3;
    const int x = (screenRect.x() + screenRect.width()) / 2 - w / 2;
    const int y = (screenRect.y() + screenRect.height()) / 2 - h * 53 / 100;

    setGeometry(x, y, w, h);
}

void GMainWindow::RestoreUIState() {
    restoreGeometry(UISettings::values.geometry);
    restoreState(UISettings::values.state);
    render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
#if MICROPROFILE_ENABLED
    microProfileDialog->restoreGeometry(UISettings::values.microprofile_geometry);
    microProfileDialog->setVisible(UISettings::values.microprofile_visible);
#endif

    game_list->LoadInterfaceLayout();

    ui.action_Single_Window_Mode->setChecked(UISettings::values.single_window_mode);
    ToggleWindowMode();

    ui.action_Fullscreen->setChecked(UISettings::values.fullscreen);

    ui.action_Display_Dock_Widget_Headers->setChecked(UISettings::values.display_titlebar);
    OnDisplayTitleBars(ui.action_Display_Dock_Widget_Headers->isChecked());

    ui.action_Show_Filter_Bar->setChecked(UISettings::values.show_filter_bar);
    game_list->setFilterVisible(ui.action_Show_Filter_Bar->isChecked());

    ui.action_Show_Status_Bar->setChecked(UISettings::values.show_status_bar);
    statusBar()->setVisible(ui.action_Show_Status_Bar->isChecked());
    Debugger::ToggleConsole();
}

void GMainWindow::OnAppFocusStateChanged(Qt::ApplicationState state) {
    if (!UISettings::values.pause_when_in_background) {
        return;
    }
    if (state != Qt::ApplicationHidden && state != Qt::ApplicationInactive &&
        state != Qt::ApplicationActive) {
        LOG_DEBUG(Frontend, "ApplicationState unusual flag: {} ", state);
    }
    if (ui.action_Pause->isEnabled() &&
        (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
        auto_paused = true;
        OnPauseGame();
    } else if (ui.action_Start->isEnabled() && auto_paused && state == Qt::ApplicationActive) {
        auto_paused = false;
        OnStartGame();
    }
}

void GMainWindow::ConnectWidgetEvents() {
    connect(game_list, &GameList::GameChosen, this, &GMainWindow::OnGameListLoadFile);
    connect(game_list, &GameList::OpenDirectory, this, &GMainWindow::OnGameListOpenDirectory);
    connect(game_list, &GameList::OpenFolderRequested, this, &GMainWindow::OnGameListOpenFolder);
    connect(game_list, &GameList::OpenTransferableShaderCacheRequested, this,
            &GMainWindow::OnTransferableShaderCacheOpenFile);
    connect(game_list, &GameList::RemoveInstalledEntryRequested, this,
            &GMainWindow::OnGameListRemoveInstalledEntry);
    connect(game_list, &GameList::RemoveFileRequested, this, &GMainWindow::OnGameListRemoveFile);
    connect(game_list, &GameList::DumpRomFSRequested, this, &GMainWindow::OnGameListDumpRomFS);
    connect(game_list, &GameList::CopyTIDRequested, this, &GMainWindow::OnGameListCopyTID);
    connect(game_list, &GameList::NavigateToGamedbEntryRequested, this,
            &GMainWindow::OnGameListNavigateToGamedbEntry);
    connect(game_list, &GameList::AddDirectory, this, &GMainWindow::OnGameListAddDirectory);
    connect(game_list_placeholder, &GameListPlaceholder::AddDirectory, this,
            &GMainWindow::OnGameListAddDirectory);
    connect(game_list, &GameList::ShowList, this, &GMainWindow::OnGameListShowList);

    connect(game_list, &GameList::OpenPerGameGeneralRequested, this,
            &GMainWindow::OnGameListOpenPerGameProperties);

    connect(this, &GMainWindow::UpdateInstallProgress, this,
            &GMainWindow::IncrementInstallProgress);

    connect(this, &GMainWindow::EmulationStarting, render_window,
            &GRenderWindow::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, render_window,
            &GRenderWindow::OnEmulationStopping);

    connect(&status_bar_update_timer, &QTimer::timeout, this, &GMainWindow::UpdateStatusBar);
}

void GMainWindow::ConnectMenuEvents() {
    // File
    connect(ui.action_Load_File, &QAction::triggered, this, &GMainWindow::OnMenuLoadFile);
    connect(ui.action_Load_Folder, &QAction::triggered, this, &GMainWindow::OnMenuLoadFolder);
    connect(ui.action_Install_File_NAND, &QAction::triggered, this,
            &GMainWindow::OnMenuInstallToNAND);
    connect(ui.action_Exit, &QAction::triggered, this, &QMainWindow::close);
    connect(ui.action_Load_Amiibo, &QAction::triggered, this, &GMainWindow::OnLoadAmiibo);

    // Emulation
    connect(ui.action_Start, &QAction::triggered, this, &GMainWindow::OnStartGame);
    connect(ui.action_Pause, &QAction::triggered, this, &GMainWindow::OnPauseGame);
    connect(ui.action_Stop, &QAction::triggered, this, &GMainWindow::OnStopGame);
    connect(ui.action_Report_Compatibility, &QAction::triggered, this,
            &GMainWindow::OnMenuReportCompatibility);
    connect(ui.action_Open_Mods_Page, &QAction::triggered, this, &GMainWindow::OnOpenModsPage);
    connect(ui.action_Open_Quickstart_Guide, &QAction::triggered, this,
            &GMainWindow::OnOpenQuickstartGuide);
    connect(ui.action_Open_FAQ, &QAction::triggered, this, &GMainWindow::OnOpenFAQ);
    connect(ui.action_Restart, &QAction::triggered, this, [this] { BootGame(QString(game_path)); });
    connect(ui.action_Configure, &QAction::triggered, this, &GMainWindow::OnConfigure);

    // View
    connect(ui.action_Single_Window_Mode, &QAction::triggered, this,
            &GMainWindow::ToggleWindowMode);
    connect(ui.action_Display_Dock_Widget_Headers, &QAction::triggered, this,
            &GMainWindow::OnDisplayTitleBars);
    connect(ui.action_Show_Filter_Bar, &QAction::triggered, this, &GMainWindow::OnToggleFilterBar);
    connect(ui.action_Show_Status_Bar, &QAction::triggered, statusBar(), &QStatusBar::setVisible);
    connect(ui.action_Reset_Window_Size, &QAction::triggered, this, &GMainWindow::ResetWindowSize);

    // Fullscreen
    connect(ui.action_Fullscreen, &QAction::triggered, this, &GMainWindow::ToggleFullscreen);

    // Movie
    connect(ui.action_Capture_Screenshot, &QAction::triggered, this,
            &GMainWindow::OnCaptureScreenshot);

    // Help
    connect(ui.action_Open_yuzu_Folder, &QAction::triggered, this, &GMainWindow::OnOpenYuzuFolder);
    connect(ui.action_Rederive, &QAction::triggered, this,
            std::bind(&GMainWindow::OnReinitializeKeys, this, ReinitializeKeyBehavior::Warning));
    connect(ui.action_About, &QAction::triggered, this, &GMainWindow::OnAbout);
}

void GMainWindow::OnDisplayTitleBars(bool show) {
    QList<QDockWidget*> widgets = findChildren<QDockWidget*>();

    if (show) {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(nullptr);
            if (old != nullptr)
                delete old;
        }
    } else {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(new QWidget());
            if (old != nullptr)
                delete old;
        }
    }
}

void GMainWindow::PreventOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
}

void GMainWindow::AllowOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#endif
}

bool GMainWindow::LoadROM(const QString& filename) {
    // Shutdown previous session if the emu thread is still active...
    if (emu_thread != nullptr)
        ShutdownGame();

    if (!render_window->InitRenderTarget()) {
        return false;
    }

    Core::System& system{Core::System::GetInstance()};
    system.SetFilesystem(vfs);

    system.SetAppletFrontendSet({
        nullptr,                                     // Parental Controls
        std::make_unique<QtErrorDisplay>(*this),     //
        nullptr,                                     // Photo Viewer
        std::make_unique<QtProfileSelector>(*this),  //
        std::make_unique<QtSoftwareKeyboard>(*this), //
        std::make_unique<QtWebBrowser>(*this),       //
        nullptr,                                     // E-Commerce
    });

    system.RegisterHostThread();

    const Core::System::ResultStatus result{system.Load(*render_window, filename.toStdString())};

    const auto drd_callout =
        (UISettings::values.callout_flags & static_cast<u32>(CalloutFlag::DRDDeprecation)) == 0;

    if (result == Core::System::ResultStatus::Success &&
        system.GetAppLoader().GetFileType() == Loader::FileType::DeconstructedRomDirectory &&
        drd_callout) {
        UISettings::values.callout_flags |= static_cast<u32>(CalloutFlag::DRDDeprecation);
        QMessageBox::warning(
            this, tr("警告过时的游戏格式"),
            tr("您正在为此游戏使用解构的ROM目录格式，这是已被 "
               "取代由其他如NCA，NAX，XCI，或NSP过时的格式 "
               "解构ROM目录缺少图标，元数据和更新和 "
               "支持.<br><br>搜索结果有关各种转换格式yuzu支持的说明, <a "
               "href='https://yuzu-emu.org/wiki/overview-of-switch-game-formats'>请参阅我们的 "
               "wiki</a>. 此消息将不再显示."));
    }

    if (result != Core::System::ResultStatus::Success) {
        switch (result) {
        case Core::System::ResultStatus::ErrorGetLoader:
            LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filename.toStdString());
            QMessageBox::critical(this, tr("加载时出错 ROM!"),
                                  tr("该ROM格式不支持."));
            break;
        case Core::System::ResultStatus::ErrorVideoCore:
            QMessageBox::critical(
                this, tr("发生错误初始化视频核心."),
                tr("yuzu 遇到了错误运行视频核心，同时，请查看 "
                   "日志以详细了解更多信息."
                   "访问日志，请参阅下面的页面如何上传: "
                   "<a href='https://community.citra-emu.org/t/how-to-upload-the-log-file/296'>How "
                   "文件 "
                   "确保您有最新的</a>."
                   "图形驱动程序，为您的GPU."));

            break;

        default:
            if (static_cast<u32>(result) >
                static_cast<u32>(Core::System::ResultStatus::ErrorLoader)) {
                const u16 loader_id = static_cast<u16>(Core::System::ResultStatus::ErrorLoader);
                const u16 error_id = static_cast<u16>(result) - loader_id;
                const std::string error_code = fmt::format("({:04X}-{:04X})", loader_id, error_id);
                LOG_CRITICAL(Frontend, "Failed to load ROM! {}", error_code);
                QMessageBox::critical(
                    this,
                    tr("加载时出错 ROM! ").append(QString::fromStdString(error_code)),
                    QString::fromStdString(fmt::format(
                        "{}<br>请关注 <a href='https://yuzu-emu.org/help/quickstart/'>这 "
                        "yuzu 快速入门指南</a> 还原文件.<br>你可以参考 "
                        " yuzu 维基</a> 或者 yuzu Discord</a> 求助.",
                        static_cast<Loader::ResultStatus>(error_id))));
            } else {
                QMessageBox::critical(
                    this, tr("加载时出错 ROM!"),
                    tr("出现未知错误，请参阅日志以了解更多详细信息."));
            }
            break;
        }
        return false;
    }
    game_path = filename;

    system.TelemetrySession().AddField(Telemetry::FieldType::App, "Frontend", "Qt");
    return true;
}

void GMainWindow::SelectAndSetCurrentUser() {
    QtProfileSelectionDialog dialog(this);
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Rejected) {
        return;
    }

    Settings::values.current_user = dialog.GetIndex();
}

void GMainWindow::BootGame(const QString& filename) {
    LOG_INFO(Frontend, "yuzu starting...");
    StoreRecentFile(filename); // Put the filename on top of the list

    u64 title_id{0};

    const auto v_file = Core::GetGameFileFromPath(vfs, filename.toUtf8().constData());
    const auto loader = Loader::GetLoader(v_file);
    if (!(loader == nullptr || loader->ReadProgramId(title_id) != Loader::ResultStatus::Success)) {
        // Load per game settings
        Config per_game_config(fmt::format("{:016X}.ini", title_id), false);
    }

    Settings::LogSettings();

    if (UISettings::values.select_user_on_boot) {
        SelectAndSetCurrentUser();
    }

    if (!LoadROM(filename))
        return;

    // Create and start the emulation thread
    emu_thread = std::make_unique<EmuThread>();
    emit EmulationStarting(emu_thread.get());
    emu_thread->start();

    connect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    // BlockingQueuedConnection is important here, it makes sure we've finished refreshing our views
    // before the CPU continues
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);

    connect(emu_thread.get(), &EmuThread::LoadProgress, loading_screen,
            &LoadingScreen::OnLoadProgress, Qt::QueuedConnection);

    // Update the GUI
    UpdateStatusButtons();
    if (ui.action_Single_Window_Mode->isChecked()) {
        game_list->hide();
        game_list_placeholder->hide();
    }
    status_bar_update_timer.start(2000);
    async_status_button->setDisabled(true);
    multicore_status_button->setDisabled(true);
    renderer_status_button->setDisabled(true);

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
        setMouseTracking(true);
        ui.centralwidget->setMouseTracking(true);
    }

    std::string title_name;
    std::string title_version;
    const auto res = Core::System::GetInstance().GetGameName(title_name);

    const auto metadata = FileSys::PatchManager(title_id).GetControlMetadata();
    if (metadata.first != nullptr) {
        title_version = metadata.first->GetVersionString();
        title_name = metadata.first->GetApplicationName();
    }
    if (res != Loader::ResultStatus::Success || title_name.empty()) {
        title_name = FileUtil::GetFilename(filename.toStdString());
    }
    LOG_INFO(Frontend, "Booting game: {:016X} | {} | {}", title_id, title_name, title_version);
    UpdateWindowTitle(title_name, title_version);

    loading_screen->Prepare(Core::System::GetInstance().GetAppLoader());
    loading_screen->show();

    emulation_running = true;
    if (ui.action_Fullscreen->isChecked()) {
        ShowFullscreen();
    }
    OnStartGame();
}

void GMainWindow::ShutdownGame() {
    if (!emulation_running) {
        return;
    }

    if (ui.action_Fullscreen->isChecked()) {
        HideFullscreen();
    }

    AllowOSSleep();

    discord_rpc->Pause();
    emu_thread->RequestStop();

    emit EmulationStopping();

    // Wait for emulation thread to complete and delete it
    emu_thread->wait();
    emu_thread = nullptr;

    discord_rpc->Update();

    // The emulation is stopped, so closing the window or not does not matter anymore
    disconnect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);

    // Update the GUI
    ui.action_Start->setEnabled(false);
    ui.action_Start->setText(tr("开始"));
    ui.action_Pause->setEnabled(false);
    ui.action_Stop->setEnabled(false);
    ui.action_Restart->setEnabled(false);
    ui.action_Report_Compatibility->setEnabled(false);
    ui.action_Load_Amiibo->setEnabled(false);
    ui.action_Capture_Screenshot->setEnabled(false);
    render_window->hide();
    loading_screen->hide();
    loading_screen->Clear();
    if (game_list->isEmpty())
        game_list_placeholder->show();
    else
        game_list->show();
    game_list->setFilterFocus();

    setMouseTracking(false);
    ui.centralwidget->setMouseTracking(false);

    UpdateWindowTitle();

    // Disable status bar updates
    status_bar_update_timer.stop();
    shader_building_label->setVisible(false);
    emu_speed_label->setVisible(false);
    game_fps_label->setVisible(false);
    emu_frametime_label->setVisible(false);
    async_status_button->setEnabled(true);
    multicore_status_button->setEnabled(true);
#ifdef HAS_VULKAN
    renderer_status_button->setEnabled(true);
#endif

    emulation_running = false;

    game_path.clear();

    // When closing the game, destroy the GLWindow to clear the context after the game is closed
    render_window->ReleaseRenderTarget();
}

void GMainWindow::StoreRecentFile(const QString& filename) {
    UISettings::values.recent_files.prepend(filename);
    UISettings::values.recent_files.removeDuplicates();
    while (UISettings::values.recent_files.size() > max_recent_files_item) {
        UISettings::values.recent_files.removeLast();
    }

    UpdateRecentFiles();
}

void GMainWindow::UpdateRecentFiles() {
    const int num_recent_files =
        std::min(UISettings::values.recent_files.size(), max_recent_files_item);

    for (int i = 0; i < num_recent_files; i++) {
        const QString text = QStringLiteral("&%1. %2").arg(i + 1).arg(
            QFileInfo(UISettings::values.recent_files[i]).fileName());
        actions_recent_files[i]->setText(text);
        actions_recent_files[i]->setData(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setToolTip(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setVisible(true);
    }

    for (int j = num_recent_files; j < max_recent_files_item; ++j) {
        actions_recent_files[j]->setVisible(false);
    }

    // Enable the recent files menu if the list isn't empty
    ui.menu_recent_files->setEnabled(num_recent_files != 0);
}

void GMainWindow::OnGameListLoadFile(QString game_path) {
    BootGame(game_path);
}

void GMainWindow::OnGameListOpenFolder(GameListOpenTarget target, const std::string& game_path) {
    std::string path;
    QString open_target;

    const auto v_file = Core::GetGameFileFromPath(vfs, game_path);
    const auto loader = Loader::GetLoader(v_file);
    FileSys::NACP control{};
    u64 program_id{};

    loader->ReadControlData(control);
    loader->ReadProgramId(program_id);

    const bool has_user_save{control.GetDefaultNormalSaveSize() > 0};
    const bool has_device_save{control.GetDeviceSaveDataSize() > 0};

    ASSERT_MSG(has_user_save != has_device_save, "Game uses both user and device savedata?");

    switch (target) {
    case GameListOpenTarget::SaveData: {
        open_target = tr("保存数据");
        const std::string nand_dir = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);

        if (has_user_save) {
            // User save data
            const auto select_profile = [this] {
                QtProfileSelectionDialog dialog(this);
                dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                                      Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
                dialog.setWindowModality(Qt::WindowModal);

                if (dialog.exec() == QDialog::Rejected) {
                    return -1;
                }

                return dialog.GetIndex();
            };

            const auto index = select_profile();
            if (index == -1) {
                return;
            }

            Service::Account::ProfileManager manager;
            const auto user_id = manager.GetUser(static_cast<std::size_t>(index));
            ASSERT(user_id);
            path = nand_dir + FileSys::SaveDataFactory::GetFullPath(
                                  FileSys::SaveDataSpaceId::NandUser,
                                  FileSys::SaveDataType::SaveData, program_id, user_id->uuid, 0);
        } else {
            // Device save data
            path = nand_dir + FileSys::SaveDataFactory::GetFullPath(
                                  FileSys::SaveDataSpaceId::NandUser,
                                  FileSys::SaveDataType::SaveData, program_id, {}, 0);
        }

        if (!FileUtil::Exists(path)) {
            FileUtil::CreateFullPath(path);
            FileUtil::CreateDir(path);
        }

        break;
    }
    case GameListOpenTarget::ModData: {
        open_target = tr("Mod 数据");
        const auto load_dir = FileUtil::GetUserPath(FileUtil::UserPath::LoadDir);
        path = fmt::format("{}{:016X}", load_dir, program_id);
        break;
    }
    default:
        UNIMPLEMENTED();
    }

    const QString qpath = QString::fromStdString(path);
    const QDir dir(qpath);
    if (!dir.exists()) {
        QMessageBox::warning(this, tr("错误打开 %1 文件夹").arg(open_target),
                             tr("文件夹不存在!"));
        return;
    }
    LOG_INFO(Frontend, "Opening {} path for program_id={:016x}", open_target.toStdString(),
             program_id);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

void GMainWindow::OnTransferableShaderCacheOpenFile(u64 program_id) {
    const QString shader_dir =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir));
    const QString transferable_shader_cache_folder_path =
        shader_dir + QStringLiteral("opengl") + QDir::separator() + QStringLiteral("transferable");
    const QString transferable_shader_cache_file_path =
        transferable_shader_cache_folder_path + QDir::separator() +
        QString::fromStdString(fmt::format("{:016X}.bin", program_id));

    if (!QFile::exists(transferable_shader_cache_file_path)) {
        QMessageBox::warning(this, tr("错误打开转换着色器缓存"),
                             tr("对于这个游戏着色器缓存中不存在."));
        return;
    }

    // Windows supports opening a folder with selecting a specified file in explorer. On every other
    // OS we just open the transferable shader cache folder without preselecting the transferable
    // shader cache file for the selected game.
#if defined(Q_OS_WIN)
    const QString explorer = QStringLiteral("explorer");
    QStringList param;
    if (!QFileInfo(transferable_shader_cache_file_path).isDir()) {
        param << QStringLiteral("/select,");
    }
    param << QDir::toNativeSeparators(transferable_shader_cache_file_path);
    QProcess::startDetached(explorer, param);
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(transferable_shader_cache_folder_path));
#endif
}

static std::size_t CalculateRomFSEntrySize(const FileSys::VirtualDir& dir, bool full) {
    std::size_t out = 0;

    for (const auto& subdir : dir->GetSubdirectories()) {
        out += 1 + CalculateRomFSEntrySize(subdir, full);
    }

    return out + (full ? dir->GetFiles().size() : 0);
}

static bool RomFSRawCopy(QProgressDialog& dialog, const FileSys::VirtualDir& src,
                         const FileSys::VirtualDir& dest, std::size_t block_size, bool full) {
    if (src == nullptr || dest == nullptr || !src->IsReadable() || !dest->IsWritable())
        return false;
    if (dialog.wasCanceled())
        return false;

    if (full) {
        for (const auto& file : src->GetFiles()) {
            const auto out = VfsDirectoryCreateFileWrapper(dest, file->GetName());
            if (!FileSys::VfsRawCopy(file, out, block_size))
                return false;
            dialog.setValue(dialog.value() + 1);
            if (dialog.wasCanceled())
                return false;
        }
    }

    for (const auto& dir : src->GetSubdirectories()) {
        const auto out = dest->CreateSubdirectory(dir->GetName());
        if (!RomFSRawCopy(dialog, dir, out, block_size, full))
            return false;
        dialog.setValue(dialog.value() + 1);
        if (dialog.wasCanceled())
            return false;
    }

    return true;
}

void GMainWindow::OnGameListRemoveInstalledEntry(u64 program_id, InstalledEntryType type) {
    const QString entry_type = [this, type] {
        switch (type) {
        case InstalledEntryType::Game:
            return tr("目录");
        case InstalledEntryType::Update:
            return tr("Update");
        case InstalledEntryType::AddOnContent:
            return tr("DLC");
        default:
            return QString{};
        }
    }();

    if (QMessageBox::question(
            this, tr("删除条目"), tr("删除已安装的游戏 %1?").arg(entry_type),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    switch (type) {
    case InstalledEntryType::Game:
        RemoveBaseContent(program_id, entry_type);
        [[fallthrough]];
    case InstalledEntryType::Update:
        RemoveUpdateContent(program_id, entry_type);
        if (type != InstalledEntryType::Game) {
            break;
        }
        [[fallthrough]];
    case InstalledEntryType::AddOnContent:
        RemoveAddOnContent(program_id, entry_type);
        break;
    }
    FileUtil::DeleteDirRecursively(FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + DIR_SEP +
                                   "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

void GMainWindow::RemoveBaseContent(u64 program_id, const QString& entry_type) {
    const auto& fs_controller = Core::System::GetInstance().GetFileSystemController();
    const auto res = fs_controller.GetUserNANDContents()->RemoveExistingEntry(program_id) ||
                     fs_controller.GetSDMCContents()->RemoveExistingEntry(program_id);

    if (res) {
        QMessageBox::information(this, tr("成功删除"),
                                 tr("成功删除了已安装的基本游戏。"));
    } else {
        QMessageBox::warning(
            this, tr("错误删除 %1").arg(entry_type),
            tr("基本游戏未安装在NAND中，因此无法删除。"));
    }
}

void GMainWindow::RemoveUpdateContent(u64 program_id, const QString& entry_type) {
    const auto update_id = program_id | 0x800;
    const auto& fs_controller = Core::System::GetInstance().GetFileSystemController();
    const auto res = fs_controller.GetUserNANDContents()->RemoveExistingEntry(update_id) ||
                     fs_controller.GetSDMCContents()->RemoveExistingEntry(update_id);

    if (res) {
        QMessageBox::information(this, tr("成功删除"),
                                 tr("成功删除了已安装的更新。"));
    } else {
        QMessageBox::warning(this, tr("错误删除 %1").arg(entry_type),
                             tr("没有为此标题安装更新。"));
    }
}

void GMainWindow::RemoveAddOnContent(u64 program_id, const QString& entry_type) {
    u32 count{};
    const auto& fs_controller = Core::System::GetInstance().GetFileSystemController();
    const auto dlc_entries = Core::System::GetInstance().GetContentProvider().ListEntriesFilter(
        FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);

    for (const auto& entry : dlc_entries) {
        if ((entry.title_id & DLC_BASE_TITLE_ID_MASK) == program_id) {
            const auto res =
                fs_controller.GetUserNANDContents()->RemoveExistingEntry(entry.title_id) ||
                fs_controller.GetSDMCContents()->RemoveExistingEntry(entry.title_id);
            if (res) {
                ++count;
            }
        }
    }

    if (count == 0) {
        QMessageBox::warning(this, tr("错误删除 %1").arg(entry_type),
                             tr("没有为此标题安装DLC。"));
        return;
    }

    QMessageBox::information(this, tr("成功删除"),
                             tr("成功删除 %1 安装的 DLC.").arg(count));
}

void GMainWindow::OnGameListRemoveFile(u64 program_id, GameListRemoveTarget target) {
    const QString question = [this, target] {
        switch (target) {
        case GameListRemoveTarget::ShaderCache:
            return tr("删除可传输着色器缓存？");
        case GameListRemoveTarget::CustomConfiguration:
            return tr("删除自定义游戏设置？");
        default:
            return QString{};
        }
    }();

    if (QMessageBox::question(this, tr("删除文件"), question, QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    switch (target) {
    case GameListRemoveTarget::ShaderCache:
        RemoveTransferableShaderCache(program_id);
        break;
    case GameListRemoveTarget::CustomConfiguration:
        RemoveCustomConfiguration(program_id);
        break;
    }
}

void GMainWindow::RemoveTransferableShaderCache(u64 program_id) {
    const QString shader_dir =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir));
    const QString transferable_shader_cache_folder_path =
        shader_dir + QStringLiteral("opengl") + QDir::separator() + QStringLiteral("transferable");
    const QString transferable_shader_cache_file_path =
        transferable_shader_cache_folder_path + QDir::separator() +
        QString::fromStdString(fmt::format("{:016X}.bin", program_id));

    if (!QFile::exists(transferable_shader_cache_file_path)) {
        QMessageBox::warning(this, tr("删除可传输着色器缓存时出错"),
                             tr("此游戏的着色器缓存不存在。"));
        return;
    }

    if (QFile::remove(transferable_shader_cache_file_path)) {
        QMessageBox::information(this, tr("成功删除"),
                                 tr("成功删除了可转移的着色器缓存。"));
    } else {
        QMessageBox::warning(this, tr("删除可传输着色器缓存时出错"),
                             tr("无法删除可转移的着色器缓存。"));
    }
}

void GMainWindow::RemoveCustomConfiguration(u64 program_id) {
    const QString config_dir =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir));
    const QString custom_config_file_path =
        config_dir + QString::fromStdString(fmt::format("{:016X}.ini", program_id));

    if (!QFile::exists(custom_config_file_path)) {
        QMessageBox::warning(this, tr("删除自定义设置时出错"),
                             tr("此游戏的自定义设置不存在。"));
        return;
    }

    if (QFile::remove(custom_config_file_path)) {
        QMessageBox::information(this, tr("成功删除"),
                                 tr("成功删除了自定义游戏设置。"));
    } else {
        QMessageBox::warning(this, tr("删除自定义设置时出错"),
                             tr("无法删除自定义游戏设置。"));
    }
}

void GMainWindow::OnGameListDumpRomFS(u64 program_id, const std::string& game_path) {
    const auto failed = [this] {
        QMessageBox::warning(this, tr("RomFS 提取失败!"),
                             tr("有一个错误复制RomFS文件 "
                                "或用户取消了操作."));
    };

    const auto loader = Loader::GetLoader(vfs->OpenFile(game_path, FileSys::Mode::Read));
    if (loader == nullptr) {
        failed();
        return;
    }

    FileSys::VirtualFile file;
    if (loader->ReadRomFS(file) != Loader::ResultStatus::Success) {
        failed();
        return;
    }

    const auto& installed = Core::System::GetInstance().GetContentProvider();
    const auto romfs_title_id = SelectRomFSDumpTarget(installed, program_id);

    if (!romfs_title_id) {
        failed();
        return;
    }

    const auto path = fmt::format(
        "{}{:016X}/romfs", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir), *romfs_title_id);

    FileSys::VirtualFile romfs;

    if (*romfs_title_id == program_id) {
        const u64 ivfc_offset = loader->ReadRomFSIVFCOffset();
        FileSys::PatchManager pm{program_id};
        romfs = pm.PatchRomFS(file, ivfc_offset, FileSys::ContentRecordType::Program);
    } else {
        romfs = installed.GetEntry(*romfs_title_id, FileSys::ContentRecordType::Data)->GetRomFS();
    }

    const auto extracted = FileSys::ExtractRomFS(romfs, FileSys::RomFSExtractionType::Full);
    if (extracted == nullptr) {
        failed();
        return;
    }

    const auto out = VfsFilesystemCreateDirectoryWrapper(vfs, path, FileSys::Mode::ReadWrite);

    if (out == nullptr) {
        failed();
        vfs->DeleteDirectory(path);
        return;
    }

    bool ok = false;
    const QStringList selections{tr("全部"), tr("空文件夹")};
    const auto res = QInputDialog::getItem(
        this, tr("选择RomFS转储模式"),
        tr("请选择您希望的RomFS的 全部 完全将所有的文件复 "
           "制到新\n目录中，而结果 空文件夹 只会创建 "
           "目录结构."),
        selections, 0, false, &ok);
    if (!ok) {
        failed();
        vfs->DeleteDirectory(path);
        return;
    }

    const auto full = res == selections.constFirst();
    const auto entry_size = CalculateRomFSEntrySize(extracted, full);

    QProgressDialog progress(tr("提取 RomFS..."), tr("取消"), 0,
                             static_cast<s32>(entry_size), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(100);

    if (RomFSRawCopy(progress, extracted, out, 0x400000, full)) {
        progress.close();
        QMessageBox::information(this, tr("RomFS 提取成功了!"),
                                 tr("操作已成功完成."));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
    } else {
        progress.close();
        failed();
        vfs->DeleteDirectory(path);
    }
}

void GMainWindow::OnGameListCopyTID(u64 program_id) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QString::fromStdString(fmt::format("{:016X}", program_id)));
}

void GMainWindow::OnGameListNavigateToGamedbEntry(u64 program_id,
                                                  const CompatibilityList& compatibility_list) {
    const auto it = FindMatchingCompatibilityEntry(compatibility_list, program_id);

    QString directory;
    if (it != compatibility_list.end()) {
        directory = it->second.second;
    }

    QDesktopServices::openUrl(QUrl(QStringLiteral("https://yuzu-emu.org/game/") + directory));
}

void GMainWindow::OnGameListOpenDirectory(const QString& directory) {
    QString path;
    if (directory == QStringLiteral("SDMC")) {
        path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) +
                                      "Nintendo/Contents/registered");
    } else if (directory == QStringLiteral("UserNAND")) {
        path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir) +
                                      "user/Contents/registered");
    } else if (directory == QStringLiteral("SysNAND")) {
        path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir) +
                                      "system/Contents/registered");
    } else {
        path = directory;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::critical(this, tr("错误打开 %1").arg(path), tr("文件夹不存在!"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void GMainWindow::OnGameListAddDirectory() {
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("选择目录"));
    if (dir_path.isEmpty())
        return;
    UISettings::GameDir game_dir{dir_path, false, true};
    if (!UISettings::values.game_dirs.contains(game_dir)) {
        UISettings::values.game_dirs.append(game_dir);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    } else {
        LOG_WARNING(Frontend, "Selected directory is already in the game list");
    }
}

void GMainWindow::OnGameListShowList(bool show) {
    if (emulation_running && ui.action_Single_Window_Mode->isChecked())
        return;
    game_list->setVisible(show);
    game_list_placeholder->setVisible(!show);
};

void GMainWindow::OnGameListOpenPerGameProperties(const std::string& file) {
    u64 title_id{};
    const auto v_file = Core::GetGameFileFromPath(vfs, file);
    const auto loader = Loader::GetLoader(v_file);
    if (loader == nullptr || loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        QMessageBox::information(this, tr("属性"),
                                 tr("游戏性能无法加载."));
        return;
    }

    ConfigurePerGame dialog(this, title_id);
    dialog.LoadFromFile(v_file);
    auto result = dialog.exec();
    if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();

        const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
        if (reload) {
            game_list->PopulateAsync(UISettings::values.game_dirs);
        }

        // Do not cause the global config to write local settings into the config file
        Settings::RestoreGlobalState();

        if (!Core::System::GetInstance().IsPoweredOn()) {
            config->Save();
        }
    } else {
        Settings::RestoreGlobalState();
    }
}

void GMainWindow::OnMenuLoadFile() {
    const QString extensions =
        QStringLiteral("*.")
            .append(GameList::supported_file_extensions.join(QStringLiteral(" *.")))
            .append(QStringLiteral(" main"));
    const QString file_filter = tr("Switch 可执行文件 (%1);;所有的文件 (*.*)",
                                   "%1 is an identifier for the Switch executable file extensions.")
                                    .arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("加载文件"), UISettings::values.roms_path, file_filter);

    if (filename.isEmpty()) {
        return;
    }

    UISettings::values.roms_path = QFileInfo(filename).path();
    BootGame(filename);
}

void GMainWindow::OnMenuLoadFolder() {
    const QString dir_path =
        QFileDialog::getExistingDirectory(this, tr("打开提取 ROM 目录"));

    if (dir_path.isNull()) {
        return;
    }

    const QDir dir{dir_path};
    const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
    if (matching_main.size() == 1) {
        BootGame(dir.path() + QDir::separator() + matching_main[0]);
    } else {
        QMessageBox::warning(this, tr("无效的目录选择"),
                             tr("您选择的目录不包含一个 'main' 文件."));
    }
}

void GMainWindow::IncrementInstallProgress() {
    install_progress->setValue(install_progress->value() + 1);
}

void GMainWindow::OnMenuInstallToNAND() {
    const QString file_filter =
        tr("安装 Switch 文件 (*.nca *.nsp *.xci);;任天堂内容存档 "
           "(*.nca);;任天堂提交包 (*.nsp);;NX 盒式 "
           "图像 (*.xci)");

    QStringList filenames = QFileDialog::getOpenFileNames(
        this, tr("安装文件"), UISettings::values.roms_path, file_filter);

    if (filenames.isEmpty()) {
        return;
    }

    InstallDialog installDialog(this, filenames);
    if (installDialog.exec() == QDialog::Rejected) {
        return;
    }

    const QStringList files = installDialog.GetFiles();

    if (files.isEmpty()) {
        return;
    }

    int remaining = filenames.size();

    // This would only overflow above 2^43 bytes (8.796 TB)
    int total_size = 0;
    for (const QString& file : files) {
        total_size += static_cast<int>(QFile(file).size() / 0x1000);
    }
    if (total_size < 0) {
        LOG_CRITICAL(Frontend, "Attempting to install too many files, aborting.");
        return;
    }

    QStringList new_files{};         // Newly installed files that do not yet exist in the NAND
    QStringList overwritten_files{}; // Files that overwrote those existing in the NAND
    QStringList failed_files{};      // Files that failed to install due to errors

    ui.action_Install_File_NAND->setEnabled(false);

    install_progress = new QProgressDialog(QStringLiteral(""), tr("取消"), 0, total_size, this);
    install_progress->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint &
                                     ~Qt::WindowMaximizeButtonHint);
    install_progress->setAttribute(Qt::WA_DeleteOnClose, true);
    install_progress->setFixedWidth(installDialog.GetMinimumWidth() + 40);
    install_progress->show();

    for (const QString& file : files) {
        install_progress->setWindowTitle(tr("%n 文件(s) 剩余的", "", remaining));
        install_progress->setLabelText(
            tr("正在安装 文件 \"%1\"...").arg(QFileInfo(file).fileName()));

        QFuture<InstallResult> future;
        InstallResult result;

        if (file.endsWith(QStringLiteral("xci"), Qt::CaseInsensitive) ||
            file.endsWith(QStringLiteral("nsp"), Qt::CaseInsensitive)) {

            future = QtConcurrent::run([this, &file] { return InstallNSPXCI(file); });

            while (!future.isFinished()) {
                QCoreApplication::processEvents();
            }

            result = future.result();

        } else {
            result = InstallNCA(file);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        switch (result) {
        case InstallResult::Success:
            new_files.append(QFileInfo(file).fileName());
            break;
        case InstallResult::Overwrite:
            overwritten_files.append(QFileInfo(file).fileName());
            break;
        case InstallResult::Failure:
            failed_files.append(QFileInfo(file).fileName());
            break;
        }

        --remaining;
    }

    install_progress->close();

    const QString install_results =
        (new_files.isEmpty() ? QStringLiteral("")
                             : tr("%n 文件(s) 是新安装的\n", "", new_files.size())) +
        (overwritten_files.isEmpty()
             ? QStringLiteral("")
             : tr("%n 文件(s) 被覆盖\n", "", overwritten_files.size())) +
        (failed_files.isEmpty() ? QStringLiteral("")
                                : tr("%n 文件(s) 安装失败\n", "", failed_files.size()));

    QMessageBox::information(this, tr("安装结果"), install_results);
    FileUtil::DeleteDirRecursively(FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + DIR_SEP +
                                   "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
    ui.action_Install_File_NAND->setEnabled(true);
}

InstallResult GMainWindow::InstallNSPXCI(const QString& filename) {
    const auto qt_raw_copy = [this](const FileSys::VirtualFile& src,
                                    const FileSys::VirtualFile& dest, std::size_t block_size) {
        if (src == nullptr || dest == nullptr) {
            return false;
        }
        if (!dest->Resize(src->GetSize())) {
            return false;
        }

        std::array<u8, 0x1000> buffer{};

        for (std::size_t i = 0; i < src->GetSize(); i += buffer.size()) {
            if (install_progress->wasCanceled()) {
                dest->Resize(0);
                return false;
            }

            emit UpdateInstallProgress();

            const auto read = src->Read(buffer.data(), buffer.size(), i);
            dest->Write(buffer.data(), read, i);
        }
        return true;
    };

    std::shared_ptr<FileSys::NSP> nsp;
    if (filename.endsWith(QStringLiteral("nsp"), Qt::CaseInsensitive)) {
        nsp = std::make_shared<FileSys::NSP>(
            vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
        if (nsp->IsExtractedType()) {
            return InstallResult::Failure;
        }
    } else {
        const auto xci = std::make_shared<FileSys::XCI>(
            vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
        nsp = xci->GetSecurePartitionNSP();
    }

    if (nsp->GetStatus() != Loader::ResultStatus::Success) {
        return InstallResult::Failure;
    }
    const auto res =
        Core::System::GetInstance().GetFileSystemController().GetUserNANDContents()->InstallEntry(
            *nsp, true, qt_raw_copy);
    if (res == FileSys::InstallResult::Success) {
        return InstallResult::Success;
    } else if (res == FileSys::InstallResult::OverwriteExisting) {
        return InstallResult::Overwrite;
    } else {
        return InstallResult::Failure;
    }
}

InstallResult GMainWindow::InstallNCA(const QString& filename) {
    const auto qt_raw_copy = [this](const FileSys::VirtualFile& src,
                                    const FileSys::VirtualFile& dest, std::size_t block_size) {
        if (src == nullptr || dest == nullptr) {
            return false;
        }
        if (!dest->Resize(src->GetSize())) {
            return false;
        }

        std::array<u8, 0x1000> buffer{};

        for (std::size_t i = 0; i < src->GetSize(); i += buffer.size()) {
            if (install_progress->wasCanceled()) {
                dest->Resize(0);
                return false;
            }

            emit UpdateInstallProgress();

            const auto read = src->Read(buffer.data(), buffer.size(), i);
            dest->Write(buffer.data(), read, i);
        }
        return true;
    };

    const auto nca =
        std::make_shared<FileSys::NCA>(vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
    const auto id = nca->GetStatus();

    // Game updates necessary are missing base RomFS
    if (id != Loader::ResultStatus::Success &&
        id != Loader::ResultStatus::ErrorMissingBKTRBaseRomFS) {
        return InstallResult::Failure;
    }

    const QStringList tt_options{tr("系统中的应用"),
                                 tr("系统存档"),
                                 tr("系统应用程序更新"),
                                 tr("固件包（A型）"),
                                 tr("固件包（B型）"),
                                 tr("游戏"),
                                 tr("游戏更新"),
                                 tr("游戏  DLC"),
                                 tr("Delta 游戏")};
    bool ok;
    const auto item = QInputDialog::getItem(
        this, tr("选择 NCA 安装类型..."),
        tr("请选择题目的类型，你想安装此NCA，因为在大多数情况:\n(In "
           "默认的 '游戏' 是很好的。)"),
        tt_options, 5, false, &ok);

    auto index = tt_options.indexOf(item);
    if (!ok || index == -1) {
        QMessageBox::warning(this, tr("安装失败"),
                             tr("您选择的NCA游戏类型无效。"));
        return InstallResult::Failure;
    }

    // If index is equal to or past Game, add the jump in TitleType.
    if (index >= 5) {
        index += static_cast<size_t>(FileSys::TitleType::Application) -
                 static_cast<size_t>(FileSys::TitleType::FirmwarePackageB);
    }

    FileSys::InstallResult res;
    if (index >= static_cast<s32>(FileSys::TitleType::Application)) {
        res = Core::System::GetInstance()
                  .GetFileSystemController()
                  .GetUserNANDContents()
                  ->InstallEntry(*nca, static_cast<FileSys::TitleType>(index), true, qt_raw_copy);
    } else {
        res = Core::System::GetInstance()
                  .GetFileSystemController()
                  .GetSystemNANDContents()
                  ->InstallEntry(*nca, static_cast<FileSys::TitleType>(index), true, qt_raw_copy);
    }

    if (res == FileSys::InstallResult::Success) {
        return InstallResult::Success;
    } else if (res == FileSys::InstallResult::OverwriteExisting) {
        return InstallResult::Overwrite;
    } else {
        return InstallResult::Failure;
    }
}

void GMainWindow::OnMenuRecentFile() {
    QAction* action = qobject_cast<QAction*>(sender());
    assert(action);

    const QString filename = action->data().toString();
    if (QFileInfo::exists(filename)) {
        BootGame(filename);
    } else {
        // Display an error message and remove the file from the list.
        QMessageBox::information(this, tr("文件未找到"),
                                 tr("文件 \"%1\" 未找到").arg(filename));

        UISettings::values.recent_files.removeOne(filename);
        UpdateRecentFiles();
    }
}

void GMainWindow::OnStartGame() {
    PreventOSSleep();

    emu_thread->SetRunning(true);

    qRegisterMetaType<Core::Frontend::SoftwareKeyboardParameters>(
        "Core::Frontend::SoftwareKeyboardParameters");
    qRegisterMetaType<Core::System::ResultStatus>("Core::System::ResultStatus");
    qRegisterMetaType<std::string>("std::string");
    qRegisterMetaType<std::optional<std::u16string>>("std::optional<std::u16string>");
    qRegisterMetaType<std::string_view>("std::string_view");

    connect(emu_thread.get(), &EmuThread::ErrorThrown, this, &GMainWindow::OnCoreError);

    ui.action_Start->setEnabled(false);
    ui.action_Start->setText(tr("继续"));

    ui.action_Pause->setEnabled(true);
    ui.action_Stop->setEnabled(true);
    ui.action_Restart->setEnabled(true);
    ui.action_Report_Compatibility->setEnabled(true);

    discord_rpc->Update();
    ui.action_Load_Amiibo->setEnabled(true);
    ui.action_Capture_Screenshot->setEnabled(true);
}

void GMainWindow::OnPauseGame() {
    emu_thread->SetRunning(false);

    ui.action_Start->setEnabled(true);
    ui.action_Pause->setEnabled(false);
    ui.action_Stop->setEnabled(true);
    ui.action_Capture_Screenshot->setEnabled(false);

    AllowOSSleep();
}

void GMainWindow::OnStopGame() {
    Core::System& system{Core::System::GetInstance()};
    if (system.GetExitLock() && !ConfirmForceLockedExit()) {
        return;
    }

    ShutdownGame();

    Settings::RestoreGlobalState();
    UpdateStatusButtons();
}

void GMainWindow::OnLoadComplete() {
    loading_screen->OnLoadComplete();
}

void GMainWindow::ErrorDisplayDisplayError(QString body) {
    QMessageBox::critical(this, tr("错误显示"), body);
    emit ErrorDisplayFinished();
}

void GMainWindow::OnMenuReportCompatibility() {
    if (!Settings::values.yuzu_token.empty() && !Settings::values.yuzu_username.empty()) {
        CompatDB compatdb{this};
        compatdb.exec();
    } else {
        QMessageBox::critical(
            this, tr("缺少 yuzu 账户"),
            tr("为了提交一个游戏兼容性测试用 "
               "您必须.<br><br/>连接您的yuzu帐户以链接您的yuzu帐户，然后转到模拟器 "
               "&gt; "
               "Web."));
    }
}

void GMainWindow::OpenURL(const QUrl& url) {
    const bool open = QDesktopServices::openUrl(url);
    if (!open) {
        QMessageBox::warning(this, tr("打开网址时出错"),
                             tr("无法打开网址 \"%1\".").arg(url.toString()));
    }
}

void GMainWindow::OnOpenModsPage() {
    OpenURL(QUrl(QStringLiteral("https://github.com/yuzu-emu/yuzu/wiki/Switch-Mods")));
}

void GMainWindow::OnOpenQuickstartGuide() {
    OpenURL(QUrl(QStringLiteral("https://yuzu-emu.org/help/quickstart/")));
}

void GMainWindow::OnOpenFAQ() {
    OpenURL(QUrl(QStringLiteral("https://yuzu-emu.org/wiki/faq/")));
}

void GMainWindow::ToggleFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (ui.action_Fullscreen->isChecked()) {
        ShowFullscreen();
    } else {
        HideFullscreen();
    }
}

void GMainWindow::ShowFullscreen() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        ui.menubar->hide();
        statusBar()->hide();
        showFullScreen();
    } else {
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
        render_window->showFullScreen();
    }
}

void GMainWindow::HideFullscreen() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        statusBar()->setVisible(ui.action_Show_Status_Bar->isChecked());
        ui.menubar->show();
        showNormal();
        restoreGeometry(UISettings::values.geometry);
    } else {
        render_window->showNormal();
        render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
    }
}

void GMainWindow::ToggleWindowMode() {
    if (ui.action_Single_Window_Mode->isChecked()) {
        // Render in the main window...
        render_window->BackupGeometry();
        ui.horizontalLayout->addWidget(render_window);
        render_window->setFocusPolicy(Qt::StrongFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->setFocus();
            game_list->hide();
        }

    } else {
        // Render in a separate window...
        ui.horizontalLayout->removeWidget(render_window);
        render_window->setParent(nullptr);
        render_window->setFocusPolicy(Qt::NoFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->RestoreGeometry();
            game_list->show();
        }
    }
}

void GMainWindow::ResetWindowSize() {
    const auto aspect_ratio = Layout::EmulationAspectRatio(
        static_cast<Layout::AspectRatio>(Settings::values.aspect_ratio.GetValue()),
        static_cast<float>(Layout::ScreenUndocked::Height) / Layout::ScreenUndocked::Width);
    if (!ui.action_Single_Window_Mode->isChecked()) {
        render_window->resize(Layout::ScreenUndocked::Height / aspect_ratio,
                              Layout::ScreenUndocked::Height);
    } else {
        resize(Layout::ScreenUndocked::Height / aspect_ratio,
               Layout::ScreenUndocked::Height + menuBar()->height() +
                   (ui.action_Show_Status_Bar->isChecked() ? statusBar()->height() : 0));
    }
}

void GMainWindow::OnConfigure() {
    const auto old_theme = UISettings::values.theme;
    const bool old_discord_presence = UISettings::values.enable_discord_presence;

    ConfigureDialog configure_dialog(this, hotkey_registry);
    connect(&configure_dialog, &ConfigureDialog::LanguageChanged, this,
            &GMainWindow::OnLanguageChanged);

    const auto result = configure_dialog.exec();
    if (result != QDialog::Accepted) {
        return;
    }

    configure_dialog.ApplyConfiguration();
    InitializeHotkeys();
    if (UISettings::values.theme != old_theme) {
        UpdateUITheme();
    }
    if (UISettings::values.enable_discord_presence != old_discord_presence) {
        SetDiscordEnabled(UISettings::values.enable_discord_presence);
    }
    emit UpdateThemedIcons();

    const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
    if (reload) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }

    config->Save();

    if (UISettings::values.hide_mouse && emulation_running) {
        setMouseTracking(true);
        ui.centralwidget->setMouseTracking(true);
        mouse_hide_timer.start();
    } else {
        setMouseTracking(false);
        ui.centralwidget->setMouseTracking(false);
    }

    UpdateStatusButtons();
}

void GMainWindow::OnLoadAmiibo() {
    const QString extensions{QStringLiteral("*.bin")};
    const QString file_filter = tr("Amiibo 文件 (%1);; 所有的文件 (*.*)").arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(this, tr("加载 Amiibo"), {}, file_filter);

    if (filename.isEmpty()) {
        return;
    }

    LoadAmiibo(filename);
}

void GMainWindow::LoadAmiibo(const QString& filename) {
    Core::System& system{Core::System::GetInstance()};
    Service::SM::ServiceManager& sm = system.ServiceManager();
    auto nfc = sm.GetService<Service::NFP::Module::Interface>("nfp:user");
    if (nfc == nullptr) {
        return;
    }

    QFile nfc_file{filename};
    if (!nfc_file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("错误打开 Amiibo 数据文件"),
                             tr("无法打开 Amiibo 文件 \"%1\" 阅读.").arg(filename));
        return;
    }

    const u64 nfc_file_size = nfc_file.size();
    std::vector<u8> buffer(nfc_file_size);
    const u64 read_size = nfc_file.read(reinterpret_cast<char*>(buffer.data()), nfc_file_size);
    if (nfc_file_size != read_size) {
        QMessageBox::warning(this, tr("读取错误 Amiibo 数据文件"),
                             tr("无法完全读 Amiibo 数据. 预计读取 %1 个字节 "
                                "但只能读取 %2 个字节.")
                                 .arg(nfc_file_size)
                                 .arg(read_size));
        return;
    }

    if (!nfc->LoadAmiibo(buffer)) {
        QMessageBox::warning(this, tr("错误加载 Amiibo 数据"),
                             tr("无法加载 Amiibo 数据."));
    }
}

void GMainWindow::OnOpenYuzuFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir))));
}

void GMainWindow::OnAbout() {
    AboutDialog aboutDialog(this);
    aboutDialog.exec();
}

void GMainWindow::OnToggleFilterBar() {
    game_list->setFilterVisible(ui.action_Show_Filter_Bar->isChecked());
    if (ui.action_Show_Filter_Bar->isChecked()) {
        game_list->setFilterFocus();
    } else {
        game_list->clearFilter();
    }
}

void GMainWindow::OnCaptureScreenshot() {
    OnPauseGame();

    const u64 title_id = Core::System::GetInstance().CurrentProcess()->GetTitleID();
    const auto screenshot_path =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ScreenshotsDir));
    const auto date =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_hh-mm-ss-zzz"));
    QString filename = QStringLiteral("%1%2_%3.png")
                           .arg(screenshot_path)
                           .arg(title_id, 16, 16, QLatin1Char{'0'})
                           .arg(date);

#ifdef _WIN32
    if (UISettings::values.enable_screenshot_save_as) {
        filename = QFileDialog::getSaveFileName(this, tr("捕捉截图"), filename,
                                                tr("PNG 图片 (*.png)"));
        if (filename.isEmpty()) {
            OnStartGame();
            return;
        }
    }
#endif
    render_window->CaptureScreenshot(UISettings::values.screenshot_resolution_factor, filename);
    OnStartGame();
}

void GMainWindow::UpdateWindowTitle(const std::string& title_name,
                                    const std::string& title_version) {
    const auto full_name = std::string(Common::g_build_fullname);
    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto date =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")).toStdString();

    if (title_name.empty()) {
        const auto fmt = std::string(Common::g_title_bar_format_idle);
        setWindowTitle(QString::fromStdString(fmt::format(fmt.empty() ? "yuzu Early Access 809" : fmt,
                                                          full_name, branch_name, description,
                                                          std::string{}, date, build_id)));
    } else {
        const auto fmt = std::string(Common::g_title_bar_format_running);
        setWindowTitle(QString::fromStdString(
            fmt::format(fmt.empty() ? "yuzu Early Access 809 {0}| {3} {6}" : fmt, full_name, branch_name,
                        description, title_name, date, build_id, title_version)));
    }
}

void GMainWindow::UpdateStatusBar() {
    if (emu_thread == nullptr) {
        status_bar_update_timer.stop();
        return;
    }

    auto results = Core::System::GetInstance().GetAndResetPerfStats();
    auto& shader_notify = Core::System::GetInstance().GPU().ShaderNotify();
    const auto shaders_building = shader_notify.GetShadersBuilding();

    if (shaders_building != 0) {
        shader_building_label->setText(
            tr("构建: %1 着色器").arg(shaders_building) +
            (shaders_building != 1 ? QString::fromStdString("s") : QString::fromStdString("")));
        shader_building_label->setVisible(true);
    } else {
        shader_building_label->setVisible(false);
    }

    if (Settings::values.use_frame_limit.GetValue()) {
        emu_speed_label->setText(tr("速度: %1% / %2%")
                                     .arg(results.emulation_speed * 100.0, 0, 'f', 0)
                                     .arg(Settings::values.frame_limit.GetValue()));
    } else {
        emu_speed_label->setText(tr("速度: %1%").arg(results.emulation_speed * 100.0, 0, 'f', 0));
    }
    game_fps_label->setText(tr("游戏: %1 FPS").arg(results.game_fps, 0, 'f', 0));
    emu_frametime_label->setText(tr("帧: %1 ms").arg(results.frametime * 1000.0, 0, 'f', 2));

    emu_speed_label->setVisible(!Settings::values.use_multi_core.GetValue());
    game_fps_label->setVisible(true);
    emu_frametime_label->setVisible(true);
}

void GMainWindow::UpdateStatusButtons() {
    dock_status_button->setChecked(Settings::values.use_docked_mode);
    multicore_status_button->setChecked(Settings::values.use_multi_core.GetValue());
    Settings::values.use_asynchronous_gpu_emulation.SetValue(
        Settings::values.use_asynchronous_gpu_emulation.GetValue() ||
        Settings::values.use_multi_core.GetValue());
    async_status_button->setChecked(Settings::values.use_asynchronous_gpu_emulation.GetValue());
#ifdef HAS_VULKAN
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
#endif
}

void GMainWindow::HideMouseCursor() {
    if (emu_thread == nullptr || UISettings::values.hide_mouse == false) {
        mouse_hide_timer.stop();
        ShowMouseCursor();
        return;
    }
    setCursor(QCursor(Qt::BlankCursor));
}

void GMainWindow::ShowMouseCursor() {
    unsetCursor();
    if (emu_thread != nullptr && UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }
}

void GMainWindow::mouseMoveEvent(QMouseEvent* event) {
    ShowMouseCursor();
}

void GMainWindow::mousePressEvent(QMouseEvent* event) {
    ShowMouseCursor();
}

void GMainWindow::OnCoreError(Core::System::ResultStatus result, std::string details) {
    QMessageBox::StandardButton answer;
    QString status_message;
    const QString common_message =
        tr("您试图加载的游戏需要卸载来自您的 Switch 的 "
           "其他文件 "
           "开始前.<br/><br/>有关卸载这些文件的详细信息 "
           "请参见下面的wiki页面: <a "
           "href='https://yuzu-emu.org/wiki/"
           "卸载-系统-存档-和-这-共享-字体-from-a-switch-控制台/'>从 "
           "Switch 控制台卸载系统存档和共享字体</a>.<br/><br/>你想退出 "
           "吗 "
           "回到游戏列表上? 持续模拟可能会导致崩溃、损坏保存数据 "
           "或其他bug.");
    switch (result) {
    case Core::System::ResultStatus::ErrorSystemFiles: {
        QString message;
        if (details.empty()) {
            message =
                tr("yuzu 无法找到每一种Switch系统存档. %1").arg(common_message);
        } else {
            message = tr("无法找到一种Switch系统存档: %1. %2")
                          .arg(QString::fromStdString(details), common_message);
        }

        answer = QMessageBox::question(this, tr("系统存档文件未找到"), message,
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        status_message = tr("系统存档文件丢失");
        break;
    }

    case Core::System::ResultStatus::ErrorSharedFont: {
        const QString message =
            tr("yuzu无法找到Switch共享字体. %1").arg(common_message);
        answer = QMessageBox::question(this, tr("共享字体未找到"), message,
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        status_message = tr("共享字体缺失");
        break;
    }

    default:
        answer = QMessageBox::question(
            this, tr("致命错误"),
            tr("yuzu 遇到一个致命错误，请查看日志了解更多详情. "
               "有关访问日志的详细信息，请参阅下面的页面: "
               "<a href='https://community.citra-emu.org/t/how-to-upload-the-log-file/296'>How "
               "to "
               "上传日志文件</a>.<br/><br/>你想退出返回到游戏 "
               "列表? "
               "持续模拟可能会导致崩溃、损坏保存数据 "
               "或其他bug."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        status_message = tr("遇到致命错误");
        break;
    }

    if (answer == QMessageBox::Yes) {
        if (emu_thread) {
            ShutdownGame();

            Settings::RestoreGlobalState();
            UpdateStatusButtons();
        }
    } else {
        // Only show the message if the game is still running.
        if (emu_thread) {
            emu_thread->SetRunning(true);
            message_label->setText(status_message);
        }
    }
}

void GMainWindow::OnReinitializeKeys(ReinitializeKeyBehavior behavior) {
    if (behavior == ReinitializeKeyBehavior::Warning) {
        const auto res = QMessageBox::information(
            this, tr("确认密钥重新确认"),
            tr("您将要强制重新分发所有密钥。 \n如果你不知道什么 "
               "这个 "
               "手段或你在做什么, \n这是潜在的破坏性行动。 "
               "\n请 "
               "确保这是你想要的 \n并选择进行备份。\n\n这将 "
               "删除 "
               "您自动生成的密钥文件，然后重新运行密钥恢复模块。"),
            QMessageBox::StandardButtons{QMessageBox::Ok, QMessageBox::Cancel});

        if (res == QMessageBox::Cancel)
            return;

        FileUtil::Delete(FileUtil::GetUserPath(FileUtil::UserPath::KeysDir) +
                         "prod.keys_autogenerated");
        FileUtil::Delete(FileUtil::GetUserPath(FileUtil::UserPath::KeysDir) +
                         "console.keys_autogenerated");
        FileUtil::Delete(FileUtil::GetUserPath(FileUtil::UserPath::KeysDir) +
                         "title.keys_autogenerated");
    }

    Core::Crypto::KeyManager& keys = Core::Crypto::KeyManager::Instance();
    if (keys.BaseDeriveNecessary()) {
        Core::Crypto::PartitionDataManager pdm{vfs->OpenDirectory(
            FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir), FileSys::Mode::Read)};

        const auto function = [this, &keys, &pdm] {
            keys.PopulateFromPartitionData(pdm);
            Core::System::GetInstance().GetFileSystemController().CreateFactories(*vfs);
            keys.DeriveETicket(pdm);
        };

        QString errors;
        if (!pdm.HasFuses()) {
            errors += tr("缺少保险丝");
        }
        if (!pdm.HasBoot0()) {
            errors += tr(" - 缺少 BOOT0");
        }
        if (!pdm.HasPackage2()) {
            errors += tr(" - 缺少 BCPKG2-1-Normal-Main");
        }
        if (!pdm.HasProdInfo()) {
            errors += tr(" - 缺少 PRODINFO");
        }
        if (!errors.isEmpty()) {
            QMessageBox::warning(
                this, tr("警告缺少推导组件"),
                tr("缺少可能妨碍完成密钥获取的组件. "
                   "<br>请关注 <a href='https://yuzu-emu.org/help/quickstart/'>yuzu "
                   "快速入门指南</a> 得到你所有的钥匙和 "
                   "游戏.<br><br><small>(%1)</small>")
                    .arg(errors));
        }

        QProgressDialog prog;
        prog.setRange(0, 0);
        prog.setLabelText(tr("再生密钥...\n这可能需要长达一分钟 \n取决于 "
                             "系统'的表现."));
        prog.setWindowTitle(tr("获取 Keys"));

        prog.show();

        auto future = QtConcurrent::run(function);
        while (!future.isFinished()) {
            QCoreApplication::processEvents();
        }

        prog.close();
    }

    Core::System::GetInstance().GetFileSystemController().CreateFactories(*vfs);

    if (behavior == ReinitializeKeyBehavior::Warning) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }
}

std::optional<u64> GMainWindow::SelectRomFSDumpTarget(const FileSys::ContentProvider& installed,
                                                      u64 program_id) {
    const auto dlc_entries =
        installed.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);
    std::vector<FileSys::ContentProviderEntry> dlc_match;
    dlc_match.reserve(dlc_entries.size());
    std::copy_if(dlc_entries.begin(), dlc_entries.end(), std::back_inserter(dlc_match),
                 [&program_id, &installed](const FileSys::ContentProviderEntry& entry) {
                     return (entry.title_id & DLC_BASE_TITLE_ID_MASK) == program_id &&
                            installed.GetEntry(entry)->GetStatus() == Loader::ResultStatus::Success;
                 });

    std::vector<u64> romfs_tids;
    romfs_tids.push_back(program_id);
    for (const auto& entry : dlc_match) {
        romfs_tids.push_back(entry.title_id);
    }

    if (romfs_tids.size() > 1) {
        QStringList list{QStringLiteral("Base")};
        for (std::size_t i = 1; i < romfs_tids.size(); ++i) {
            list.push_back(QStringLiteral("DLC %1").arg(romfs_tids[i] & 0x7FF));
        }

        bool ok;
        const auto res = QInputDialog::getItem(
            this, tr("选择RomFS转储目标"),
            tr("请选择您想转储的只读文件系统."), list, 0, false, &ok);
        if (!ok) {
            return {};
        }

        return romfs_tids[list.indexOf(res)];
    }

    return program_id;
}

bool GMainWindow::ConfirmClose() {
    if (emu_thread == nullptr || !UISettings::values.confirm_before_closing)
        return true;

    QMessageBox::StandardButton answer =
        QMessageBox::question(this, tr("yuzu"), tr("你确定要关闭 yuzu?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::closeEvent(QCloseEvent* event) {
    if (!ConfirmClose()) {
        event->ignore();
        return;
    }

    if (!ui.action_Fullscreen->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
    }
    UISettings::values.state = saveState();
#if MICROPROFILE_ENABLED
    UISettings::values.microprofile_geometry = microProfileDialog->saveGeometry();
    UISettings::values.microprofile_visible = microProfileDialog->isVisible();
#endif
    UISettings::values.single_window_mode = ui.action_Single_Window_Mode->isChecked();
    UISettings::values.fullscreen = ui.action_Fullscreen->isChecked();
    UISettings::values.display_titlebar = ui.action_Display_Dock_Widget_Headers->isChecked();
    UISettings::values.show_filter_bar = ui.action_Show_Filter_Bar->isChecked();
    UISettings::values.show_status_bar = ui.action_Show_Status_Bar->isChecked();
    UISettings::values.first_start = false;

    game_list->SaveInterfaceLayout();
    hotkey_registry.SaveHotkeys();

    // Shutdown session if the emu thread is active...
    if (emu_thread != nullptr) {
        ShutdownGame();

        Settings::RestoreGlobalState();
        UpdateStatusButtons();
    }

    render_window->close();

    QWidget::closeEvent(event);
}

static bool IsSingleFileDropEvent(const QMimeData* mime) {
    return mime->hasUrls() && mime->urls().length() == 1;
}

void GMainWindow::AcceptDropEvent(QDropEvent* event) {
    if (IsSingleFileDropEvent(event->mimeData())) {
        event->setDropAction(Qt::DropAction::LinkAction);
        event->accept();
    }
}

bool GMainWindow::DropAction(QDropEvent* event) {
    if (!IsSingleFileDropEvent(event->mimeData())) {
        return false;
    }

    const QMimeData* mime_data = event->mimeData();
    const QString& filename = mime_data->urls().at(0).toLocalFile();

    if (emulation_running && QFileInfo(filename).suffix() == QStringLiteral("bin")) {
        // Amiibo
        LoadAmiibo(filename);
    } else {
        // Game
        if (ConfirmChangeGame()) {
            BootGame(filename);
        }
    }
    return true;
}

void GMainWindow::dropEvent(QDropEvent* event) {
    DropAction(event);
}

void GMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    AcceptDropEvent(event);
}

void GMainWindow::dragMoveEvent(QDragMoveEvent* event) {
    AcceptDropEvent(event);
}

bool GMainWindow::ConfirmChangeGame() {
    if (emu_thread == nullptr)
        return true;

    const auto answer = QMessageBox::question(
        this, tr("yuzu"),
        tr("你确定你要停止模拟？任何未保存的进度将会丢失."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

bool GMainWindow::ConfirmForceLockedExit() {
    if (emu_thread == nullptr)
        return true;

    const auto answer =
        QMessageBox::question(this, tr("yuzu"),
                              tr("当前运行的应用程序已请求yuzu"
                                 "不退出.\n\n你想绕过这一点，并退出呢?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::RequestGameExit() {
    auto& sm{Core::System::GetInstance().ServiceManager()};
    auto applet_oe = sm.GetService<Service::AM::AppletOE>("appletOE");
    auto applet_ae = sm.GetService<Service::AM::AppletAE>("appletAE");
    bool has_signalled = false;

    if (applet_oe != nullptr) {
        applet_oe->GetMessageQueue()->RequestExit();
        has_signalled = true;
    }

    if (applet_ae != nullptr && !has_signalled) {
        applet_ae->GetMessageQueue()->RequestExit();
    }
}

void GMainWindow::filterBarSetChecked(bool state) {
    ui.action_Show_Filter_Bar->setChecked(state);
    emit(OnToggleFilterBar());
}

void GMainWindow::UpdateUITheme() {
    const QString default_icons = QStringLiteral(":/icons/default");
    const QString& current_theme = UISettings::values.theme;
    const bool is_default_theme = current_theme == QString::fromUtf8(UISettings::themes[0].second);
    QStringList theme_paths(default_theme_paths);

    if (is_default_theme || current_theme.isEmpty()) {
        const QString theme_uri(QStringLiteral(":default/style.qss"));
        QFile f(theme_uri);
        if (f.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&f);
            qApp->setStyleSheet(ts.readAll());
            setStyleSheet(ts.readAll());
        } else {
            qApp->setStyleSheet({});
            setStyleSheet({});
        }
        theme_paths.append(default_icons);
        QIcon::setThemeName(default_icons);
    } else {
        const QString theme_uri(QLatin1Char{':'} + current_theme + QStringLiteral("/style.qss"));
        QFile f(theme_uri);
        if (f.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&f);
            qApp->setStyleSheet(ts.readAll());
            setStyleSheet(ts.readAll());
        } else {
            LOG_ERROR(Frontend, "Unable to set style, stylesheet file not found");
        }

        const QString theme_name = QStringLiteral(":/icons/") + current_theme;
        theme_paths.append({default_icons, theme_name});
        QIcon::setThemeName(theme_name);
    }

    QIcon::setThemeSearchPaths(theme_paths);
}

void GMainWindow::LoadTranslation() {
    // If the selected language is English, no need to install any translation
    if (UISettings::values.language == QStringLiteral("en")) {
        return;
    }

    bool loaded;

    if (UISettings::values.language.isEmpty()) {
        // If the selected language is empty, use system locale
        loaded = translator.load(QLocale(), {}, {}, QStringLiteral(":/languages/"));
    } else {
        // Otherwise load from the specified file
        loaded = translator.load(UISettings::values.language, QStringLiteral(":/languages/"));
    }

    if (loaded) {
        qApp->installTranslator(&translator);
    } else {
        UISettings::values.language = QStringLiteral("en");
    }
}

void GMainWindow::OnLanguageChanged(const QString& locale) {
    if (UISettings::values.language != QStringLiteral("en")) {
        qApp->removeTranslator(&translator);
    }

    UISettings::values.language = locale;
    LoadTranslation();
    ui.retranslateUi(this);
    UpdateWindowTitle();

    if (emulation_running)
        ui.action_Start->setText(tr("继续"));
}

void GMainWindow::SetDiscordEnabled([[maybe_unused]] bool state) {
#ifdef USE_DISCORD_PRESENCE
    if (state) {
        discord_rpc = std::make_unique<DiscordRPC::DiscordImpl>();
    } else {
        discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
    }
#else
    discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
#endif
    discord_rpc->Update();
}

#ifdef main
#undef main
#endif

int main(int argc, char* argv[]) {
    Common::DetachedTasks detached_tasks;
    MicroProfileOnThreadCreate("Frontend");
    SCOPE_EXIT({ MicroProfileShutdown(); });

    // Init settings params
    QCoreApplication::setOrganizationName(QStringLiteral("yuzu team"));
    QCoreApplication::setApplicationName(QStringLiteral("yuzu"));

#ifdef __APPLE__
    // If you start a bundle (binary) on OSX without the Terminal, the working directory is "/".
    // But since we require the working directory to be the executable path for the location of
    // the user folder in the Qt Frontend, we need to cd into that working directory
    const std::string bin_path = FileUtil::GetBundleDirectory() + DIR_SEP + "..";
    chdir(bin_path.c_str());
#endif

    // Enables the core to make the qt created contexts current on std::threads
    QCoreApplication::setAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity);
    QApplication app(argc, argv);

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    GMainWindow main_window;
    // After settings have been loaded by GMainWindow, apply the filter
    main_window.show();

    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &main_window,
                     &GMainWindow::OnAppFocusStateChanged);

    int result = app.exec();
    detached_tasks.WaitForAllTasks();
    return result;
}
