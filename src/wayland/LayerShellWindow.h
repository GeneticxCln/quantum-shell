#pragma once

#include <QMargins>
#include <QQuickWindow>
#include <QRect>
#include <QString>

namespace QuantumShell {

// The QML window a bar, dock or OSD is drawn into.
//
// It carries the per-surface layer-shell configuration the integration reads when it assigns the
// zwlr_layer_surface_v1 role. The surface is not shown until QML calls present(), because the
// protocol forbids attaching a buffer before the compositor has acknowledged the first configure:
// showing the window is what creates the Wayland surface and assigns the role, so every property
// has to be set before it happens.
class LayerShellWindow : public QQuickWindow
{
    Q_OBJECT

    Q_PROPERTY(Layer layer READ layer WRITE setLayer NOTIFY configurationChanged)
    Q_PROPERTY(Anchors anchors READ anchors WRITE setAnchors NOTIFY configurationChanged)
    Q_PROPERTY(int exclusiveZone READ exclusiveZone WRITE setExclusiveZone NOTIFY configurationChanged)
    Q_PROPERTY(KeyboardInteractivity keyboardInteractivity READ keyboardInteractivity
                   WRITE setKeyboardInteractivity NOTIFY configurationChanged)
    Q_PROPERTY(QString layerNamespace READ layerNamespace WRITE setLayerNamespace NOTIFY configurationChanged)
    Q_PROPERTY(QMargins margins READ margins WRITE setMargins NOTIFY configurationChanged)
    Q_PROPERTY(QRect inputRect READ inputRect WRITE setInputRect NOTIFY inputRectChanged)

public:
    // zwlr_layer_shell_v1.layer. The z-order a surface is drawn in, bottom-most first.
    enum Layer { Background = 0, Bottom = 1, Top = 2, Overlay = 3 };
    Q_ENUM(Layer)

    // zwlr_layer_surface_v1.anchor, a bitfield of the edges the surface is pinned to. Anchoring to
    // both opposite edges on an axis makes the surface stretch along it, and the compositor then
    // decides that size rather than the client.
    //
    // The edges are named TopEdge..RightEdge rather than Top..Right because C++ enumerators of one
    // class share a scope with the Layer enum above, where Top and Bottom already mean a z-order.
    enum Anchor { TopEdge = 1, BottomEdge = 2, LeftEdge = 4, RightEdge = 8 };
    Q_DECLARE_FLAGS(Anchors, Anchor)
    Q_FLAG(Anchors)

    enum KeyboardInteractivity { NoKeyboard = 0, ExclusiveKeyboard = 1, OnDemandKeyboard = 2 };
    Q_ENUM(KeyboardInteractivity)

    explicit LayerShellWindow(QWindow *parent = nullptr);

    Layer layer() const { return m_layer; }
    void setLayer(Layer layer);

    Anchors anchors() const { return m_anchors; }
    void setAnchors(Anchors anchors);

    int exclusiveZone() const { return m_exclusiveZone; }
    void setExclusiveZone(int zone);

    KeyboardInteractivity keyboardInteractivity() const { return m_keyboardInteractivity; }
    void setKeyboardInteractivity(KeyboardInteractivity interactivity);

    QString layerNamespace() const { return m_layerNamespace; }
    void setLayerNamespace(const QString &layerNamespace);

    QMargins margins() const { return m_margins; }
    void setMargins(const QMargins &margins);

    // The part of the surface that takes pointer input, in the surface's own coordinates. Anywhere else a click
    // reaches whatever is beneath the surface: the compositor is told through `wl_surface.set_input_region`,
    // which Qt's Wayland backend sends for a window mask. An empty rectangle, the default, is the whole surface,
    // because that is what "no mask" means to Qt and to the protocol's default input region.
    QRect inputRect() const { return m_inputRect; }
    void setInputRect(const QRect &rect);

    // Maps the surface for the first time. Called by `BarHost` rather than from QML, and after that object has
    // assigned the window's screen: showing the window is what creates the Wayland surface and assigns the
    // layer role against an output, and the role is assigned once, so the output has to be right before the
    // window is mapped. QML cannot do that for itself — `screen` is not a QML property of a `QQuickWindow`.
    Q_INVOKABLE void present();

    // Shows or hides the surface after it exists, applying the same namespace refusal present() does.
    // `qsctl bar toggle` reaches a surface through this rather than through `setVisible`, because a bar
    // hidden and shown again has to pass the same check as one shown at startup: showing a surface whose
    // namespace is empty would ask the compositor for a surface the shell cannot claim back, which is the
    // one thing present() refuses.
    void setSurfaceVisible(bool visible);

Q_SIGNALS:
    // Emitted whenever a property above changes, so a live surface can apply the new value rather
    // than needing a restart. The integration connector applies them without a QML reload.
    void configurationChanged();
    void inputRectChanged();

private:
    Layer m_layer = Top;
    Anchors m_anchors;
    int m_exclusiveZone = 0;
    KeyboardInteractivity m_keyboardInteractivity = NoKeyboard;
    QString m_layerNamespace;
    QMargins m_margins;
    QRect m_inputRect;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(LayerShellWindow::Anchors)

} // namespace QuantumShell
