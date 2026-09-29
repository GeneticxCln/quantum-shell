#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QQmlComponent;
class QQmlEngine;
class QScreen;
class QWindow;

namespace quantum::app {

// The surface behind a panel that lets a click outside the panel close it.
//
// A layer-shell surface has no popup grab, so "a click elsewhere" can only be seen by a surface that is there to be
// clicked: a transparent one covering the whole output, on the top layer, one below the panel's overlay layer, so the
// protocol's own layer order puts the panel above it. A press on it emits `dismissed`, and the host that owns the panel answers by
// closing it through the service, the same path Escape takes. It takes no keyboard and reserves nothing.
//
// The three panels that use it (launcher, control centre, notification history) each own one, because each is created
// and destroyed by its own host as its own service's `open` moves. The window comes from `Backdrop.qml`, which
// declares the `dismissed()` signal this class listens for.
class Backdrop : public QObject
{
    Q_OBJECT

public:
    // `engine` must outlive this object. An empty `url` makes a backdrop that never shows, so a host built without
    // one behaves as it did before the backdrop existed.
    explicit Backdrop(QQmlEngine& engine, const QUrl& url, QObject* parent = nullptr);

    bool ready() const { return componentError_.isEmpty(); }
    QString componentError() const { return componentError_; }

    // Creates and maps the surface on `screen`. Does nothing when there is no url or one is already showing.
    // Returns false when the component could not produce a layer-shell window.
    bool show(QScreen* screen);

    // Takes the surface down, later than the call: it can be reached from the surface's own click handler.
    void hide();

    QWindow* window() const;

Q_SIGNALS:
    void dismissed();

private:
    QQmlComponent* component_ = nullptr;
    QString componentError_;
    QPointer<QWindow> window_;
};

}  // namespace quantum::app
