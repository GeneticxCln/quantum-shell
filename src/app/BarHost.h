#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QWindow;

namespace quantum::app {

// The shell's bars: one per output.
//
// A bar is a layer-shell surface, and a surface is created *against an output* — the protocol assigns the
// role to a wl_output, so "one bar" and "one output" are the same statement rather than a choice about how
// wide to draw. A two-monitor session therefore has two bars, and this class is where that multiplicity
// lives: it instantiates the bar component once per screen and keeps the set in step with Qt's own screen
// list, so a monitor plugged in or turned off is a bar appearing or going away without anything here
// polling for it.
//
// It is a class rather than a loop in `main.cpp` because two things need the whole set: the sampling gate,
// which follows whether *any* bar is on screen, and `qsctl bar toggle`, which has to move every one of them.
// Both ask this object rather than keeping a copy of the list, so there is one answer to "which bars exist".
class BarHost : public QObject
{
    Q_OBJECT

public:
    // `barUrl` is the component one bar is made of; the engine must outlive this object. The component is
    // read once and instantiated per screen, so every bar is the same QML the application packs rather than
    // a copy retyped per output.
    BarHost(QQmlEngine& engine, const QUrl& barUrl, QObject* parent = nullptr);

    // Whether the component was readable at all. A bar component that does not load means the shell has no
    // bar to draw, which the composition root turns into a failed process rather than a shell running with
    // nothing on screen. A bar that could not be created for one screen is not this: it is reported against
    // that screen, and the other outputs still get theirs.
    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // The bars in the order their screens arrived. A bar whose surface the compositor has unmapped is still
    // a window here: visibility is the window's own, not a second copy kept alongside it.
    QList<QWindow*> bars() const;

    // Whether any bar is on screen. This is what the sampling gate follows — a reading nobody can see is a
    // wake-up nobody asked for.
    bool anyVisible() const { return anyVisible_; }

    // Hides every bar if any of them is on screen, and shows them all if none is, answering the state they
    // are all in afterwards. A set in mixed states — which only a compositor unmapping one surface can
    // produce — is made uniform rather than left disagreeing with itself.
    bool toggleAll();

Q_SIGNALS:
    void anyVisibleChanged();

private:
    struct Bar
    {
        QPointer<QScreen> screen;
        QPointer<QWindow> window;
    };

    void addForScreen(QScreen* screen);
    void removeForScreen(QScreen* screen);
    void recheckVisibility();

    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QList<Bar> bars_;
    bool anyVisible_ = false;
};

}  // namespace quantum::app
