#include <SDL.h>
#include <QDebug>
#include <QScopeGuard>
#include <QApplication>

#include "desktop/monitor_window.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("media_server"));
    QCoreApplication::setApplicationName(QStringLiteral("gb_monitor"));
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        qWarning() << "音频输出不可用，仍可静音观看";
    }
    const auto audio_cleanup = qScopeGuard([]() { SDL_Quit(); });
    media_server::monitor_window window;
    window.show();
    return app.exec();
}
