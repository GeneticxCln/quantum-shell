#include "app/BarHost.h"

#include "app/Logging.h"
#include "wayland/LayerShellWindow.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>
#include <QVariant>
#include <QWindow>

namespace quantum::app {

BarHost::BarHost(QQmlEngine& engine, const QUrl& barUrl, QObject* parent)
    : QObject(parent)
{
    component_ = new QQmlComponent(&engine, barUrl, this);
    if (component_->isError()) {
        // Held rather than logged and dropped: whether a shell with no bar runs at all is the composition
        // root's decision, and it cannot make it without the reason.
        componentError_ = component_->errorString();
        qCCritical(quantum::app::waylandLog) << "the bar component did not load:" << componentError_;
        return;
    }

    for (QScreen* screen : QGuiApplication::screens())
        addForScreen(screen);

    // Qt's own screen list, so a monitor appearing or going away is followed rather than polled for: the set
    // of bars is the set of outputs, and the compositor owns the other half of that statement.
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &BarHost::addForScreen);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &BarHost::removeForScreen);
}

QList<QWindow*> BarHost::bars() const
{
    QList<QWindow*> windows;
    windows.reserve(bars_.size());
    for (const Bar& bar : bars_) {
        if (bar.window != nullptr)
            windows.append(bar.window.data());
    }
    return windows;
}

void BarHost::addForScreen(QScreen* screen)
{
    if (screen == nullptr)
        return;

    // One bar per output, so a second signal for a screen already in the set is not a second bar.
    for (const Bar& bar : bars_) {
        if (bar.screen == screen)
            return;
    }

    // The output's name is handed over as an initial property, because it is the one thing the QML cannot work
    // out for itself: `screen` is not a QML property of a `QQuickWindow`, so a bar cannot look up the output it
    // was placed on. The workspace strip is filtered by it (qml/Workspaces.qml).
    QObject* created = component_->createWithInitialProperties(
        {{QStringLiteral("outputName"), screen->name()}});
    auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(created);
    if (window == nullptr) {
        // The component loaded but did not produce a layer-shell window. That is a defect in the QML rather
        // than a missing output, so it is reported with the screen that exposed it and the rest of the shell
        // carries on — the other outputs are not this one's problem.
        qCWarning(quantum::app::waylandLog)
            << "the bar component did not create a layer-shell window for" << screen->name() << ":"
            << component_->errorString();
        delete created;
        return;
    }

    // The screen is assigned here, and the surface is presented after it rather than while the component was
    // being created. A layer surface is created against an output when the window is mapped and the role is
    // assigned once, so this has to happen first: presenting the window from QML's `Component.onCompleted`
    // would map it against whichever output the compositor picked for a window that had not asked.
    window->setScreen(screen);
    window->present();

    bars_.append(Bar{screen, window});

    // The bar's own visibility, followed rather than mirrored: the compositor's answer is the surface being
    // in or out of its layer list, and a second copy of that here could disagree with it and be believed.
    // `destroyed` is connected as well because a bar can go away without this class asking — the engine owns
    // what it created, and a dangling entry would make `anyVisible()` an answer about a window that is gone.
    connect(window, &QWindow::visibleChanged, this, [this] { recheckVisibility(); });
    connect(window, &QObject::destroyed, this, [this] { recheckVisibility(); });

    qCInfo(quantum::app::waylandLog) << "bar created for output" << screen->name();
    recheckVisibility();
}

void BarHost::removeForScreen(QScreen* screen)
{
    for (int i = 0; i < bars_.size(); ++i) {
        if (bars_.at(i).screen != screen)
            continue;

        // Deleting the window is what destroys the surface. A window kept for an output that no longer
        // exists is one the shell can never place again, and one that would still count towards whether a bar
        // is on screen.
        delete bars_.at(i).window.data();
        bars_.removeAt(i);
        qCInfo(quantum::app::waylandLog) << "bar removed with output"
                                         << (screen != nullptr ? screen->name() : QString());
        recheckVisibility();
        return;
    }
}

bool BarHost::toggleAll()
{
    const bool show = !anyVisible_;
    for (const Bar& bar : bars_) {
        if (auto* window = qobject_cast<QuantumShell::LayerShellWindow*>(bar.window.data()))
            window->setSurfaceVisible(show);
    }

    // Read back rather than assumed: the surface is the compositor's to map, and `qsctl bar toggle` should
    // answer what the bars are rather than what they were asked to be.
    recheckVisibility();
    return anyVisible_;
}

void BarHost::recheckVisibility()
{
    bool any = false;
    for (const Bar& bar : bars_) {
        if (bar.window != nullptr && bar.window->isVisible()) {
            any = true;
            break;
        }
    }

    if (any == anyVisible_)
        return;
    anyVisible_ = any;
    Q_EMIT anyVisibleChanged();
}

}  // namespace quantum::app
